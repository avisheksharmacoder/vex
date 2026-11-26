#include <stdlib.h>
#include <string.h>
#include "vx_batch.h"

vx_status_t vx_batch_init(vx_batch_t *b, uint32_t ncols, const vx_type_t *types) {
    if (!b) return VX_ERR_INVALID;
    memset(b, 0, sizeof(*b));
    if (ncols > 0) {
        b->cols = (vx_vec_t *)calloc(ncols, sizeof(vx_vec_t));
        if (!b->cols) return VX_ERR_OOM;
        b->ncols = ncols;
        b->cap_cols = ncols;
        if (types) {
            for (uint32_t i = 0; i < ncols; ++i) {
                b->cols[i].type = types[i];
                b->cols[i].kind = VX_VEC_FLAT;
            }
        }
    }
    b->sel.count = 0;
    b->sel.is_identity = true;
    return VX_OK;
}

void vx_batch_reset(vx_batch_t *b) {
    if (!b) return;
    if (b->row_src.src_owner) {
        vx_owner_release(b->row_src.src_owner);
        b->row_src.src_owner = NULL;
    }
    b->row_src.src_ctx = NULL;
    b->row_src.fetch_col = NULL;
    b->row_src.row_offset = 0;
    for (uint32_t i = 0; i < b->ncols; ++i) {
        if (b->cols[i].owner) {
            vx_owner_release(b->cols[i].owner);
            b->cols[i].owner = NULL;
        }
    }
    b->sel.count = 0;
    b->sel.is_identity = true;
}

void vx_batch_destroy(vx_batch_t *b) {
    if (!b) return;
    vx_batch_reset(b);
    free(b->cols);
    b->cols = NULL;
    b->ncols = 0;
    b->cap_cols = 0;
}

