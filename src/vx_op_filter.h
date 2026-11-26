#pragma once
#include "vx_op.h"

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_pred_kind : uint8_t {
    VX_PRED_COL_LIT = 0,      /* SIMD fast path: col <cmp> literal */
    VX_PRED_IS_NULL,          /* col == null */
    VX_PRED_IS_NOT_NULL,      /* col != null */
    VX_PRED_BUILTIN_BOOL      /* e.g. startswith(col, "vx_"), contains(col, "err") */
} vx_pred_kind_t;

typedef struct vx_pred_arg {
    bool     is_col;
    uint32_t batch_col_idx;
    uint32_t table_col_idx;
    vx_vec_t lit_vec;         /* VX_VEC_CONST when !is_col */
} vx_pred_arg_t;

typedef struct vx_filter_pred {
    vx_pred_kind_t  kind;
    vx_cmp_op_t     cmp;
    vx_type_t       type;
    bool            negate;          /* used by STR_EQ (!=) and BUILTIN_BOOL (!fn(...)) */
    uint32_t        batch_col_idx;
    uint32_t        table_col_idx;

    /* Literal payload for VX_PRED_COL_LIT */
    union {
        int64_t  i64;
        uint64_t u64;
        double   f64;
        uint8_t  b;
        struct { const uint8_t *ptr; int32_t len; } str;
    } lit;

    /* Built-in payload for VX_PRED_BUILTIN_BOOL */
    vx_builtin_id_t builtin_id;
    uint32_t        nargs;
    vx_pred_arg_t   args[4];
} vx_filter_pred_t;

typedef struct vx_filter_cfg {
    const vx_filter_pred_t *preds;
    uint32_t                npreds;
} vx_filter_cfg_t;

extern const vx_op_vtbl_t VX_OP_FILTER;

