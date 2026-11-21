#ifndef VX_AST_H
#define VX_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *ptr;
    uint32_t    len;
} vx_span_t;

static inline bool vx_span_eq_cstr(vx_span_t s, const char *cstr) {
    if (!cstr) {
        return false;
    }
    size_t n = strlen(cstr);
    return (size_t)s.len == n && (n == 0 || memcmp(s.ptr, cstr, n) == 0);
}

typedef enum {
    VX_EXPR_NULL = 0,
    VX_EXPR_BOOL,
    VX_EXPR_INT,
    VX_EXPR_FLOAT,
    VX_EXPR_STR,
    VX_EXPR_PARAM,
    VX_EXPR_IDENT,
    VX_EXPR_UNARY,
    VX_EXPR_BINARY,
    VX_EXPR_CALL,
    VX_EXPR_LIST
} vx_expr_kind_t;

typedef enum {
    VX_UNOP_NEG = 0,
    VX_UNOP_NOT
} vx_unop_t;

typedef enum {
    VX_BINOP_ADD = 0,
    VX_BINOP_SUB,
    VX_BINOP_MUL,
    VX_BINOP_DIV,
    VX_BINOP_MOD,
    VX_BINOP_EQ,
    VX_BINOP_NE,
    VX_BINOP_LT,
    VX_BINOP_LE,
    VX_BINOP_GT,
    VX_BINOP_GE,
    VX_BINOP_AND,
    VX_BINOP_OR,
    VX_BINOP_IN
} vx_binop_t;

typedef struct vx_expr vx_expr_t;

struct vx_expr {
    vx_expr_kind_t kind;
    union {
        bool      b;
        int64_t   i64;
        double    f64;
        vx_span_t str;
        vx_span_t param;
        struct {
            vx_span_t *parts;
            uint32_t   nparts;
        } ident;
        struct {
            vx_unop_t  op;
            vx_expr_t *operand;
        } unary;
        struct {
            vx_binop_t op;
            vx_expr_t *lhs;
            vx_expr_t *rhs;
        } binary;
        struct {
            vx_span_t   name;
            vx_expr_t **args;
            uint32_t    nargs;
        } call;
        struct {
            vx_expr_t **items;
            uint32_t    nitems;
        } list;
    } u;
};

typedef struct {
    vx_span_t  alias; /* len == 0 if unaliased */
    vx_expr_t *expr;
} vx_proj_item_t;

typedef struct {
    vx_expr_t *expr;
    bool       desc;
} vx_sort_item_t;

typedef enum {
    VX_JOIN_LEFT  = 0, /* +  */
    VX_JOIN_FULL  = 1, /* ++ */
    VX_JOIN_INNER = 2  /* &  */
} vx_join_kind_t;

typedef enum {
    VX_STAGE_PK = 0,
    VX_STAGE_FILTER,
    VX_STAGE_SLICE,
    VX_STAGE_PROJECT,
    VX_STAGE_JOIN,
    VX_STAGE_GROUP,
    VX_STAGE_SORT
} vx_stage_kind_t;

typedef struct {
    vx_stage_kind_t kind;
    union {
        struct {
            vx_expr_t *key;
        } pk;
        struct {
            vx_expr_t *pred;
        } filter;
        struct {
            bool    has_start;
            bool    has_stop;
            int64_t start;
            int64_t stop;
        } slice;
        struct {
            vx_proj_item_t *items;
            uint32_t        nitems;
        } project;
        struct {
            vx_join_kind_t kind;
            vx_span_t      right_table;
            bool           has_on;
            vx_span_t      left_col;
            vx_span_t      right_col;
        } join;
        struct {
            vx_expr_t      **keys;
            uint32_t         nkeys;
            vx_proj_item_t  *aggs;
            uint32_t         naggs;
        } group;
        struct {
            vx_sort_item_t *items;
            uint32_t        nitems;
        } sort;
    } u;
} vx_stage_t;

typedef struct {
    vx_span_t  field;
    vx_expr_t *value;
} vx_field_assign_t;

typedef struct {
    vx_field_assign_t *fields;
    uint32_t           nfields;
} vx_record_t;

typedef struct {
    vx_span_t name;
    vx_span_t type_name;
    bool      is_key;
    bool      has_fk;
    vx_span_t fk_table;
    vx_span_t fk_col;
} vx_col_def_t;

typedef enum {
    VX_STMT_QUERY = 0,
    VX_STMT_INSERT,
    VX_STMT_UPDATE,
    VX_STMT_DELETE,
    VX_STMT_CREATE_TABLE,
    VX_STMT_DROP_TABLE,
    VX_STMT_CREATE_INDEX
} vx_stmt_kind_t;

typedef struct {
    vx_stmt_kind_t kind;
    union {
        struct {
            bool        explain;
            vx_span_t   bind_name; /* len == 0 if unbound */
            vx_span_t   source_table;
            vx_stage_t *stages;
            uint32_t    nstages;
        } query;
        struct {
            vx_span_t    table;
            bool         is_upsert; /* false: +=, true: |= */
            vx_record_t *records;
            uint32_t     nrecords;
        } insert;
        struct {
            vx_span_t   table;
            vx_stage_t  target;    /* PK, FILTER, or SLICE */
            vx_record_t patch;
        } update;
        struct {
            vx_span_t  table;
            vx_stage_t target;     /* PK, FILTER, or SLICE */
        } del;
        struct {
            vx_span_t     table;
            vx_col_def_t *cols;
            uint32_t      ncols;
        } create_table;
        struct {
            vx_span_t table;
        } drop_table;
        struct {
            vx_span_t  table;
            bool       is_unique;
            vx_span_t *cols;
            uint32_t   ncols;
        } create_index;
    } u;
} vx_stmt_t;

typedef struct {
    vx_stmt_t **stmts;
    uint32_t    nstmts;
} vx_program_t;

#endif /* VX_AST_H */

