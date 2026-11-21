#ifndef VX_LEXER_H
#define VX_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VX_TOK_EOF = 0,
    VX_TOK_ERR,
    VX_TOK_NEWLINE,
    VX_TOK_SEMI,

    /* Identifiers & literals */
    VX_TOK_IDENT,
    VX_TOK_INT,
    VX_TOK_FLOAT,
    VX_TOK_STR,
    VX_TOK_PARAM,      /* $name */

    /* 6 structural keywords */
    VX_TOK_KW_BY,
    VX_TOK_KW_SORT,
    VX_TOK_KW_DEL,
    VX_TOK_KW_TABLE,
    VX_TOK_KW_INDEX,
    VX_TOK_KW_UNIQUE,

    /* Expression & literal keywords */
    VX_TOK_KW_AND,
    VX_TOK_KW_OR,
    VX_TOK_KW_NOT,
    VX_TOK_KW_IN,
    VX_TOK_KW_TRUE,
    VX_TOK_KW_FALSE,
    VX_TOK_KW_NULL,

    /* Delimiters */
    VX_TOK_LBRACKET,   /* [ */
    VX_TOK_RBRACKET,   /* ] */
    VX_TOK_LBRACE,     /* { */
    VX_TOK_RBRACE,     /* } */
    VX_TOK_LPAREN,     /* ( */
    VX_TOK_RPAREN,     /* ) */
    VX_TOK_COMMA,      /* , */
    VX_TOK_COLON,      /* : */
    VX_TOK_DOT,        /* . */
    VX_TOK_QUESTION,   /* ? */

    /* Operators */
    VX_TOK_PLUS,       /* +  (left outer join / add) */
    VX_TOK_PLUS_PLUS,  /* ++ (full outer join) */
    VX_TOK_MINUS,      /* -  (desc sort / sub / neg) */
    VX_TOK_STAR,       /* * */
    VX_TOK_SLASH,      /* / */
    VX_TOK_PERCENT,    /* % */
    VX_TOK_AMP,        /* &  (inner join) */
    VX_TOK_ARROW,      /* -> (foreign key) */

    VX_TOK_EQ,         /* = */
    VX_TOK_EQ_EQ,      /* == */
    VX_TOK_BANG_EQ,    /* != */
    VX_TOK_LT,         /* < */
    VX_TOK_LE,         /* <= */
    VX_TOK_GT,         /* > */
    VX_TOK_GE,         /* >= */

    VX_TOK_PLUS_EQ,    /* += (insert) */
    VX_TOK_PIPE_EQ,    /* |= (upsert) */
    VX_TOK_MINUS_EQ    /* -= (delete) */
} vx_tok_kind_t;

typedef struct {
    vx_tok_kind_t kind;
    const char   *ptr;  /* raw slice (quotes/backticks/$ stripped for STR/IDENT/PARAM) */
    uint32_t      len;
    uint32_t      line;
    uint32_t      col;
} vx_tok_t;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    uint32_t    line;
    uint32_t    col;
    bool        has_peek;
    vx_tok_t    peek_tok;
} vx_lexer_t;

void        vx_lexer_init(vx_lexer_t *lx, const char *src, size_t len);
vx_tok_t    vx_lexer_next(vx_lexer_t *lx);
vx_tok_t    vx_lexer_peek(vx_lexer_t *lx);
const char *vx_tok_kind_name(vx_tok_kind_t kind);

#endif /* VX_LEXER_H */

