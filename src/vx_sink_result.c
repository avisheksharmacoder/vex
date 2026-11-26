#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vx_result.h"

extern vx_status_t vx_vec_compact(const vx_vec_t *in, const vx_sel_t *sel,
                                  vx_arena_t *dst_arena, vx_vec_t *out);

#define VX_RES_ARENA_INIT (128u * 1024u)
#define VX_RES_ARENA_MAX  (4u * 1024u * 1024u)

static inline size_t vx_res_elem_size(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return sizeof(uint8_t);
    case VX_TYPE_I64:  return sizeof(int64_t);
    case VX_TYPE_U64:  return sizeof(uint64_t);
    case VX_TYPE_F64:  return sizeof(double);
    case VX_TYPE_STR:  return sizeof(vx_strview_t);
    }
    return 0;
}

/* --- vx_result_t Lifecycle --- */
static void vx_result_destroy(vx_owner_t *owner) {
    vx_result_t *res = (vx_result_t *)owner;
    if (!res) return;

    for (uint32_t b = 0; b < res->nbatches; ++b) {
        if (res->batches[b].cols) {
            for (uint32_t c = 0; c < res->ncols; ++c) {
                vx_owner_release(res->batches[b].cols[c].owner);
            }
            free(res->batches[b].cols);
        }
    }
    free(res->batches);

    if (res->flat_cols && res->flat_ready) {
        for (uint32_t c = 0; c < res->ncols; ++c) {
            if (res->flat_ready[c]) vx_owner_release(res->flat_cols[c].owner);
        }
    }
    free(res->flat_cols);
    free(res->flat_ready);
    if (res->flat_arena) vx_owner_release(&res->flat_arena->hdr);
    pthread_mutex_destroy(&res->flat_mu);

    if (res->col_names) {
        for (uint32_t c = 0; c < res->ncols; ++c) free(res->col_names[c]);
        free(res->col_names);
    }
    free(res->col_types);
    free(res);
}

static vx_result_t *vx_result_create(const vx_result_cfg_t *cfg) {
    vx_result_t *res = (vx_result_t *)calloc(1u, sizeof(*res));
    if (!res) return NULL;

    vx_owner_init(&res->hdr, VX_OWN_RESULT, vx_result_destroy);
    if (pthread_mutex_init(&res->flat_mu, NULL) != 0) {
        free(res);
        return NULL;
    }

    res->ncols      = cfg->ncols;
    res->col_names  = (char **)calloc(cfg->ncols ? cfg->ncols : 1u, sizeof(char *));
    res->col_types  = (vx_type_t *)calloc(cfg->ncols ? cfg->ncols : 1u, sizeof(vx_type_t));
    res->flat_cols  = (vx_vec_t *)calloc(cfg->ncols ? cfg->ncols : 1u, sizeof(vx_vec_t));
    res->flat_ready = (uint8_t *)calloc(cfg->ncols ? cfg->ncols : 1u, sizeof(uint8_t));
    if (!res->col_names || !res->col_types || !res->flat_cols || !res->flat_ready) {
        vx_owner_release(&res->hdr);
        return NULL;
    }

    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        res->col_types[c] = cfg->col_types[c];
        const char *nm    = cfg->col_names[c] ? cfg->col_names[c] : "";
        size_t      len   = strlen(nm);
        res->col_names[c] = (char *)malloc(len + 1u);
        if (!res->col_names[c]) {
            vx_owner_release(&res->hdr);
            return NULL;
        }
        memcpy(res->col_names[c], nm, len + 1u);
    }
    return res;
}

/* --- ResultCollector Global & Per-Worker States --- */
typedef struct vx_res_gstate {
    pthread_mutex_t mu;
    vx_result_t    *res;
} vx_res_gstate_t;

typedef struct vx_res_lstate {
    vx_arena_t     *out_arena;  /* lazily created only if a column requires compaction */
    vx_res_batch_t *batches;
    uint32_t        nbatches;
    uint32_t        cap_batches;
    uint64_t        local_rows;
    uint32_t        ncols;
} vx_res_lstate_t;

static vx_status_t vx_res_init_global(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void **gstate) {
    (void)ctx;
    const vx_result_cfg_t *cfg = (const vx_result_cfg_t *)sk->plan_cfg;
    vx_res_gstate_t       *g   = (vx_res_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;

    if (pthread_mutex_init(&g->mu, NULL) != 0) {
        free(g);
        return VX_ERR_OOM;
    }
    g->res = vx_result_create(cfg);
    if (!g->res) {
        pthread_mutex_destroy(&g->mu);
        free(g);
        return VX_ERR_OOM;
    }
    *gstate = g;
    return VX_OK;
}

static vx_status_t vx_res_init_local(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                     void *gstate, void **lstate) {
    (void)ctx; (void)gstate;
    const vx_result_cfg_t *cfg = (const vx_result_cfg_t *)sk->plan_cfg;
    vx_res_lstate_t       *l   = (vx_res_lstate_t *)calloc(1u, sizeof(*l));
    if (!l) return VX_ERR_OOM;
    l->ncols = cfg->ncols;
    *lstate  = l;
    return VX_OK;
}

/* Zero-copy when dense + flat + backed by immutable (non-scratch) owner; else compact. */
static vx_op_res_t vx_res_sink_consume(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                       void *gstate, void *lstate, vx_batch_t *in) {
    (void)sk; (void)gstate;
    if (in->sel.count == 0) return VX_OP_NEED_MORE_INPUT;

    vx_res_lstate_t *l = (vx_res_lstate_t *)lstate;
    if (l->nbatches == l->cap_batches) {
        uint32_t        ncap = l->cap_batches ? l->cap_batches * 2u : 16u;
        vx_res_batch_t *nb   = (vx_res_batch_t *)realloc(l->batches, (size_t)ncap * sizeof(*nb));
        if (!nb) return VX_OP_OOM;
        l->batches     = nb;
        l->cap_batches = ncap;
    }

    const uint32_t nc   = l->ncols;
    vx_vec_t      *cols = (vx_vec_t *)calloc(nc ? nc : 1u, sizeof(*cols));
    if (!cols) return VX_OP_OOM;

    for (uint32_t c = 0; c < nc; ++c) {
        const vx_vec_t *src = &in->cols[c];
        bool can_zero_copy  = in->sel.is_identity &&
                              src->kind == VX_VEC_FLAT &&
                              src->owner != &ctx->scratch->hdr;
        if (can_zero_copy) {
            cols[c]          = *src;
            cols[c].phys_len = in->sel.count;
            cols[c].owner    = vx_owner_retain(src->owner);
        } else {
            if (!l->out_arena) {
                l->out_arena = vx_arena_create(VX_RES_ARENA_INIT, VX_RES_ARENA_MAX, 0);
                if (!l->out_arena) goto oom;
            }
            vx_status_t st = vx_vec_compact(src, &in->sel, l->out_arena, &cols[c]);
            if (st != VX_OK) goto oom;
        }
    }

    l->batches[l->nbatches].nrows = in->sel.count;
    l->batches[l->nbatches].cols  = cols;
    l->nbatches++;
    l->local_rows += in->sel.count;
    return VX_OP_NEED_MORE_INPUT;

oom:
    for (uint32_t j = 0; j < nc; ++j) vx_owner_release(cols[j].owner);
    free(cols);
    return VX_OP_OOM;
}

static vx_status_t vx_res_combine(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                  void *gstate, void *lstate) {
    (void)sk; (void)ctx;
    vx_res_gstate_t *g = (vx_res_gstate_t *)gstate;
    vx_res_lstate_t *l = (vx_res_lstate_t *)lstate;
    if (l->nbatches == 0) return VX_OK;

    pthread_mutex_lock(&g->mu);
    vx_result_t *res  = g->res;
    uint32_t     need = res->nbatches + l->nbatches;
    if (need > res->cap_batches) {
        uint32_t ncap = res->cap_batches ? res->cap_batches : 16u;
        while (ncap < need) ncap *= 2u;
        vx_res_batch_t *nb = (vx_res_batch_t *)realloc(res->batches, (size_t)ncap * sizeof(*nb));
        if (!nb) {
            pthread_mutex_unlock(&g->mu);
            return VX_ERR_OOM;
        }
        res->batches     = nb;
        res->cap_batches = ncap;
    }

    /* Move batch ownership from worker lstate into global vx_result_t */
    memcpy(&res->batches[res->nbatches], l->batches, (size_t)l->nbatches * sizeof(*l->batches));
    res->nbatches   += l->nbatches;
    res->total_rows += l->local_rows;
    l->nbatches      = 0;
    l->local_rows    = 0;
    pthread_mutex_unlock(&g->mu);
    return VX_OK;
}

static vx_status_t vx_res_finalize(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate) {
    (void)sk; (void)ctx; (void)gstate;
    return VX_OK;
}

static void vx_res_destroy_local(void *lstate) {
    vx_res_lstate_t *l = (vx_res_lstate_t *)lstate;
    if (!l) return;
    /* Only non-zero if aborted before combine() */
    for (uint32_t b = 0; b < l->nbatches; ++b) {
        for (uint32_t c = 0; c < l->ncols; ++c) {
            vx_owner_release(l->batches[b].cols[c].owner);
        }
        free(l->batches[b].cols);
    }
    free(l->batches);
    /* Releasing worker's local handle keeps out_arena alive iff merged columns retained it */
    if (l->out_arena) vx_owner_release(&l->out_arena->hdr);
    free(l);
}

static void vx_res_destroy_global(void *gstate) {
    vx_res_gstate_t *g = (vx_res_gstate_t *)gstate;
    if (!g) return;
    if (g->res) vx_owner_release(&g->res->hdr);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

vx_result_t *vx_result_sink_take(void *sink_gstate) {
    vx_res_gstate_t *g = (vx_res_gstate_t *)sink_gstate;
    if (!g) return NULL;
    vx_result_t *res = g->res;
    g->res = NULL;
    vx_res_destroy_global(g);
    return res;
}

/* --- Lazy Contiguous Column Accessor for PEP 3118 ColumnView --- */
static void vx_copy_validity_offset(uint64_t *dst, uint64_t dst_bit_off,
                                    const uint64_t *src, uint32_t nrows) {
    if (!src) {
        for (uint32_t i = 0; i < nrows; ++i) {
            uint64_t idx = dst_bit_off + i;
            dst[idx >> 6] |= (1ULL << (idx & 63u));
        }
        return;
    }
    if ((dst_bit_off & 63u) == 0) {
        uint32_t full_words = nrows >> 6;
        if (full_words) memcpy(dst + (dst_bit_off >> 6), src, (size_t)full_words * sizeof(uint64_t));
        for (uint32_t i = full_words << 6; i < nrows; ++i) {
            if ((src[i >> 6] >> (i & 63u)) & 1ULL) {
                uint64_t idx = dst_bit_off + i;
                dst[idx >> 6] |= (1ULL << (idx & 63u));
            }
        }
    } else {
        for (uint32_t i = 0; i < nrows; ++i) {
            if ((src[i >> 6] >> (i & 63u)) & 1ULL) {
                uint64_t idx = dst_bit_off + i;
                dst[idx >> 6] |= (1ULL << (idx & 63u));
            }
        }
    }
}

vx_status_t vx_result_get_contiguous_col(vx_result_t *res, uint32_t col_idx, vx_vec_t *out_vec) {
    if (!res || col_idx >= res->ncols || !out_vec) return VX_ERR_INVALID;

    /* Fast path 1: empty result */
    if (res->total_rows == 0 || res->nbatches == 0) {
        memset(out_vec, 0, sizeof(*out_vec));
        out_vec->type = res->col_types[col_idx];
        out_vec->kind = VX_VEC_FLAT;
        return VX_OK;
    }

    /* Fast path 2: single dense batch -> strictly ZERO-COPY (retains chunk/mmap/arena owner) */
    if (res->nbatches == 1u) {
        *out_vec       = res->batches[0].cols[col_idx];
        out_vec->owner = vx_owner_retain(out_vec->owner);
        return VX_OK;
    }

    /* Multi-batch path: stitch once into flat_arena under flat_mu */
    pthread_mutex_lock(&res->flat_mu);
    if (res->flat_ready[col_idx]) {
        *out_vec       = res->flat_cols[col_idx];
        out_vec->owner = vx_owner_retain(out_vec->owner);
        pthread_mutex_unlock(&res->flat_mu);
        return VX_OK;
    }

    if (res->total_rows > UINT32_MAX) {
        pthread_mutex_unlock(&res->flat_mu);
        return VX_ERR_OOM;
    }
    if (!res->flat_arena) {
        res->flat_arena = vx_arena_create(VX_RES_ARENA_INIT, VX_RES_ARENA_MAX, 0);
        if (!res->flat_arena) {
            pthread_mutex_unlock(&res->flat_mu);
            return VX_ERR_OOM;
        }
    }

    const vx_type_t type    = res->col_types[col_idx];
    const size_t    elem_sz = vx_res_elem_size(type);
    const uint64_t  nrows   = res->total_rows;

    bool     any_nulls  = false;
    uint32_t total_bufs = 0;
    for (uint32_t b = 0; b < res->nbatches; ++b) {
        const vx_vec_t *v = &res->batches[b].cols[col_idx];
        if (v->has_nulls) any_nulls = true;
        if (type == VX_TYPE_STR && v->strpool) total_bufs += v->strpool->nbufs;
    }

    uint8_t *dst_data = (uint8_t *)vx_arena_alloc(res->flat_arena, (size_t)nrows * elem_sz);
    uint64_t *dst_val = NULL;
    if (any_nulls) {
        uint64_t nwords = (nrows + 63ULL) >> 6;
        dst_val = (uint64_t *)vx_arena_calloc(res->flat_arena, (size_t)nwords, sizeof(uint64_t));
    }
    if (!dst_data || (any_nulls && !dst_val)) {
        pthread_mutex_unlock(&res->flat_mu);
        return VX_ERR_OOM;
    }

    /* For VX_TYPE_STR, stitch only the 16B vx_strview_t array; share out-of-line bytes zero-copy */
    vx_strpool_t *merged_pool = NULL;
    if (type == VX_TYPE_STR && total_bufs > 0) {
        merged_pool = (vx_strpool_t *)vx_arena_calloc(res->flat_arena, 1u, sizeof(*merged_pool));
        const uint8_t **ptrs = (const uint8_t **)vx_arena_calloc(res->flat_arena, total_bufs, sizeof(*ptrs));
        int64_t        *lens = (int64_t *)vx_arena_calloc(res->flat_arena, total_bufs, sizeof(*lens));
        if (!merged_pool || !ptrs || !lens) {
            pthread_mutex_unlock(&res->flat_mu);
            return VX_ERR_OOM;
        }
        merged_pool->ptrs   = ptrs;
        merged_pool->lens   = lens;
        merged_pool->owners = NULL; /* pinned via &res->hdr below */
        merged_pool->nbufs  = total_bufs;
        merged_pool->cap    = total_bufs;
    }

    uint64_t row_off = 0;
    uint32_t buf_off = 0;
    for (uint32_t b = 0; b < res->nbatches; ++b) {
        const uint32_t  bn = res->batches[b].nrows;
        const vx_vec_t *v  = &res->batches[b].cols[col_idx];

        if (type == VX_TYPE_STR) {
            vx_strview_t       *dsv = (vx_strview_t *)dst_data + row_off;
            const vx_strview_t *ssv = (const vx_strview_t *)v->data;
            for (uint32_t i = 0; i < bn; ++i) {
                vx_strview_t sv = ssv[i];
                if (sv.inl.len > VX_STR_INLINE_MAX) sv.out.buf_idx += (int32_t)buf_off;
                dsv[i] = sv;
            }
            if (v->strpool && merged_pool) {
                for (uint32_t k = 0; k < v->strpool->nbufs; ++k) {
                    merged_pool->ptrs[buf_off + k] = v->strpool->ptrs[k];
                    merged_pool->lens[buf_off + k] = v->strpool->lens[k];
                }
                buf_off += v->strpool->nbufs;
            }
        } else {
            memcpy(dst_data + row_off * elem_sz, v->data, (size_t)bn * elem_sz);
        }

        if (any_nulls) {
            vx_copy_validity_offset(dst_val, row_off, v->has_nulls ? v->validity : NULL, bn);
        }
        row_off += bn;
    }

    vx_vec_t *fc = &res->flat_cols[col_idx];
    memset(fc, 0, sizeof(*fc));
    fc->type      = type;
    fc->kind      = VX_VEC_FLAT;
    fc->has_nulls = any_nulls ? 1u : 0u;
    fc->phys_len  = (uint32_t)nrows;
    fc->data      = dst_data;
    fc->validity  = dst_val;
    fc->strpool   = merged_pool;
    /* Pin flat_arena for fixed-width columns; if strpool aliases batch buffers, caller retains res */
    fc->owner     = vx_owner_retain(&res->flat_arena->hdr);
    res->flat_ready[col_idx] = 1u;

    *out_vec       = *fc;
    out_vec->owner = vx_owner_retain(fc->owner);
    pthread_mutex_unlock(&res->flat_mu);
    return VX_OK;
}

const vx_sink_vtbl_t VX_SINK_RESULT = {
    .name           = "ResultCollector",
    .init_global    = vx_res_init_global,
    .init_local     = vx_res_init_local,
    .sink           = vx_res_sink_consume,
    .combine        = vx_res_combine,
    .finalize       = vx_res_finalize,
    .destroy_local  = vx_res_destroy_local,
    .destroy_global = vx_res_destroy_global
};
