#pragma once
#include "vx_idx.h"
#include "vx_op.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- 1. Logical Plan IR (AST -> Logical Tree -> Rewritten Logical Tree) --- */
#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_lop_kind : uint8_t {
    VX_LOP_SCAN    = 0,
    VX_LOP_FILTER  = 1,
    VX_LOP_PROJECT = 2,
    VX_LOP_JOIN    = 3,
    VX_LOP_AGG     = 4,
    VX_LOP_SORT    = 5,
    VX_LOP_SLICE   = 6
} vx_lop_kind_t;

typedef struct vx_lplan {
    vx_lop_kind_t   kind;
    struct vx_lplan *left;
    struct vx_lplan *right;       /* non-NULL only for VX_LOP_JOIN */
    uint64_t         est_rows;
    uint64_t         required_cols_mask; /* populated by Projection Pushdown pass */
    void            *node_cfg;    /* logical table/predicates/exprs/join/agg/sort/slice */
} vx_lplan_t;

/* --- 2. Physical Access Path & Cost Model --- */
#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_access_path : uint8_t {
    VX_ACCESS_PK_POINT      = 0, /* O(1) Tiered PK lookup (Hot RH + Cold Bloom/RH) */
    VX_ACCESS_SEC_HASH      = 1, /* O(1) Secondary Hash index equality lookup */
    VX_ACCESS_SEC_ART_RANGE = 2, /* O(log N + K) Secondary ART range / prefix scan */
    VX_ACCESS_TABLE_SCAN    = 3  /* Morsel AVX2/NEON SIMD scan + Zone-Map pruning */
} vx_access_path_t;

typedef struct vx_access_choice {
    vx_access_path_t path;
    uint32_t         index_id;          /* valid for SEC_HASH / SEC_ART_RANGE */
    double           est_selectivity;   /* [0.0 .. 1.0] */
    double           est_cost;          /* normalized I/O + CPU cost units */
    uint64_t         est_rows;
    uint32_t         chunks_total;
    uint32_t         chunks_after_zmap; /* chunks surviving zone-map bounds */
} vx_access_choice_t;

/* --- 3. Physical Plan Tree (rendered by `?` explain, compiled to vx_pipeline_t[]) --- */
typedef struct vx_pnode {
    const char         *op_name;        /* "PKLookup", "ARTScan", "TableScan", "HashJoin", ... */
    vx_access_choice_t  access;         /* populated on leaf scan nodes */
    uint64_t            est_rows;
    double              est_cost;
    char                detail[160];    /* e.g. "idx=idx_age [25..40], zmap=12/128 chunks" */
    struct vx_pnode    *left;
    struct vx_pnode    *right;
} vx_pnode_t;

typedef struct vx_plan_result {
    vx_pnode_t    *root;                /* physical tree for `?` explain */
    vx_pipeline_t *pipelines;           /* topological array of M12 pipelines */
    uint32_t       n_pipelines;
    vx_arena_t    *plan_arena;          /* owns all compiled cfg/plan structs */
} vx_plan_result_t;

/* Optimizer Catalog Model */
#define VX_OPT_NAME_MAX 64U

typedef struct vx_opt_index_def {
    bool     is_unique;
    uint32_t ncols;
    uint32_t col_idxs[16];
} vx_opt_index_def_t;

typedef struct vx_opt_table {
    char               name[VX_OPT_NAME_MAX];
    uint32_t           table_id;
    uint32_t           ncols;
    int32_t            pk_col;
    uint8_t            _unused[128];
    uint32_t           nchunks;
    void              *chunks;
    void              *delmaps;
    void              *pk_index;
    uint32_t           nindexes;
    vx_opt_index_def_t indexes[16];
    uint64_t           total_rows;
} vx_opt_table_t;

typedef struct vx_opt_catalog {
    uint32_t          ntables;
    uint32_t          next_table_id;
    vx_opt_table_t  **tables;
} vx_opt_catalog_t;

/* Optimizer entry points */
vx_status_t vx_plan_compile(const void *ast, const void *catalog,
                            vx_plan_result_t *out_plan);
vx_status_t vx_plan_format_explain(const vx_pnode_t *root, char *buf, size_t buf_cap);
void        vx_plan_destroy(vx_plan_result_t *plan);

#ifdef __cplusplus
}
#endif
