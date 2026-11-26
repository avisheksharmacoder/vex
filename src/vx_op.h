#pragma once
#include "vx_batch.h"
#include "vx_simd.h"

typedef struct vx_pool vx_pool_t;

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_op_res : int32_t {
    VX_OP_NEED_MORE_INPUT  = 0, /* operator consumed input and produced 0 or 1 output batch */
    VX_OP_HAVE_MORE_OUTPUT = 1, /* call execute() again with same input (e.g. 1:N HashJoinProbe) */
    VX_OP_FINISHED         = 2, /* early stop upstream (e.g. Slice [:N] limit reached) */
    VX_OP_OOM              = -1,
    VX_OP_ERR              = -2
} vx_op_res_t;

typedef struct vx_err {
    vx_status_t code;
    char        msg[256];
} vx_err_t;

typedef struct vx_exec_ctx {
    uint32_t             worker_id;
    uint32_t             nworkers;
    vx_arena_t          *scratch;       /* rewound after every morsel batch */
    vx_arena_t          *out_arena;     /* long-lived arena for final Result / Sink buffers */
    const vx_simd_ops_t *simd;
    size_t               work_mem_bytes;/* per-operator spill threshold (M15) */
    _Atomic bool        *cancelled;     /* shared across all workers on error or Slice stop */
    vx_err_t            *err;
} vx_exec_ctx_t;

/* --- 1. Source (TableScan, IndexScan, HashAggScan, SortScan) --- */
typedef struct vx_source vx_source_t;
typedef struct vx_source_vtbl {
    const char  *name;
    vx_status_t (*init_global)(const vx_source_t *s, vx_exec_ctx_t *ctx, void **gstate);
    vx_status_t (*init_local)(const vx_source_t *s, vx_exec_ctx_t *ctx, void *gstate, void **lstate);
    /* Returns VX_OP_HAVE_MORE_OUTPUT with populated batch, or VX_OP_FINISHED when exhausted */
    vx_op_res_t (*next_batch)(const vx_source_t *s, vx_exec_ctx_t *ctx,
                              void *gstate, void *lstate, vx_batch_t *out);
    void        (*destroy_local)(void *lstate);
    void        (*destroy_global)(void *gstate);
} vx_source_vtbl_t;

struct vx_source {
    const vx_source_vtbl_t *vtbl;
    const vx_type_t        *out_types;
    uint32_t                out_ncols;
    void                   *plan_cfg;   /* immutable compiled scan/zone-map/projection config */
};

/* --- 2. Streaming Operator (Filter, Project, HashJoinProbe, Slice) --- */
typedef struct vx_op vx_op_t;
typedef struct vx_op_vtbl {
    const char  *name;
    vx_status_t (*init_global)(const vx_op_t *op, vx_exec_ctx_t *ctx, void **gstate);
    vx_status_t (*init_local)(const vx_op_t *op, vx_exec_ctx_t *ctx, void *gstate, void **lstate);
    vx_op_res_t (*execute)(const vx_op_t *op, vx_exec_ctx_t *ctx,
                           void *gstate, void *lstate,
                           vx_batch_t *in, vx_batch_t *out);
    void        (*destroy_local)(void *lstate);
    void        (*destroy_global)(void *gstate);
} vx_op_vtbl_t;

struct vx_op {
    const vx_op_vtbl_t *vtbl;
    const vx_type_t    *out_types;
    uint32_t            out_ncols;
    void               *plan_cfg;       /* immutable AST/bytecode exprs, join keys, slice bounds */
};

/* --- 3. Sink (ResultCollector, HashJoinBuild, HashAggSink, SortSink, TopKSink) --- */
typedef struct vx_sink vx_sink_t;
typedef struct vx_sink_vtbl {
    const char  *name;
    vx_status_t (*init_global)(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void **gstate);
    vx_status_t (*init_local)(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate, void **lstate);
    vx_op_res_t (*sink)(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                        void *gstate, void *lstate, vx_batch_t *in);
    /* Merges thread-local HT/sort runs/result chunks into gstate once worker finishes */
    vx_status_t (*combine)(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate, void *lstate);
    /* Called once on coordinator thread after all workers combine() */
    vx_status_t (*finalize)(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate);
    void        (*destroy_local)(void *lstate);
    void        (*destroy_global)(void *gstate);
} vx_sink_vtbl_t;

struct vx_sink {
    const vx_sink_vtbl_t *vtbl;
    void                 *plan_cfg;
};

/* --- Pipeline container & morsel runner --- */
typedef struct vx_pipeline {
    vx_source_t  source;
    vx_op_t     *ops;
    uint32_t     n_ops;
    vx_sink_t    sink;
} vx_pipeline_t;

/* Standard pipeline operator vtables */
extern const vx_source_vtbl_t VX_SOURCE_TABLE_SCAN;
extern const vx_op_vtbl_t     VX_OP_FILTER;
extern const vx_op_vtbl_t     VX_OP_PROJECT;

/* Executes pipeline across vx_pool_t (falls back to caller thread on trylock contention) */
vx_status_t vx_pipeline_execute(const vx_pipeline_t *pipe, vx_pool_t *pool,
                                vx_exec_ctx_t *root_ctx, void **out_sink_gstate);

