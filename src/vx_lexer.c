#include "vx_lexer.h"

#include <string.h>

static inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool is_ident_cont(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline char cur_char(const vx_lexer_t *lx) {
    return lx->pos < lx->len ? lx->src[lx->pos] : '\0';
}

static inline char peek_char(const vx_lexer_t *lx, size_t off) {
    return (lx->pos + off) < lx->len ? lx->src[lx->pos + off] : '\0';
}

static inline char advance(vx_lexer_t *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static vx_tok_kind_t classify_keyword(const char *p, uint32_t len) {
    switch (len) {
        case 2:
            if (p[0] == 'b' && p[1] == 'y') return VX_TOK_KW_BY;
            if (p[0] == 'o' && p[1] == 'r') return VX_TOK_KW_OR;
            if (p[0] == 'i' && p[1] == 'n') return VX_TOK_KW_IN;
            break;
        case 3:
            if (memcmp(p, "del", 3) == 0) return VX_TOK_KW_DEL;
            if (memcmp(p, "and", 3) == 0) return VX_TOK_KW_AND;
            if (memcmp(p, "not", 3) == 0) return VX_TOK_KW_NOT;
            break;
        case 4:
            if (memcmp(p, "sort", 4) == 0) return VX_TOK_KW_SORT;
            if (memcmp(p, "true", 4) == 0) return VX_TOK_KW_TRUE;
            if (memcmp(p, "null", 4) == 0) return VX_TOK_KW_NULL;
            break;
        case 5:
            if (memcmp(p, "table", 5) == 0) return VX_TOK_KW_TABLE;
            if (memcmp(p, "index", 5) == 0) return VX_TOK_KW_INDEX;
            if (memcmp(p, "false", 5) == 0) return VX_TOK_KW_FALSE;
            break;
        case 6:
            if (memcmp(p, "unique", 6) == 0) return VX_TOK_KW_UNIQUE;
            break;
        default:
            break;
    }
    return VX_TOK_IDENT;
}

static vx_tok_t scan_token(vx_lexer_t *lx) {
    for (;;) {
        while (lx->pos < lx->len) {
            char c = cur_char(lx);
            if (c == ' ' || c == '\t' || c == '\r') {
                advance(lx);
            } else if (c == '#') {
                while (lx->pos < lx->len && cur_char(lx) != '\n') {
                    advance(lx);
                }
            } else {
                break;
            }
        }
        break;
    }

    vx_tok_t tok;
    tok.line = lx->line;
    tok.col = lx->col;
    tok.ptr = lx->src + lx->pos;
    tok.len = 0;

    if (lx->pos >= lx->len) {
        tok.kind = VX_TOK_EOF;
        return tok;
    }

    char c = advance(lx);

    if (c == '\n') {
        tok.kind = VX_TOK_NEWLINE;
        tok.len = 1;
        return tok;
    }

    /* Identifiers & keywords */
    if (is_ident_start(c)) {
        size_t start = lx->pos - 1;
        while (lx->pos < lx->len && is_ident_cont(cur_char(lx))) {
            advance(lx);
        }
        tok.ptr = lx->src + start;
        tok.len = (uint32_t)(lx->pos - start);
        tok.kind = classify_keyword(tok.ptr, tok.len);
        return tok;
    }

    /* Backtick-escaped identifier */
    if (c == '`') {
        size_t start = lx->pos;
        while (lx->pos < lx->len && cur_char(lx) != '`' && cur_char(lx) != '\n') {
            advance(lx);
        }
        if (lx->pos >= lx->len || cur_char(lx) != '`' || lx->pos == start) {
            tok.kind = VX_TOK_ERR;
            return tok;
        }
        tok.ptr = lx->src + start;
        tok.len = (uint32_t)(lx->pos - start);
        advance(lx); /* closing '`' */
        tok.kind = VX_TOK_IDENT;
        return tok;
    }

    /* Parameter: $name */
    if (c == '$') {
        size_t start = lx->pos;
        if (lx->pos >= lx->len || !is_ident_start(cur_char(lx))) {
            tok.kind = VX_TOK_ERR;
            return tok;
        }
        while (lx->pos < lx->len && is_ident_cont(cur_char(lx))) {
            advance(lx);
        }
        tok.ptr = lx->src + start;
        tok.len = (uint32_t)(lx->pos - start);
        tok.kind = VX_TOK_PARAM;
        return tok;
    }

    /* Numbers */
    if (is_digit(c)) {
        size_t start = lx->pos - 1;
        bool is_float = false;
        while (lx->pos < lx->len && is_digit(cur_char(lx))) {
            advance(lx);
        }
        if (cur_char(lx) == '.' && is_digit(peek_char(lx, 1))) {
            is_float = true;
            advance(lx);
            while (lx->pos < lx->len && is_digit(cur_char(lx))) {
                advance(lx);
            }
        }
        if (cur_char(lx) == 'e' || cur_char(lx) == 'E') {
            char next1 = peek_char(lx, 1);
            char next2 = peek_char(lx, 2);
            if (is_digit(next1) ||
                ((next1 == '+' || next1 == '-') && is_digit(next2))) {
                is_float = true;
                advance(lx);
                if (cur_char(lx) == '+' || cur_char(lx) == '-') {
                    advance(lx);
                }
                while (lx->pos < lx->len && is_digit(cur_char(lx))) {
                    advance(lx);
                }
            }
        }
        tok.ptr = lx->src + start;
        tok.len = (uint32_t)(lx->pos - start);
        tok.kind = is_float ? VX_TOK_FLOAT : VX_TOK_INT;
        return tok;
    }

    /* String literals ("..." or '...') */
    if (c == '"' || c == '\'') {
        char quote = c;
        size_t start = lx->pos;
        while (lx->pos < lx->len) {
            char ch = cur_char(lx);
            if (ch == quote) {
                tok.ptr = lx->src + start;
                tok.len = (uint32_t)(lx->pos - start);
                advance(lx);
                tok.kind = VX_TOK_STR;
                return tok;
            }
            if (ch == '\\' && (lx->pos + 1) < lx->len && peek_char(lx, 1) != '\n') {
                advance(lx);
                advance(lx);
                continue;
            }
            if (ch == '\n') {
                tok.kind = VX_TOK_ERR;
                return tok;
            }
            advance(lx);
        }
        tok.kind = VX_TOK_ERR;
        return tok;
    }

    /* Single & multi-char delimiters/operators */
    switch (c) {
        case ';': tok.kind = VX_TOK_SEMI;     tok.len = 1; return tok;
        case '[': tok.kind = VX_TOK_LBRACKET; tok.len = 1; return tok;
        case ']': tok.kind = VX_TOK_RBRACKET; tok.len = 1; return tok;
        case '{': tok.kind = VX_TOK_LBRACE;   tok.len = 1; return tok;
        case '}': tok.kind = VX_TOK_RBRACE;   tok.len = 1; return tok;
        case '(': tok.kind = VX_TOK_LPAREN;   tok.len = 1; return tok;
        case ')': tok.kind = VX_TOK_RPAREN;   tok.len = 1; return tok;
        case ',': tok.kind = VX_TOK_COMMA;    tok.len = 1; return tok;
        case ':': tok.kind = VX_TOK_COLON;    tok.len = 1; return tok;
        case '.': tok.kind = VX_TOK_DOT;      tok.len = 1; return tok;
        case '?': tok.kind = VX_TOK_QUESTION; tok.len = 1; return tok;
        case '*': tok.kind = VX_TOK_STAR;     tok.len = 1; return tok;
        case '/': tok.kind = VX_TOK_SLASH;    tok.len = 1; return tok;
        case '%': tok.kind = VX_TOK_PERCENT;  tok.len = 1; return tok;
        case '&': tok.kind = VX_TOK_AMP;      tok.len = 1; return tok;

        case '+':
            if (cur_char(lx) == '+') {
                advance(lx);
                tok.kind = VX_TOK_PLUS_PLUS;
                tok.len = 2;
            } else if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_PLUS_EQ;
                tok.len = 2;
            } else {
                tok.kind = VX_TOK_PLUS;
                tok.len = 1;
            }
            return tok;

        case '-':
            if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_MINUS_EQ;
                tok.len = 2;
            } else if (cur_char(lx) == '>') {
                advance(lx);
                tok.kind = VX_TOK_ARROW;
                tok.len = 2;
            } else {
                tok.kind = VX_TOK_MINUS;
                tok.len = 1;
            }
            return tok;

        case '|':
            if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_PIPE_EQ;
                tok.len = 2;
                return tok;
            }
            tok.kind = VX_TOK_ERR;
            tok.len = 1;
            return tok;

        case '=':
            if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_EQ_EQ;
                tok.len = 2;
            } else {
                tok.kind = VX_TOK_EQ;
                tok.len = 1;
            }
            return tok;

        case '!':
            if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_BANG_EQ;
                tok.len = 2;
                return tok;
            }
            tok.kind = VX_TOK_ERR;
            tok.len = 1;
            return tok;

        case '<':
            if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_LE;
                tok.len = 2;
            } else {
                tok.kind = VX_TOK_LT;
                tok.len = 1;
            }
            return tok;

        case '>':
            if (cur_char(lx) == '=') {
                advance(lx);
                tok.kind = VX_TOK_GE;
                tok.len = 2;
            } else {
                tok.kind = VX_TOK_GT;
                tok.len = 1;
            }
            return tok;

        default:
            tok.kind = VX_TOK_ERR;
            tok.len = 1;
            return tok;
    }
}

void vx_lexer_init(vx_lexer_t *lx, const char *src, size_t len) {
    if (!lx) {
        return;
    }
    lx->src = src ? src : "";
    lx->len = src ? len : 0;
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
    lx->has_peek = false;
    memset(&lx->peek_tok, 0, sizeof(lx->peek_tok));
}

vx_tok_t vx_lexer_peek(vx_lexer_t *lx) {
    if (!lx->has_peek) {
        lx->peek_tok = scan_token(lx);
        lx->has_peek = true;
    }
    return lx->peek_tok;
}

vx_tok_t vx_lexer_next(vx_lexer_t *lx) {
    if (lx->has_peek) {
        lx->has_peek = false;
        return lx->peek_tok;
    }
    return scan_token(lx);
}

const char *vx_tok_kind_name(vx_tok_kind_t kind) {
    switch (kind) {
        case VX_TOK_EOF:       return "EOF";
        case VX_TOK_ERR:       return "ERR";
        case VX_TOK_NEWLINE:   return "NEWLINE";
        case VX_TOK_SEMI:      return ";";
        case VX_TOK_IDENT:     return "IDENT";
        case VX_TOK_INT:       return "INT";
        case VX_TOK_FLOAT:     return "FLOAT";
        case VX_TOK_STR:       return "STR";
        case VX_TOK_PARAM:     return "PARAM";
        case VX_TOK_KW_BY:     return "by";
        case VX_TOK_KW_SORT:   return "sort";
        case VX_TOK_KW_DEL:    return "del";
        case VX_TOK_KW_TABLE:  return "table";
        case VX_TOK_KW_INDEX:  return "index";
        case VX_TOK_KW_UNIQUE: return "unique";
        case VX_TOK_KW_AND:    return "and";
        case VX_TOK_KW_OR:     return "or";
        case VX_TOK_KW_NOT:    return "not";
        case VX_TOK_KW_IN:     return "in";
        case VX_TOK_KW_TRUE:   return "true";
        case VX_TOK_KW_FALSE:  return "false";
        case VX_TOK_KW_NULL:   return "null";
        case VX_TOK_LBRACKET:  return "[";
        case VX_TOK_RBRACKET:  return "]";
        case VX_TOK_LBRACE:    return "{";
        case VX_TOK_RBRACE:    return "}";
        case VX_TOK_LPAREN:    return "(";
        case VX_TOK_RPAREN:    return ")";
        case VX_TOK_COMMA:     return ",";
        case VX_TOK_COLON:     return ":";
        case VX_TOK_DOT:       return ".";
        case VX_TOK_QUESTION:  return "?";
        case VX_TOK_PLUS:      return "+";
        case VX_TOK_PLUS_PLUS: return "++";
        case VX_TOK_MINUS:     return "-";
        case VX_TOK_STAR:      return "*";
        case VX_TOK_SLASH:     return "/";
        case VX_TOK_PERCENT:   return "%";
        case VX_TOK_AMP:       return "&";
        case VX_TOK_ARROW:     return "->";
        case VX_TOK_EQ:        return "=";
        case VX_TOK_EQ_EQ:     return "==";
        case VX_TOK_BANG_EQ:   return "!=";
        case VX_TOK_LT:        return "<";
        case VX_TOK_LE:        return "<=";
        case VX_TOK_GT:        return ">";
        case VX_TOK_GE:        return ">=";
        case VX_TOK_PLUS_EQ:   return "+=";
        case VX_TOK_PIPE_EQ:   return "|=";
        case VX_TOK_MINUS_EQ:  return "-=";
        default:               return "UNKNOWN";
    }
}

