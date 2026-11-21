#include "vx_parser.h"
#include "vx_lexer.h"

#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VX_MAX_EXPR_DEPTH 128U

typedef struct {
    vx_lexer_t        lx;
    vx_arena_t       *arena;
    vx_parse_result_t *res;
    uint32_t          depth;
    bool              failed;
} vx_parser_t;

static inline vx_span_t tok_span(vx_tok_t t) {
    return (vx_span_t){ .ptr = t.ptr, .len = t.len };
}

static void fail_at(vx_parser_t *p, uint32_t line, uint32_t col, const char *msg) {
    if (p->failed) {
        return;
    }
    p->failed = true;
    p->res->ok = false;
    p->res->err_line = line;
    p->res->err_col = col;
    snprintf(p->res->err_msg, sizeof(p->res->err_msg), "%s", msg ? msg : "syntax error");
}

static void fail_tok(vx_parser_t *p, vx_tok_t t, const char *msg) {
    fail_at(p, t.line, t.col, msg);
}

static void *palloc(vx_parser_t *p, size_t sz, size_t align) {
    void *ptr = vx_arena_alloc(p->arena, sz, align);
    if (!ptr) {
        vx_tok_t t = vx_lexer_peek(&p->lx);
        fail_tok(p, t, "parser arena out of memory");
    }
    return ptr;
}

static vx_tok_t peek_t(vx_parser_t *p) {
    vx_tok_t t = vx_lexer_peek(&p->lx);
    if (t.kind == VX_TOK_ERR) {
        fail_tok(p, t, "invalid or unterminated token");
    }
    return t;
}

static vx_tok_t next_t(vx_parser_t *p) {
    vx_tok_t t = vx_lexer_next(&p->lx);
    if (t.kind == VX_TOK_ERR) {
        fail_tok(p, t, "invalid or unterminated token");
    }
    return t;
}

static bool match_t(vx_parser_t *p, vx_tok_kind_t kind) {
    if (peek_t(p).kind == kind) {
        next_t(p);
        return true;
    }
    return false;
}

static vx_tok_t expect_t(vx_parser_t *p, vx_tok_kind_t kind, const char *msg) {
    vx_tok_t t = peek_t(p);
    if (t.kind != kind) {
        fail_tok(p, t, msg);
        return t;
    }
    return next_t(p);
}

static void skip_newlines(vx_parser_t *p) {
    while (!p->failed && peek_t(p).kind == VX_TOK_NEWLINE) {
        next_t(p);
    }
}

static bool is_stmt_end(vx_tok_kind_t k) {
    return k == VX_TOK_EOF || k == VX_TOK_NEWLINE || k == VX_TOK_SEMI;
}

static bool is_multiline_stage_cont(vx_tok_kind_t k) {
    return k == VX_TOK_PLUS || k == VX_TOK_PLUS_PLUS || k == VX_TOK_AMP ||
           k == VX_TOK_KW_BY || k == VX_TOK_KW_SORT ||
           k == VX_TOK_LBRACKET || k == VX_TOK_LBRACE;
}

/* Peek past newlines without consuming unless a continuation stage follows */
static bool consume_multiline_continuation(vx_parser_t *p) {
    if (peek_t(p).kind != VX_TOK_NEWLINE) {
        return false;
    }
    vx_lexer_t snap = p->lx;
    while (!p->failed && peek_t(p).kind == VX_TOK_NEWLINE) {
        next_t(p);
    }
    if (p->failed) {
        return false;
    }
    if (is_multiline_stage_cont(peek_t(p).kind)) {
        return true;
    }
    p->lx = snap;
    return false;
}

static bool parse_i64_span(vx_tok_t tok, int64_t *out) {
    if (tok.len == 0 || tok.len > 20) {
        return false;
    }
    uint64_t acc = 0;
    for (uint32_t i = 0; i < tok.len; i++) {
        uint64_t d = (uint64_t)(tok.ptr[i] - '0');
        if (acc > ((uint64_t)INT64_MAX - d) / 10ULL) {
            return false;
        }
        acc = acc * 10ULL + d;
    }
    *out = (int64_t)acc;
    return true;
}

static bool parse_f64_span(vx_tok_t tok, double *out) {
    char buf[128];
    if (tok.len == 0 || tok.len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, tok.ptr, tok.len);
    buf[tok.len] = '\0';
    errno = 0;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (errno == ERANGE || end != buf + tok.len) {
        return false;
    }
    *out = v;
    return true;
}

/* ==================== EXPRESSION PARSER ==================== */

static vx_expr_t *parse_expr(vx_parser_t *p);

static vx_expr_t *alloc_expr(vx_parser_t *p, vx_expr_kind_t kind) {
    vx_expr_t *e = (vx_expr_t *)palloc(p, sizeof(vx_expr_t), alignof(vx_expr_t));
    if (!e) {
        return NULL;
    }
    e->kind = kind;
    return e;
}

static vx_span_t unescape_str_tok(vx_parser_t *p, vx_tok_t t) {
    if (t.len == 0 || memchr(t.ptr, '\\', t.len) == NULL) {
        return tok_span(t);
    }
    char *dst = (char *)palloc(p, t.len, 1);
    if (!dst) {
        return (vx_span_t){ NULL, 0 };
    }
    uint32_t out = 0;
    for (uint32_t i = 0; i < t.len; i++) {
        if (t.ptr[i] == '\\' && (i + 1U) < t.len) {
            char esc = t.ptr[++i];
            switch (esc) {
                case 'n':  dst[out++] = '\n'; break;
                case 'r':  dst[out++] = '\r'; break;
                case 't':  dst[out++] = '\t'; break;
                case '\\': dst[out++] = '\\'; break;
                case '"':  dst[out++] = '"';  break;
                case '\'': dst[out++] = '\''; break;
                case '0':  dst[out++] = '\0'; break;
                default:   dst[out++] = esc;  break;
            }
        } else {
            dst[out++] = t.ptr[i];
        }
    }
    return (vx_span_t){ .ptr = dst, .len = out };
}

static vx_expr_t *parse_primary(vx_parser_t *p) {
    vx_tok_t t = peek_t(p);
    if (p->failed) {
        return NULL;
    }

    switch (t.kind) {
        case VX_TOK_KW_NULL:
            next_t(p);
            return alloc_expr(p, VX_EXPR_NULL);

        case VX_TOK_KW_TRUE:
        case VX_TOK_KW_FALSE: {
            next_t(p);
            vx_expr_t *e = alloc_expr(p, VX_EXPR_BOOL);
            if (e) e->u.b = (t.kind == VX_TOK_KW_TRUE);
            return e;
        }

        case VX_TOK_INT: {
            next_t(p);
            int64_t v = 0;
            if (!parse_i64_span(t, &v)) {
                fail_tok(p, t, "integer literal out of range");
                return NULL;
            }
            vx_expr_t *e = alloc_expr(p, VX_EXPR_INT);
            if (e) e->u.i64 = v;
            return e;
        }

        case VX_TOK_FLOAT: {
            next_t(p);
            double v = 0.0;
            if (!parse_f64_span(t, &v)) {
                fail_tok(p, t, "invalid float literal");
                return NULL;
            }
            vx_expr_t *e = alloc_expr(p, VX_EXPR_FLOAT);
            if (e) e->u.f64 = v;
            return e;
        }

        case VX_TOK_STR: {
            next_t(p);
            vx_expr_t *e = alloc_expr(p, VX_EXPR_STR);
            if (e) e->u.str = unescape_str_tok(p, t);
            return e;
        }

        case VX_TOK_PARAM: {
            next_t(p);
            vx_expr_t *e = alloc_expr(p, VX_EXPR_PARAM);
            if (e) e->u.param = tok_span(t);
            return e;
        }

        case VX_TOK_IDENT: {
            next_t(p);
            if (peek_t(p).kind == VX_TOK_LPAREN) {
                next_t(p); /* '(' */
                skip_newlines(p);
                vx_expr_t *stack_args[16];
                typedef struct arg_node { vx_expr_t *val; struct arg_node *next; } arg_node_t;
                arg_node_t *ovf_head = NULL;
                arg_node_t *ovf_tail = NULL;
                uint32_t nargs = 0;

                if (peek_t(p).kind != VX_TOK_RPAREN) {
                    for (;;) {
                        skip_newlines(p);
                        vx_expr_t *arg = parse_expr(p);
                        if (p->failed) return NULL;
                        if (nargs < 16U) {
                            stack_args[nargs] = arg;
                        } else {
                            arg_node_t *nd = (arg_node_t *)palloc(p, sizeof(*nd), alignof(arg_node_t));
                            if (!nd) return NULL;
                            nd->val = arg;
                            nd->next = NULL;
                            if (!ovf_tail) ovf_head = ovf_tail = nd;
                            else { ovf_tail->next = nd; ovf_tail = nd; }
                        }
                        nargs++;
                        skip_newlines(p);
                        if (!match_t(p, VX_TOK_COMMA)) break;
                    }
                }
                expect_t(p, VX_TOK_RPAREN, "expected ')' after function arguments");
                if (p->failed) return NULL;

                vx_expr_t *e = alloc_expr(p, VX_EXPR_CALL);
                if (!e) return NULL;
                e->u.call.name = tok_span(t);
                e->u.call.nargs = nargs;
                if (nargs > 0) {
                    e->u.call.args = (vx_expr_t **)palloc(p, nargs * sizeof(vx_expr_t *), alignof(vx_expr_t *));
                    if (!e->u.call.args) return NULL;
                    uint32_t nstack = nargs < 16U ? nargs : 16U;
                    memcpy(e->u.call.args, stack_args, nstack * sizeof(vx_expr_t *));
                    uint32_t idx = nstack;
                    for (arg_node_t *cur = ovf_head; cur; cur = cur->next) {
                        e->u.call.args[idx++] = cur->val;
                    }
                } else {
                    e->u.call.args = NULL;
                }
                return e;
            }

            /* Dotted identifier chain */
            vx_span_t parts_buf[16];
            uint32_t nparts = 0;
            parts_buf[nparts++] = tok_span(t);
            while (peek_t(p).kind == VX_TOK_DOT) {
                next_t(p);
                vx_tok_t seg = expect_t(p, VX_TOK_IDENT, "expected identifier after '.'");
                if (p->failed) return NULL;
                if (nparts >= 16U) {
                    fail_tok(p, seg, "identifier path too deep");
                    return NULL;
                }
                parts_buf[nparts++] = tok_span(seg);
            }

            vx_expr_t *e = alloc_expr(p, VX_EXPR_IDENT);
            if (!e) return NULL;
            e->u.ident.parts = (vx_span_t *)palloc(p, nparts * sizeof(vx_span_t), alignof(vx_span_t));
            if (!e->u.ident.parts) return NULL;
            memcpy(e->u.ident.parts, parts_buf, nparts * sizeof(vx_span_t));
            e->u.ident.nparts = nparts;
            return e;
        }

        case VX_TOK_LPAREN: {
            next_t(p);
            skip_newlines(p);
            vx_expr_t *inner = parse_expr(p);
            if (p->failed) return NULL;
            skip_newlines(p);
            expect_t(p, VX_TOK_RPAREN, "expected ')'");
            return inner;
        }

        case VX_TOK_LBRACKET: {
            next_t(p);
            skip_newlines(p);
            vx_expr_t *stack_items[16];
            typedef struct item_node { vx_expr_t *val; struct item_node *next; } item_node_t;
            item_node_t *ovf_head = NULL;
            item_node_t *ovf_tail = NULL;
            uint32_t nitems = 0;

            if (peek_t(p).kind != VX_TOK_RBRACKET) {
                for (;;) {
                    skip_newlines(p);
                    if (peek_t(p).kind == VX_TOK_RBRACKET) break;
                    vx_expr_t *it = parse_expr(p);
                    if (p->failed) return NULL;
                    if (nitems < 16U) {
                        stack_items[nitems] = it;
                    } else {
                        item_node_t *nd = (item_node_t *)palloc(p, sizeof(*nd), alignof(item_node_t));
                        if (!nd) return NULL;
                        nd->val = it;
                        nd->next = NULL;
                        if (!ovf_tail) ovf_head = ovf_tail = nd;
                        else { ovf_tail->next = nd; ovf_tail = nd; }
                    }
                    nitems++;
                    skip_newlines(p);
                    if (!match_t(p, VX_TOK_COMMA)) break;
                }
            }
            expect_t(p, VX_TOK_RBRACKET, "expected ']' to close list literal");
            if (p->failed) return NULL;

            vx_expr_t *e = alloc_expr(p, VX_EXPR_LIST);
            if (!e) return NULL;
            e->u.list.nitems = nitems;
            if (nitems > 0) {
                e->u.list.items = (vx_expr_t **)palloc(p, nitems * sizeof(vx_expr_t *), alignof(vx_expr_t *));
                if (!e->u.list.items) return NULL;
                uint32_t nstack = nitems < 16U ? nitems : 16U;
                memcpy(e->u.list.items, stack_items, nstack * sizeof(vx_expr_t *));
                uint32_t idx = nstack;
                for (item_node_t *cur = ovf_head; cur; cur = cur->next) {
                    e->u.list.items[idx++] = cur->val;
                }
            } else {
                e->u.list.items = NULL;
            }
            return e;
        }

        default:
            fail_tok(p, t, "expected expression");
            return NULL;
    }
}

static vx_expr_t *parse_unary(vx_parser_t *p) {
    if (peek_t(p).kind == VX_TOK_MINUS) {
        next_t(p);
        vx_expr_t *op = parse_unary(p);
        if (p->failed) return NULL;
        vx_expr_t *e = alloc_expr(p, VX_EXPR_UNARY);
        if (e) {
            e->u.unary.op = VX_UNOP_NEG;
            e->u.unary.operand = op;
        }
        return e;
    }
    return parse_primary(p);
}

static vx_expr_t *make_binary(vx_parser_t *p, vx_binop_t op, vx_expr_t *lhs, vx_expr_t *rhs) {
    vx_expr_t *e = alloc_expr(p, VX_EXPR_BINARY);
    if (e) {
        e->u.binary.op = op;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = rhs;
    }
    return e;
}

static vx_expr_t *parse_mul(vx_parser_t *p) {
    vx_expr_t *lhs = parse_unary(p);
    while (!p->failed) {
        vx_tok_kind_t k = peek_t(p).kind;
        vx_binop_t op;
        if (k == VX_TOK_STAR) op = VX_BINOP_MUL;
        else if (k == VX_TOK_SLASH) op = VX_BINOP_DIV;
        else if (k == VX_TOK_PERCENT) op = VX_BINOP_MOD;
        else break;
        next_t(p);
        vx_expr_t *rhs = parse_unary(p);
        if (p->failed) return NULL;
        lhs = make_binary(p, op, lhs, rhs);
    }
    return lhs;
}

static vx_expr_t *parse_add(vx_parser_t *p) {
    vx_expr_t *lhs = parse_mul(p);
    while (!p->failed) {
        vx_tok_kind_t k = peek_t(p).kind;
        vx_binop_t op;
        if (k == VX_TOK_PLUS) op = VX_BINOP_ADD;
        else if (k == VX_TOK_MINUS) op = VX_BINOP_SUB;
        else break;
        next_t(p);
        vx_expr_t *rhs = parse_mul(p);
        if (p->failed) return NULL;
        lhs = make_binary(p, op, lhs, rhs);
    }
    return lhs;
}

static vx_expr_t *parse_cmp(vx_parser_t *p) {
    vx_expr_t *lhs = parse_add(p);
    while (!p->failed) {
        vx_tok_kind_t k = peek_t(p).kind;
        vx_binop_t op;
        switch (k) {
            case VX_TOK_EQ_EQ:   op = VX_BINOP_EQ; break;
            case VX_TOK_BANG_EQ: op = VX_BINOP_NE; break;
            case VX_TOK_LT:      op = VX_BINOP_LT; break;
            case VX_TOK_LE:      op = VX_BINOP_LE; break;
            case VX_TOK_GT:      op = VX_BINOP_GT; break;
            case VX_TOK_GE:      op = VX_BINOP_GE; break;
            case VX_TOK_KW_IN:   op = VX_BINOP_IN; break;
            default:             return lhs;
        }
        next_t(p);
        vx_expr_t *rhs = parse_add(p);
        if (p->failed) return NULL;
        lhs = make_binary(p, op, lhs, rhs);
    }
    return lhs;
}

static vx_expr_t *parse_not(vx_parser_t *p) {
    if (peek_t(p).kind == VX_TOK_KW_NOT) {
        next_t(p);
        vx_expr_t *op = parse_not(p);
        if (p->failed) return NULL;
        vx_expr_t *e = alloc_expr(p, VX_EXPR_UNARY);
        if (e) {
            e->u.unary.op = VX_UNOP_NOT;
            e->u.unary.operand = op;
        }
        return e;
    }
    return parse_cmp(p);
}

static vx_expr_t *parse_and(vx_parser_t *p) {
    vx_expr_t *lhs = parse_not(p);
    while (!p->failed && peek_t(p).kind == VX_TOK_KW_AND) {
        next_t(p);
        vx_expr_t *rhs = parse_not(p);
        if (p->failed) return NULL;
        lhs = make_binary(p, VX_BINOP_AND, lhs, rhs);
    }
    return lhs;
}

static vx_expr_t *parse_or(vx_parser_t *p) {
    vx_expr_t *lhs = parse_and(p);
    while (!p->failed && peek_t(p).kind == VX_TOK_KW_OR) {
        next_t(p);
        vx_expr_t *rhs = parse_and(p);
        if (p->failed) return NULL;
        lhs = make_binary(p, VX_BINOP_OR, lhs, rhs);
    }
    return lhs;
}

static vx_expr_t *parse_expr(vx_parser_t *p) {
    if (p->depth >= VX_MAX_EXPR_DEPTH) {
        fail_tok(p, peek_t(p), "expression nesting too deep");
        return NULL;
    }
    p->depth++;
    vx_expr_t *e = parse_or(p);
    p->depth--;
    return e;
}

/* ==================== STAGE & RECORD HELPERS ==================== */

static bool extract_slice_int(const vx_expr_t *e, int64_t *out) {
    if (!e) return false;
    if (e->kind == VX_EXPR_INT) {
        *out = e->u.i64;
        return true;
    }
    if (e->kind == VX_EXPR_UNARY && e->u.unary.op == VX_UNOP_NEG &&
        e->u.unary.operand && e->u.unary.operand->kind == VX_EXPR_INT) {
        *out = -e->u.unary.operand->u.i64;
        return true;
    }
    return false;
}

static bool parse_optional_slice_bound(vx_parser_t *p, bool *has_val, int64_t *out) {
    skip_newlines(p);
    if (peek_t(p).kind == VX_TOK_RBRACKET) {
        *has_val = false;
        *out = 0;
        return true;
    }
    bool neg = match_t(p, VX_TOK_MINUS);
    vx_tok_t t = expect_t(p, VX_TOK_INT, "expected integer slice bound");
    if (p->failed) return false;
    int64_t v = 0;
    if (!parse_i64_span(t, &v)) {
        fail_tok(p, t, "slice bound out of range");
        return false;
    }
    *has_val = true;
    *out = neg ? -v : v;
    return true;
}

static bool parse_bracket_stage(vx_parser_t *p, vx_stage_t *out) {
    expect_t(p, VX_TOK_LBRACKET, "expected '['");
    if (p->failed) return false;
    skip_newlines(p);

    if (peek_t(p).kind == VX_TOK_RBRACKET) {
        fail_tok(p, peek_t(p), "empty '[]' selector is not allowed");
        return false;
    }

    /* [:stop] or [:] */
    if (match_t(p, VX_TOK_COLON)) {
        out->kind = VX_STAGE_SLICE;
        out->u.slice.has_start = false;
        out->u.slice.start = 0;
        if (!parse_optional_slice_bound(p, &out->u.slice.has_stop, &out->u.slice.stop)) {
            return false;
        }
        skip_newlines(p);
        expect_t(p, VX_TOK_RBRACKET, "expected ']'");
        return !p->failed;
    }

    vx_expr_t *first = parse_expr(p);
    if (p->failed) return false;
    skip_newlines(p);

    /* [start:stop] or [start:] */
    if (match_t(p, VX_TOK_COLON)) {
        int64_t start = 0;
        if (!extract_slice_int(first, &start)) {
            fail_tok(p, peek_t(p), "slice start must be an integer literal");
            return false;
        }
        out->kind = VX_STAGE_SLICE;
        out->u.slice.has_start = true;
        out->u.slice.start = start;
        if (!parse_optional_slice_bound(p, &out->u.slice.has_stop, &out->u.slice.stop)) {
            return false;
        }
        skip_newlines(p);
        expect_t(p, VX_TOK_RBRACKET, "expected ']'");
        return !p->failed;
    }

    /* Single expression inside [...] -> PK if literal/param, else FILTER */
    if (match_t(p, VX_TOK_RBRACKET)) {
        if (first->kind == VX_EXPR_INT || first->kind == VX_EXPR_STR ||
            first->kind == VX_EXPR_PARAM) {
            out->kind = VX_STAGE_PK;
            out->u.pk.key = first;
        } else {
            out->kind = VX_STAGE_FILTER;
            out->u.filter.pred = first;
        }
        return true;
    }

    /* Comma-separated predicates -> implicit AND filter */
    if (peek_t(p).kind == VX_TOK_COMMA) {
        vx_expr_t *pred = first;
        while (match_t(p, VX_TOK_COMMA)) {
            skip_newlines(p);
            vx_expr_t *rhs = parse_expr(p);
            if (p->failed) return false;
            pred = make_binary(p, VX_BINOP_AND, pred, rhs);
            if (p->failed) return false;
            skip_newlines(p);
        }
        expect_t(p, VX_TOK_RBRACKET, "expected ']'");
        if (p->failed) return false;
        out->kind = VX_STAGE_FILTER;
        out->u.filter.pred = pred;
        return true;
    }

    fail_tok(p, peek_t(p), "expected ']', ',', or ':' inside '[...]'");
    return false;
}

static bool parse_proj_item(vx_parser_t *p, vx_proj_item_t *out) {
    out->alias = (vx_span_t){ NULL, 0 };
    if (peek_t(p).kind == VX_TOK_IDENT) {
        vx_lexer_t snap = p->lx;
        vx_tok_t id = next_t(p);
        if (!p->failed && peek_t(p).kind == VX_TOK_COLON) {
            next_t(p);
            out->alias = tok_span(id);
        } else {
            p->lx = snap;
        }
    }
    out->expr = parse_expr(p);
    return !p->failed;
}

static bool parse_agg_block(vx_parser_t *p, vx_proj_item_t **out_items, uint32_t *out_n) {
    expect_t(p, VX_TOK_LBRACE, "expected '{'");
    if (p->failed) return false;
    skip_newlines(p);

    vx_proj_item_t stack_buf[16];
    typedef struct agg_node { vx_proj_item_t val; struct agg_node *next; } agg_node_t;
    agg_node_t *ovf_head = NULL;
    agg_node_t *ovf_tail = NULL;
    uint32_t count = 0;

    if (peek_t(p).kind != VX_TOK_RBRACE) {
        for (;;) {
            skip_newlines(p);
            if (peek_t(p).kind == VX_TOK_RBRACE) break;
            vx_proj_item_t item;
            if (!parse_proj_item(p, &item)) return false;
            if (count < 16U) {
                stack_buf[count] = item;
            } else {
                agg_node_t *nd = (agg_node_t *)palloc(p, sizeof(*nd), alignof(agg_node_t));
                if (!nd) return false;
                nd->val = item;
                nd->next = NULL;
                if (!ovf_tail) ovf_head = ovf_tail = nd;
                else { ovf_tail->next = nd; ovf_tail = nd; }
            }
            count++;
            skip_newlines(p);
            if (!match_t(p, VX_TOK_COMMA)) break;
        }
    }
    expect_t(p, VX_TOK_RBRACE, "expected '}'");
    if (p->failed) return false;

    if (count == 0) {
        fail_tok(p, peek_t(p), "empty aggregate block '{}'");
        return false;
    }

    vx_proj_item_t *arr = (vx_proj_item_t *)palloc(p, count * sizeof(vx_proj_item_t),
                                                   alignof(vx_proj_item_t));
    if (!arr) return false;
    uint32_t nstack = count < 16U ? count : 16U;
    memcpy(arr, stack_buf, nstack * sizeof(vx_proj_item_t));
    uint32_t idx = nstack;
    for (agg_node_t *cur = ovf_head; cur; cur = cur->next) {
        arr[idx++] = cur->val;
    }
    *out_items = arr;
    *out_n = count;
    return true;
}

static bool parse_join_col_span(vx_parser_t *p, vx_span_t *out) {
    vx_tok_t first = expect_t(p, VX_TOK_IDENT, "expected join column identifier");
    if (p->failed) return false;
    const char *start = first.ptr;
    const char *end = first.ptr + first.len;
    while (match_t(p, VX_TOK_DOT)) {
        vx_tok_t seg = expect_t(p, VX_TOK_IDENT, "expected column name after '.'");
        if (p->failed) return false;
        end = seg.ptr + seg.len;
    }
    out->ptr = start;
    out->len = (uint32_t)(end - start);
    return true;
}

static bool parse_join_stage(vx_parser_t *p, vx_join_kind_t jkind, vx_stage_t *out) {
    next_t(p); /* consume '+', '++', or '&' */
    vx_tok_t rtbl = expect_t(p, VX_TOK_IDENT, "expected right table name after join operator");
    if (p->failed) return false;

    out->kind = VX_STAGE_JOIN;
    out->u.join.kind = jkind;
    out->u.join.right_table = tok_span(rtbl);
    out->u.join.has_on = false;
    out->u.join.left_col = (vx_span_t){ NULL, 0 };
    out->u.join.right_col = (vx_span_t){ NULL, 0 };

    if (match_t(p, VX_TOK_LPAREN)) {
        out->u.join.has_on = true;
        if (!parse_join_col_span(p, &out->u.join.left_col)) return false;
        if (match_t(p, VX_TOK_EQ_EQ)) {
            if (!parse_join_col_span(p, &out->u.join.right_col)) return false;
        } else {
            out->u.join.right_col = out->u.join.left_col;
        }
        expect_t(p, VX_TOK_RPAREN, "expected ')' after join key");
    }
    return !p->failed;
}

static bool parse_group_stage(vx_parser_t *p, vx_stage_t *out) {
    out->kind = VX_STAGE_GROUP;
    out->u.group.keys = NULL;
    out->u.group.nkeys = 0;
    out->u.group.aggs = NULL;
    out->u.group.naggs = 0;

    if (match_t(p, VX_TOK_KW_BY)) {
        vx_expr_t *stack_keys[16];
        typedef struct key_node { vx_expr_t *val; struct key_node *next; } key_node_t;
        key_node_t *ovf_head = NULL;
        key_node_t *ovf_tail = NULL;
        uint32_t nkeys = 0;

        for (;;) {
            vx_expr_t *k = parse_expr(p);
            if (p->failed) return false;
            if (nkeys < 16U) {
                stack_keys[nkeys] = k;
            } else {
                key_node_t *nd = (key_node_t *)palloc(p, sizeof(*nd), alignof(key_node_t));
                if (!nd) return false;
                nd->val = k;
                nd->next = NULL;
                if (!ovf_tail) ovf_head = ovf_tail = nd;
                else { ovf_tail->next = nd; ovf_tail = nd; }
            }
            nkeys++;
            if (!match_t(p, VX_TOK_COMMA)) break;
        }

        out->u.group.keys = (vx_expr_t **)palloc(p, nkeys * sizeof(vx_expr_t *),
                                                 alignof(vx_expr_t *));
        if (!out->u.group.keys) return false;
        uint32_t nstack = nkeys < 16U ? nkeys : 16U;
        memcpy(out->u.group.keys, stack_keys, nstack * sizeof(vx_expr_t *));
        uint32_t idx = nstack;
        for (key_node_t *cur = ovf_head; cur; cur = cur->next) {
            out->u.group.keys[idx++] = cur->val;
        }
        out->u.group.nkeys = nkeys;

        if (peek_t(p).kind == VX_TOK_LBRACE) {
            return parse_agg_block(p, &out->u.group.aggs, &out->u.group.naggs);
        }
        return true;
    }

    return parse_agg_block(p, &out->u.group.aggs, &out->u.group.naggs);
}

static bool parse_sort_stage(vx_parser_t *p, vx_stage_t *out) {
    next_t(p); /* consume 'sort' */
    vx_sort_item_t stack_buf[16];
    typedef struct sort_node { vx_sort_item_t val; struct sort_node *next; } sort_node_t;
    sort_node_t *ovf_head = NULL;
    sort_node_t *ovf_tail = NULL;
    uint32_t count = 0;

    for (;;) {
        bool desc = match_t(p, VX_TOK_MINUS);
        vx_expr_t *e = parse_expr(p);
        if (p->failed) return false;
        vx_sort_item_t it = { .expr = e, .desc = desc };
        if (count < 16U) {
            stack_buf[count] = it;
        } else {
            sort_node_t *nd = (sort_node_t *)palloc(p, sizeof(*nd), alignof(sort_node_t));
            if (!nd) return false;
            nd->val = it;
            nd->next = NULL;
            if (!ovf_tail) ovf_head = ovf_tail = nd;
            else { ovf_tail->next = nd; ovf_tail = nd; }
        }
        count++;
        if (!match_t(p, VX_TOK_COMMA)) break;
    }

    out->kind = VX_STAGE_SORT;
    out->u.sort.nitems = count;
    out->u.sort.items = (vx_sort_item_t *)palloc(p, count * sizeof(vx_sort_item_t),
                                                 alignof(vx_sort_item_t));
    if (!out->u.sort.items) return false;
    uint32_t nstack = count < 16U ? count : 16U;
    memcpy(out->u.sort.items, stack_buf, nstack * sizeof(vx_sort_item_t));
    uint32_t idx = nstack;
    for (sort_node_t *cur = ovf_head; cur; cur = cur->next) {
        out->u.sort.items[idx++] = cur->val;
    }
    return true;
}

static bool parse_project_stage(vx_parser_t *p, vx_stage_t *out) {
    vx_proj_item_t stack_buf[16];
    typedef struct proj_node { vx_proj_item_t val; struct proj_node *next; } proj_node_t;
    proj_node_t *ovf_head = NULL;
    proj_node_t *ovf_tail = NULL;
    uint32_t count = 0;

    for (;;) {
        vx_proj_item_t item;
        if (!parse_proj_item(p, &item)) return false;
        if (count < 16U) {
            stack_buf[count] = item;
        } else {
            proj_node_t *nd = (proj_node_t *)palloc(p, sizeof(*nd), alignof(proj_node_t));
            if (!nd) return false;
            nd->val = item;
            nd->next = NULL;
            if (!ovf_tail) ovf_head = ovf_tail = nd;
            else { ovf_tail->next = nd; ovf_tail = nd; }
        }
        count++;
        if (!match_t(p, VX_TOK_COMMA)) break;
    }

    out->kind = VX_STAGE_PROJECT;
    out->u.project.nitems = count;
    out->u.project.items = (vx_proj_item_t *)palloc(p, count * sizeof(vx_proj_item_t),
                                                    alignof(vx_proj_item_t));
    if (!out->u.project.items) return false;
    uint32_t nstack = count < 16U ? count : 16U;
    memcpy(out->u.project.items, stack_buf, nstack * sizeof(vx_proj_item_t));
    uint32_t idx = nstack;
    for (proj_node_t *cur = ovf_head; cur; cur = cur->next) {
        out->u.project.items[idx++] = cur->val;
    }
    return true;
}

static bool parse_record(vx_parser_t *p, vx_record_t *out) {
    expect_t(p, VX_TOK_LBRACE, "expected '{' to start record");
    if (p->failed) return false;
    skip_newlines(p);

    vx_field_assign_t stack_buf[16];
    typedef struct fld_node { vx_field_assign_t val; struct fld_node *next; } fld_node_t;
    fld_node_t *ovf_head = NULL;
    fld_node_t *ovf_tail = NULL;
    uint32_t count = 0;

    if (peek_t(p).kind != VX_TOK_RBRACE) {
        for (;;) {
            skip_newlines(p);
            if (peek_t(p).kind == VX_TOK_RBRACE) break;
            vx_tok_t fname = expect_t(p, VX_TOK_IDENT, "expected field name in record");
            if (p->failed) return false;
            expect_t(p, VX_TOK_COLON, "expected ':' after field name");
            if (p->failed) return false;
            vx_expr_t *val = parse_expr(p);
            if (p->failed) return false;

            vx_field_assign_t fa = { .field = tok_span(fname), .value = val };
            if (count < 16U) {
                stack_buf[count] = fa;
            } else {
                fld_node_t *nd = (fld_node_t *)palloc(p, sizeof(*nd), alignof(fld_node_t));
                if (!nd) return false;
                nd->val = fa;
                nd->next = NULL;
                if (!ovf_tail) ovf_head = ovf_tail = nd;
                else { ovf_tail->next = nd; ovf_tail = nd; }
            }
            count++;
            skip_newlines(p);
            if (!match_t(p, VX_TOK_COMMA)) break;
        }
    }
    expect_t(p, VX_TOK_RBRACE, "expected '}' to close record");
    if (p->failed) return false;

    out->nfields = count;
    if (count > 0) {
        out->fields = (vx_field_assign_t *)palloc(p, count * sizeof(vx_field_assign_t),
                                                  alignof(vx_field_assign_t));
        if (!out->fields) return false;
        uint32_t nstack = count < 16U ? count : 16U;
        memcpy(out->fields, stack_buf, nstack * sizeof(vx_field_assign_t));
        uint32_t idx = nstack;
        for (fld_node_t *cur = ovf_head; cur; cur = cur->next) {
            out->fields[idx++] = cur->val;
        }
    } else {
        out->fields = NULL;
    }
    return true;
}

/* ==================== STATEMENT PARSERS ==================== */

static vx_stmt_t *alloc_stmt(vx_parser_t *p, vx_stmt_kind_t kind) {
    vx_stmt_t *s = (vx_stmt_t *)palloc(p, sizeof(vx_stmt_t), alignof(vx_stmt_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    return s;
}

static vx_stmt_t *parse_create_table(vx_parser_t *p) {
    next_t(p); /* 'table' */
    vx_tok_t tname = expect_t(p, VX_TOK_IDENT, "expected table name");
    if (p->failed) return NULL;
    expect_t(p, VX_TOK_LBRACE, "expected '{' after table name");
    if (p->failed) return NULL;
    skip_newlines(p);

    vx_col_def_t stack_cols[32];
    typedef struct col_node { vx_col_def_t val; struct col_node *next; } col_node_t;
    col_node_t *ovf_head = NULL;
    col_node_t *ovf_tail = NULL;
    uint32_t ncols = 0;

    while (!p->failed && peek_t(p).kind != VX_TOK_RBRACE) {
        vx_tok_t cname = expect_t(p, VX_TOK_IDENT, "expected column name");
        if (p->failed) return NULL;
        expect_t(p, VX_TOK_COLON, "expected ':' after column name");
        if (p->failed) return NULL;
        vx_tok_t ctype = expect_t(p, VX_TOK_IDENT, "expected column type");
        if (p->failed) return NULL;

        vx_col_def_t cd;
        memset(&cd, 0, sizeof(cd));
        cd.name = tok_span(cname);
        cd.type_name = tok_span(ctype);

        for (;;) {
            vx_tok_t pk = peek_t(p);
            if (pk.kind == VX_TOK_IDENT && pk.len == 3 && memcmp(pk.ptr, "key", 3) == 0) {
                next_t(p);
                cd.is_key = true;
            } else if (pk.kind == VX_TOK_ARROW) {
                next_t(p);
                vx_tok_t ft = expect_t(p, VX_TOK_IDENT, "expected foreign table name after '->'");
                if (p->failed) return NULL;
                expect_t(p, VX_TOK_DOT, "expected '.' in foreign key target");
                if (p->failed) return NULL;
                vx_tok_t fc = expect_t(p, VX_TOK_IDENT, "expected foreign column name");
                if (p->failed) return NULL;
                cd.has_fk = true;
                cd.fk_table = tok_span(ft);
                cd.fk_col = tok_span(fc);
            } else {
                break;
            }
        }

        if (ncols < 32U) {
            stack_cols[ncols] = cd;
        } else {
            col_node_t *nd = (col_node_t *)palloc(p, sizeof(*nd), alignof(col_node_t));
            if (!nd) return NULL;
            nd->val = cd;
            nd->next = NULL;
            if (!ovf_tail) ovf_head = ovf_tail = nd;
            else { ovf_tail->next = nd; ovf_tail = nd; }
        }
        ncols++;

        bool had_sep = match_t(p, VX_TOK_COMMA);
        if (peek_t(p).kind == VX_TOK_NEWLINE) {
            skip_newlines(p);
            had_sep = true;
        }
        if (!had_sep && peek_t(p).kind != VX_TOK_RBRACE) {
            fail_tok(p, peek_t(p), "expected ',' or '}' in table definition");
            return NULL;
        }
    }

    expect_t(p, VX_TOK_RBRACE, "expected '}' to close table definition");
    if (p->failed) return NULL;
    if (ncols == 0) {
        fail_tok(p, tname, "table definition must declare at least one column");
        return NULL;
    }

    vx_stmt_t *s = alloc_stmt(p, VX_STMT_CREATE_TABLE);
    if (!s) return NULL;
    s->u.create_table.table = tok_span(tname);
    s->u.create_table.ncols = ncols;
    s->u.create_table.cols = (vx_col_def_t *)palloc(p, ncols * sizeof(vx_col_def_t),
                                                    alignof(vx_col_def_t));
    if (!s->u.create_table.cols) return NULL;
    uint32_t nstack = ncols < 32U ? ncols : 32U;
    memcpy(s->u.create_table.cols, stack_cols, nstack * sizeof(vx_col_def_t));
    uint32_t idx = nstack;
    for (col_node_t *cur = ovf_head; cur; cur = cur->next) {
        s->u.create_table.cols[idx++] = cur->val;
    }
    return s;
}

static vx_stmt_t *parse_create_index(vx_parser_t *p, bool is_unique) {
    next_t(p); /* 'index' or 'unique' */
    vx_tok_t tname = expect_t(p, VX_TOK_IDENT, "expected table name");
    if (p->failed) return NULL;
    expect_t(p, VX_TOK_LPAREN, "expected '(' after table name");
    if (p->failed) return NULL;

    vx_span_t stack_cols[16];
    uint32_t ncols = 0;
    for (;;) {
        vx_tok_t c = expect_t(p, VX_TOK_IDENT, "expected index column name");
        if (p->failed) return NULL;
        if (ncols >= 16U) {
            fail_tok(p, c, "too many columns in index");
            return NULL;
        }
        stack_cols[ncols++] = tok_span(c);
        if (!match_t(p, VX_TOK_COMMA)) break;
    }
    expect_t(p, VX_TOK_RPAREN, "expected ')' after index columns");
    if (p->failed) return NULL;

    vx_stmt_t *s = alloc_stmt(p, VX_STMT_CREATE_INDEX);
    if (!s) return NULL;
    s->u.create_index.table = tok_span(tname);
    s->u.create_index.is_unique = is_unique;
    s->u.create_index.ncols = ncols;
    s->u.create_index.cols = (vx_span_t *)palloc(p, ncols * sizeof(vx_span_t), alignof(vx_span_t));
    if (!s->u.create_index.cols) return NULL;
    memcpy(s->u.create_index.cols, stack_cols, ncols * sizeof(vx_span_t));
    return s;
}

static vx_stmt_t *parse_del_stmt(vx_parser_t *p) {
    next_t(p); /* 'del' */
    if (match_t(p, VX_TOK_KW_TABLE)) {
        vx_tok_t tname = expect_t(p, VX_TOK_IDENT, "expected table name after 'del table'");
        if (p->failed) return NULL;
        vx_stmt_t *s = alloc_stmt(p, VX_STMT_DROP_TABLE);
        if (s) s->u.drop_table.table = tok_span(tname);
        return s;
    }

    vx_tok_t tname = expect_t(p, VX_TOK_IDENT, "expected table name after 'del'");
    if (p->failed) return NULL;
    if (peek_t(p).kind != VX_TOK_LBRACKET) {
        fail_tok(p, peek_t(p), "del requires '[...]' selector; use 'del table[:]' to truncate");
        return NULL;
    }

    vx_stage_t target;
    if (!parse_bracket_stage(p, &target)) return NULL;

    vx_stmt_t *s = alloc_stmt(p, VX_STMT_DELETE);
    if (!s) return NULL;
    s->u.del.table = tok_span(tname);
    s->u.del.target = target;
    return s;
}

static vx_stmt_t *parse_insert_stmt(vx_parser_t *p, vx_span_t table, bool is_upsert) {
    next_t(p); /* '+=' or '|=' */
    vx_stmt_t *s = alloc_stmt(p, VX_STMT_INSERT);
    if (!s) return NULL;
    s->u.insert.table = table;
    s->u.insert.is_upsert = is_upsert;

    if (peek_t(p).kind == VX_TOK_LBRACE) {
        vx_record_t *rec = (vx_record_t *)palloc(p, sizeof(vx_record_t), alignof(vx_record_t));
        if (!rec || !parse_record(p, rec)) return NULL;
        s->u.insert.records = rec;
        s->u.insert.nrecords = 1;
        return s;
    }

    if (match_t(p, VX_TOK_LBRACKET)) {
        skip_newlines(p);
        vx_record_t stack_recs[16];
        typedef struct rec_node { vx_record_t val; struct rec_node *next; } rec_node_t;
        rec_node_t *ovf_head = NULL;
        rec_node_t *ovf_tail = NULL;
        uint32_t count = 0;

        while (!p->failed && peek_t(p).kind != VX_TOK_RBRACKET) {
            vx_record_t rec;
            if (!parse_record(p, &rec)) return NULL;
            if (count < 16U) {
                stack_recs[count] = rec;
            } else {
                rec_node_t *nd = (rec_node_t *)palloc(p, sizeof(*nd), alignof(rec_node_t));
                if (!nd) return NULL;
                nd->val = rec;
                nd->next = NULL;
                if (!ovf_tail) ovf_head = ovf_tail = nd;
                else { ovf_tail->next = nd; ovf_tail = nd; }
            }
            count++;
            skip_newlines(p);
            if (!match_t(p, VX_TOK_COMMA)) break;
            skip_newlines(p);
        }

        expect_t(p, VX_TOK_RBRACKET, "expected ']' after record batch");
        if (p->failed) return NULL;
        if (count == 0) {
            fail_tok(p, peek_t(p), "empty record batch '[]'");
            return NULL;
        }

        s->u.insert.records = (vx_record_t *)palloc(p, count * sizeof(vx_record_t),
                                                    alignof(vx_record_t));
        if (!s->u.insert.records) return NULL;
        uint32_t nstack = count < 16U ? count : 16U;
        memcpy(s->u.insert.records, stack_recs, nstack * sizeof(vx_record_t));
        uint32_t idx = nstack;
        for (rec_node_t *cur = ovf_head; cur; cur = cur->next) {
            s->u.insert.records[idx++] = cur->val;
        }
        s->u.insert.nrecords = count;
        return s;
    }

    fail_tok(p, peek_t(p), "expected '{...}' or '[{...}]' after insert operator");
    return NULL;
}

static vx_stmt_t *parse_pipeline_or_mutation(vx_parser_t *p, bool explain,
                                             vx_span_t bind_name, vx_span_t source_table) {
    vx_stage_t stack_stages[16];
    typedef struct st_node { vx_stage_t val; struct st_node *next; } st_node_t;
    st_node_t *ovf_head = NULL;
    st_node_t *ovf_tail = NULL;
    uint32_t nstages = 0;

    for (;;) {
        if (p->failed) return NULL;
        vx_tok_t pk = peek_t(p);
        if (p->failed) return NULL;

        if (pk.kind == VX_TOK_NEWLINE) {
            if (!consume_multiline_continuation(p)) {
                break;
            }
            pk = peek_t(p);
            if (p->failed) return NULL;
        }

        /* Mutation suffixes allowed only immediately after table[...] */
        if (!explain && bind_name.len == 0 && nstages == 1 &&
            (stack_stages[0].kind == VX_STAGE_PK ||
             stack_stages[0].kind == VX_STAGE_FILTER ||
             stack_stages[0].kind == VX_STAGE_SLICE)) {
            if (pk.kind == VX_TOK_EQ) {
                next_t(p);
                vx_stmt_t *s = alloc_stmt(p, VX_STMT_UPDATE);
                if (!s) return NULL;
                s->u.update.table = source_table;
                s->u.update.target = stack_stages[0];
                if (!parse_record(p, &s->u.update.patch)) return NULL;
                return s;
            }
            if (pk.kind == VX_TOK_MINUS_EQ) {
                next_t(p);
                vx_stmt_t *s = alloc_stmt(p, VX_STMT_DELETE);
                if (!s) return NULL;
                s->u.del.table = source_table;
                s->u.del.target = stack_stages[0];
                return s;
            }
        }

        vx_stage_t st;
        bool parsed = false;

        switch (pk.kind) {
            case VX_TOK_LBRACKET:
                parsed = parse_bracket_stage(p, &st);
                break;
            case VX_TOK_PLUS:
                parsed = parse_join_stage(p, VX_JOIN_LEFT, &st);
                break;
            case VX_TOK_PLUS_PLUS:
                parsed = parse_join_stage(p, VX_JOIN_FULL, &st);
                break;
            case VX_TOK_AMP:
                parsed = parse_join_stage(p, VX_JOIN_INNER, &st);
                break;
            case VX_TOK_KW_BY:
            case VX_TOK_LBRACE:
                parsed = parse_group_stage(p, &st);
                break;
            case VX_TOK_KW_SORT:
                parsed = parse_sort_stage(p, &st);
                break;
            case VX_TOK_IDENT:
            case VX_TOK_LPAREN:
                parsed = parse_project_stage(p, &st);
                break;
            default:
                goto done_stages;
        }

        if (!parsed || p->failed) return NULL;

        if (nstages < 16U) {
            stack_stages[nstages] = st;
        } else {
            st_node_t *nd = (st_node_t *)palloc(p, sizeof(*nd), alignof(st_node_t));
            if (!nd) return NULL;
            nd->val = st;
            nd->next = NULL;
            if (!ovf_tail) ovf_head = ovf_tail = nd;
            else { ovf_tail->next = nd; ovf_tail = nd; }
        }
        nstages++;
    }

done_stages:
    ;
    vx_stmt_t *s = alloc_stmt(p, VX_STMT_QUERY);
    if (!s) return NULL;
    s->u.query.explain = explain;
    s->u.query.bind_name = bind_name;
    s->u.query.source_table = source_table;
    s->u.query.nstages = nstages;

    if (nstages > 0) {
        s->u.query.stages = (vx_stage_t *)palloc(p, nstages * sizeof(vx_stage_t),
                                                 alignof(vx_stage_t));
        if (!s->u.query.stages) return NULL;
        uint32_t nstack = nstages < 16U ? nstages : 16U;
        memcpy(s->u.query.stages, stack_stages, nstack * sizeof(vx_stage_t));
        uint32_t idx = nstack;
        for (st_node_t *cur = ovf_head; cur; cur = cur->next) {
            s->u.query.stages[idx++] = cur->val;
        }
    }
    return s;
}

static vx_stmt_t *parse_statement(vx_parser_t *p) {
    vx_tok_t t = peek_t(p);
    if (p->failed) return NULL;

    switch (t.kind) {
        case VX_TOK_KW_TABLE:
            return parse_create_table(p);
        case VX_TOK_KW_INDEX:
            return parse_create_index(p, false);
        case VX_TOK_KW_UNIQUE:
            return parse_create_index(p, true);
        case VX_TOK_KW_DEL:
            return parse_del_stmt(p);
        case VX_TOK_QUESTION: {
            next_t(p);
            vx_tok_t src = expect_t(p, VX_TOK_IDENT, "expected table name after '?'");
            if (p->failed) return NULL;
            return parse_pipeline_or_mutation(p, true, (vx_span_t){ NULL, 0 }, tok_span(src));
        }
        case VX_TOK_IDENT: {
            vx_tok_t id = next_t(p);
            vx_tok_t nxt = peek_t(p);
            if (p->failed) return NULL;

            if (nxt.kind == VX_TOK_PLUS_EQ) {
                return parse_insert_stmt(p, tok_span(id), false);
            }
            if (nxt.kind == VX_TOK_PIPE_EQ) {
                return parse_insert_stmt(p, tok_span(id), true);
            }
            if (nxt.kind == VX_TOK_MINUS_EQ) {
                fail_tok(p, nxt, "delete requires '[...]' selector; use 'table[:] -=' to truncate");
                return NULL;
            }
            if (nxt.kind == VX_TOK_EQ) {
                next_t(p);
                if (peek_t(p).kind == VX_TOK_LBRACE) {
                    fail_tok(p, peek_t(p), "update requires '[...]' selector; use 'table[:] = {...}'");
                    return NULL;
                }
                vx_tok_t src = expect_t(p, VX_TOK_IDENT, "expected source table after '='");
                if (p->failed) return NULL;
                return parse_pipeline_or_mutation(p, false, tok_span(id), tok_span(src));
            }
            return parse_pipeline_or_mutation(p, false, (vx_span_t){ NULL, 0 }, tok_span(id));
        }
        default:
            fail_tok(p, t, "unexpected token at start of statement");
            return NULL;
    }
}

bool vx_parse(const char *src, size_t len, vx_arena_t *arena, vx_parse_result_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!src || !arena) {
        snprintf(out->err_msg, sizeof(out->err_msg), "invalid arguments to vx_parse");
        return false;
    }

    vx_parser_t p;
    vx_lexer_init(&p.lx, src, len);
    p.arena = arena;
    p.res = out;
    p.depth = 0;
    p.failed = false;

    vx_stmt_t *stack_stmts[16];
    typedef struct stmt_node { vx_stmt_t *val; struct stmt_node *next; } stmt_node_t;
    stmt_node_t *ovf_head = NULL;
    stmt_node_t *ovf_tail = NULL;
    uint32_t nstmts = 0;

    for (;;) {
        while (!p.failed) {
            vx_tok_kind_t k = peek_t(&p).kind;
            if (k == VX_TOK_NEWLINE || k == VX_TOK_SEMI) {
                next_t(&p);
            } else {
                break;
            }
        }
        if (p.failed) return false;
        if (peek_t(&p).kind == VX_TOK_EOF) break;

        vx_stmt_t *st = parse_statement(&p);
        if (p.failed || !st) return false;

        vx_tok_t term = peek_t(&p);
        if (p.failed) return false;
        if (!is_stmt_end(term.kind)) {
            fail_tok(&p, term, "unexpected trailing token in statement");
            return false;
        }

        if (nstmts < 16U) {
            stack_stmts[nstmts] = st;
        } else {
            stmt_node_t *nd = (stmt_node_t *)palloc(&p, sizeof(*nd), alignof(stmt_node_t));
            if (!nd) return false;
            nd->val = st;
            nd->next = NULL;
            if (!ovf_tail) ovf_head = ovf_tail = nd;
            else { ovf_tail->next = nd; ovf_tail = nd; }
        }
        nstmts++;
    }

    out->program.nstmts = nstmts;
    if (nstmts > 0) {
        out->program.stmts = (vx_stmt_t **)palloc(&p, nstmts * sizeof(vx_stmt_t *),
                                                  alignof(vx_stmt_t *));
        if (!out->program.stmts) return false;
        uint32_t nstack = nstmts < 16U ? nstmts : 16U;
        memcpy(out->program.stmts, stack_stmts, nstack * sizeof(vx_stmt_t *));
        uint32_t idx = nstack;
        for (stmt_node_t *cur = ovf_head; cur; cur = cur->next) {
            out->program.stmts[idx++] = cur->val;
        }
    }

    out->ok = true;
    return true;
}

