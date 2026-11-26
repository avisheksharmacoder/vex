#pragma once
#include <pthread.h>
#include "vx_batch.h"
#include "vx_op.h"

typedef struct vx_res_batch {
    uint32_t  nrows;
    vx_vec_t *cols; /* array[ncols], all dense VX_VEC_FLAT [0..nrows-1] */
} vx_res_batch_t;

typedef struct vx_result {
    vx_owner_t       hdr;            /* refcounted by Python Result & Arrow C Stream */
    uint32_t         ncols;
    uint64_t         total_rows;
    char           **col_names;      /* deep-copied schema names */
    vx_type_t       *col_types;

    /* Zero-copy batch array (consumed directly by M18 __arrow_c_stream__) */
    vx_res_batch_t  *batches;
    uint32_t         nbatches;
    uint32_t         cap_batches;

    /* Lazy contiguous column cache for V1 PEP 3118 ColumnView / col.np() */
    pthread_mutex_t  flat_mu;        /* guards lazy stitching under free-threaded 3.13t */
    vx_arena_t      *flat_arena;     /* allocated on demand only if nbatches > 1 */
    vx_vec_t        *flat_cols;      /* array[ncols]; flat_ready[c] == 1 once populated */
    uint8_t         *flat_ready;
} vx_result_t;

typedef struct vx_result_cfg {
    uint32_t           ncols;
    const char *const *col_names;
    const vx_type_t   *col_types;
} vx_result_cfg_t;

/* Transfers ownership of vx_result_t* out of finalized ResultCollector gstate */
vx_result_t *vx_result_sink_take(void *sink_gstate);

/* Returns a contiguous 1D column vector (zero-copy when nbatches <= 1; lazy stitch otherwise).
 * Caller MUST call vx_owner_release(out_vec->owner) when releasing the view. */
vx_status_t  vx_result_get_contiguous_col(vx_result_t *res, uint32_t col_idx, vx_vec_t *out_vec);

extern const vx_sink_vtbl_t VX_SINK_RESULT;

