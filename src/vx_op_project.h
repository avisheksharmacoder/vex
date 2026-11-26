#pragma once
#include "vx_op.h"

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_proj_kind : uint8_t {
    VX_PROJ_COL     = 0, /* direct column pass-through (zero-copy when possible) */
    VX_PROJ_LITERAL = 1, /* constant scalar (VX_VEC_CONST) */
    VX_PROJ_BUILTIN = 2  /* computed expression via vx_builtin_eval() */
} vx_proj_kind_t;

typedef struct vx_proj_arg {
    bool     is_col;
    uint32_t batch_col_idx;
    uint32_t table_col_idx;
    vx_vec_t lit_vec;
} vx_proj_arg_t;

typedef struct vx_proj_item {
    vx_proj_kind_t  kind;
    uint32_t        batch_col_idx;   /* VX_PROJ_COL */
    uint32_t        table_col_idx;   /* VX_PROJ_COL */
    vx_vec_t        lit_vec;         /* VX_PROJ_LITERAL (owner == NULL) */

    vx_builtin_id_t builtin_id;      /* VX_PROJ_BUILTIN */
    uint32_t        nargs;
    vx_proj_arg_t   args[4];
} vx_proj_item_t;

typedef struct vx_project_cfg {
    const vx_proj_item_t *items;
    uint32_t              n_items;
    bool                  has_computed; /* precomputed by planner: any VX_PROJ_BUILTIN */
} vx_project_cfg_t;

extern const vx_op_vtbl_t VX_OP_PROJECT;

