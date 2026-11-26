#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vx_op.h"
#include "vx_enc.h"
#include "vx_seg_fmt.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

/* --- Zone-Map Predicate Descriptor --- */
#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_zpred_kind : uint8_t {
    VX_ZPRED_CMP = 0,
    VX_ZPRED_IS_NULL,
    VX_ZPRED_IS_NOT_NULL
} vx_zpred_kind_t;

typedef struct vx_zone_pred {
    uint32_t        table_col_idx;
    vx_type_t       type;
    vx_zpred_kind_t kind;
    vx_cmp_op_t     cmp;
    union {
        int64_t  i64;
        uint64_t u64;
        double   f64;
        uint8_t  str4[4]; /* zero-padded first 4 bytes of RHS literal */
    } rhs;
} vx_zone_pred_t;

/* --- Immutable Plan Config & Shared Morsel State --- */
typedef struct vx_scan_cfg {
    vx_owner_t               *catalog_owner; /* pins immutable catalog snapshot */
    const vx_chunk_t  *const *chunks;        /* array[nchunks] */
    const vx_delmap_t *const *delmaps;       /* array[nchunks], entries may be NULL */
    uint32_t                  nchunks;

    uint32_t                  out_ncols;     /* columns projected out of scan */
    const uint32_t           *col_ids;       /* out col b -> table col col_ids[b] */
    const uint8_t            *eager_cols;    /* 1 = bind in Scan (WHERE); 0 = DEFERRED */

    const vx_zone_pred_t     *zone_preds;    /* conjunctive (AND) chunk-skip predicates */
    uint32_t                  n_zone_preds;
} vx_scan_cfg_t;

typedef struct vx_scan_gstate {
    _Atomic uint32_t next_chunk;
    _Atomic uint64_t chunks_pruned;          /* exposed in ? explain / telemetry */
    _Atomic uint64_t chunks_scanned;
    vx_owner_t      *catalog_owner;
} vx_scan_gstate_t;

/* --- Zone-Map Pruning (returns true iff chunk is PROVEN to contain 0 matches) --- */
static bool vx_zone_can_prune(const vx_chunk_t *chunk, const vx_zone_pred_t *preds, uint32_t npreds) {
    for (uint32_t p = 0; p < npreds; ++p) {
        const vx_zone_pred_t *pr = &preds[p];
        const vx_zone_t      *z  = &chunk->cols[pr->table_col_idx].zone;

        if (pr->kind == VX_ZPRED_IS_NULL) {
            if (z->null_count == 0) return true;
            continue;
        }
        if (pr->kind == VX_ZPRED_IS_NOT_NULL) {
            if (!z->has_bounds) return true; /* all rows are NULL */
            continue;
        }
        if (!z->has_bounds) return true; /* NULL fails every comparison op */

        switch (pr->type) {
        case VX_TYPE_I64: {
            int64_t lo = z->min_val.i64, hi = z->max_val.i64, v = pr->rhs.i64;
            if ((pr->cmp == VX_CMP_EQ && (v < lo || v > hi)) ||
                (pr->cmp == VX_CMP_NE && (lo == v && hi == v)) ||
                (pr->cmp == VX_CMP_LT && (lo >= v)) ||
                (pr->cmp == VX_CMP_LE && (lo >  v)) ||
                (pr->cmp == VX_CMP_GT && (hi <= v)) ||
                (pr->cmp == VX_CMP_GE && (hi <  v))) return true;
            break;
        }
        case VX_TYPE_U64:
        case VX_TYPE_BOOL: {
            uint64_t lo = z->min_val.u64, hi = z->max_val.u64, v = pr->rhs.u64;
            if ((pr->cmp == VX_CMP_EQ && (v < lo || v > hi)) ||
                (pr->cmp == VX_CMP_NE && (lo == v && hi == v)) ||
                (pr->cmp == VX_CMP_LT && (lo >= v)) ||
                (pr->cmp == VX_CMP_LE && (lo >  v)) ||
                (pr->cmp == VX_CMP_GT && (hi <= v)) ||
                (pr->cmp == VX_CMP_GE && (hi <  v))) return true;
            break;
        }
        case VX_TYPE_F64: {
            double lo = z->min_val.f64, hi = z->max_val.f64, v = pr->rhs.f64;
            if (isnan(v)) return (pr->cmp != VX_CMP_NE);
            if ((pr->cmp == VX_CMP_EQ && (v < lo || v > hi)) ||
                (pr->cmp == VX_CMP_NE && (lo == v && hi == v)) ||
                (pr->cmp == VX_CMP_LT && (lo >= v)) ||
                (pr->cmp == VX_CMP_LE && (lo >  v)) ||
                (pr->cmp == VX_CMP_GT && (hi <= v)) ||
                (pr->cmp == VX_CMP_GE && (hi <  v))) return true;
            break;
        }
        case VX_TYPE_STR: {
            /* 4-byte prefix bounds: prune ONLY on strict inequality to stay exact
             * when strings longer than 4B share the boundary prefix. */
            int cmp_min = memcmp(z->min_val.str4, pr->rhs.str4, 4u);
            int cmp_max = memcmp(z->max_val.str4, pr->rhs.str4, 4u);
            if ((pr->cmp == VX_CMP_EQ && (cmp_min > 0 || cmp_max < 0)) ||
                ((pr->cmp == VX_CMP_LT || pr->cmp == VX_CMP_LE) && cmp_min > 0) ||
                ((pr->cmp == VX_CMP_GT || pr->cmp == VX_CMP_GE) && cmp_max < 0)) {
                return true;
            }
            break;
        }
        }
    }
    return false;
}

/* --- Zero-Copy Column Binding Helper --- */
static vx_status_t vx_bind_col_chunk(const vx_chunk_t *chunk, uint32_t table_col_idx,
                                     const vx_sel_t *sel, vx_arena_t *scratch,
                                     vx_vec_t *out_vec) {
    (void)sel;
    const vx_col_chunk_t *cc = &chunk->cols[table_col_idx];

    if (cc->encoding != VX_ENC_PLAIN) {
        if (!chunk->backing) return VX_ERR_INVALID;
        const vx_seg_t *seg = (const vx_seg_t *)chunk->backing;
        const vx_seg_col_desc_t *cd = &seg->col_dir[table_col_idx];
        uint32_t k = (uint32_t)(chunk->chunk_id & 0xFFFFFFFFULL);
        const vx_seg_chunk_meta_t *cm =
            (const vx_seg_chunk_meta_t *)(seg->mmap_base + cd->chunk_meta_off) + k;

        vx_enc_col_ctx_t ec = {
            .page_data  = cc->data,
            .page_len   = cm->page_len,
            .validity   = cc->has_nulls ? cc->validity : NULL,
            .dict_vals  = cd->dict_off ? (const void *)(seg->mmap_base + cd->dict_off) : NULL,
            .dict_count = cd->dict_count,
            .strpool    = cc->strpool ? cc->strpool : ((cd->type == VX_TYPE_STR && seg->strpool.nbufs > 0) ? &seg->strpool : NULL),
            .type       = cc->type,
            .encoding   = cc->encoding,
            .has_nulls  = cc->has_nulls,
            .nrows      = chunk->nrows
        };
        return vx_enc_decode_col_chunk(&ec, scratch, out_vec);
    }

    out_vec->type      = cc->type;
    out_vec->kind      = VX_VEC_FLAT;
    out_vec->has_nulls = cc->has_nulls;
    out_vec->phys_len  = chunk->nrows;
    out_vec->dict_size = 0;
    out_vec->data      = cc->data;
    out_vec->dict_vals = NULL;
    out_vec->validity  = cc->has_nulls ? cc->validity : NULL;
    out_vec->strpool   = cc->strpool;
    /* Pin backing segment (M13 mmap) or heap chunk directly */
    out_vec->owner     = vx_owner_retain(chunk->backing ? chunk->backing
                                                        : (vx_owner_t *)&chunk->hdr);
    return VX_OK;
}

/* --- Late Materialization Callback (invoked by Project / HashJoinProbe / HashAgg) --- */
static vx_status_t vx_scan_fetch_col(void *src_ctx, uint32_t table_col_idx,
                                     const vx_sel_t *sel, vx_arena_t *scratch,
                                     vx_vec_t *out_vec) {
    const vx_chunk_t *chunk = (const vx_chunk_t *)src_ctx;
    return vx_bind_col_chunk(chunk, table_col_idx, sel, scratch, out_vec);
}

vx_status_t vx_batch_materialize_col(vx_batch_t *b, uint32_t batch_col_idx,
                                     uint32_t table_col_idx, vx_arena_t *scratch) {
    vx_vec_t *vec = &b->cols[batch_col_idx];
    if (vec->kind != VX_VEC_DEFERRED) return VX_OK;

    /* 0 surviving rows after WHERE -> never touch cold column pages */
    if (b->sel.count == 0) {
        vec->kind      = VX_VEC_FLAT;
        vec->phys_len  = 0;
        vec->data      = NULL;
        vec->validity  = NULL;
        vec->has_nulls = 0;
        vec->owner     = NULL;
        return VX_OK;
    }

    if (!b->row_src.fetch_col) return VX_ERR_INVALID;
    return b->row_src.fetch_col(b->row_src.src_ctx, table_col_idx,
                                &b->sel, scratch, vec);
}

/* --- Optional M13 Cold mmap Lookahead Prefetch (eager columns only) --- */
static inline void vx_scan_prefetch_cold(const vx_scan_cfg_t *cfg, uint32_t next_idx) {
#if defined(MADV_WILLNEED)
    if (next_idx >= cfg->nchunks) return;
    const vx_chunk_t *nxt = cfg->chunks[next_idx];
    if (nxt->tier != VX_TIER_COLD_MMAP || nxt->nrows == 0) return;

    for (uint32_t b = 0; b < cfg->out_ncols; ++b) {
        if (!cfg->eager_cols[b]) continue;
        const vx_col_chunk_t *cc = &nxt->cols[cfg->col_ids[b]];
        if (cc->encoding != VX_ENC_PLAIN || !cc->data) continue;
        size_t elem_sz = (cc->type == VX_TYPE_BOOL) ? 1u
                       : (cc->type == VX_TYPE_STR)  ? sizeof(vx_strview_t) : 8u;
        (void)madvise((void *)cc->data, (size_t)nxt->nrows * elem_sz, MADV_WILLNEED);
    }
#else
    (void)cfg; (void)next_idx;
#endif
}

/* --- vx_source_vtbl_t Implementation --- */
static vx_status_t vx_scan_init_global(const vx_source_t *s, vx_exec_ctx_t *ctx, void **gstate) {
    (void)ctx;
    const vx_scan_cfg_t *cfg = (const vx_scan_cfg_t *)s->plan_cfg;
    vx_scan_gstate_t    *g   = (vx_scan_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;

    atomic_init(&g->next_chunk, 0);
    atomic_init(&g->chunks_pruned, 0);
    atomic_init(&g->chunks_scanned, 0);
    g->catalog_owner = vx_owner_retain(cfg->catalog_owner);
    *gstate = g;
    return VX_OK;
}

static vx_status_t vx_scan_init_local(const vx_source_t *s, vx_exec_ctx_t *ctx,
                                      void *gstate, void **lstate) {
    (void)s; (void)ctx; (void)gstate;
    *lstate = NULL;
    return VX_OK;
}

static vx_op_res_t vx_scan_next_batch(const vx_source_t *s, vx_exec_ctx_t *ctx,
                                      void *gstate, void *lstate, vx_batch_t *out) {
    (void)lstate;
    const vx_scan_cfg_t *cfg = (const vx_scan_cfg_t *)s->plan_cfg;
    vx_scan_gstate_t    *g   = (vx_scan_gstate_t *)gstate;

    for (;;) {
        if (atomic_load_explicit(ctx->cancelled, memory_order_relaxed)) {
            return VX_OP_FINISHED;
        }

        uint32_t idx = atomic_fetch_add_explicit(&g->next_chunk, 1u, memory_order_relaxed);
        if (idx >= cfg->nchunks) return VX_OP_FINISHED;

        const vx_chunk_t  *chunk = cfg->chunks[idx];
        const vx_delmap_t *dm    = cfg->delmaps ? cfg->delmaps[idx] : NULL;

        if (chunk->nrows == 0 || (dm && dm->live_count == 0)) {
            continue;
        }
        if (cfg->n_zone_preds > 0 &&
            vx_zone_can_prune(chunk, cfg->zone_preds, cfg->n_zone_preds)) {
            atomic_fetch_add_explicit(&g->chunks_pruned, 1u, memory_order_relaxed);
            continue;
        }

        /* Initialize selection vector from deletion bitmap */
        if (!dm || dm->live_count == chunk->nrows) {
            out->sel.count       = chunk->nrows;
            out->sel.is_identity = true;
        } else {
            uint32_t live = vx_delmap_live_sel(dm, chunk->nrows, out->sel.idx);
            if (live == 0) continue;
            out->sel.count       = live;
            out->sel.is_identity = false;
        }

        /* Attach row_src so downstream operators can resolve VX_VEC_DEFERRED columns */
        out->row_src.src_ctx    = (void *)chunk;
        out->row_src.src_owner  = vx_owner_retain(chunk->backing ? chunk->backing
                                                                 : (vx_owner_t *)&chunk->hdr);
        out->row_src.fetch_col  = vx_scan_fetch_col;
        out->row_src.row_offset = 0;

        /* Bind eager (WHERE) columns zero-copy; leave payload columns DEFERRED */
        for (uint32_t b = 0; b < cfg->out_ncols; ++b) {
            uint32_t tc = cfg->col_ids[b];
            if (cfg->eager_cols[b]) {
                vx_status_t st = vx_bind_col_chunk(chunk, tc, &out->sel,
                                                   ctx->scratch, &out->cols[b]);
                if (st != VX_OK) {
                    if (ctx->err) ctx->err->code = st;
                    return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
                }
            } else {
                vx_vec_t *v = &out->cols[b];
                memset(v, 0, sizeof(*v));
                v->type = chunk->cols[tc].type;
                v->kind = VX_VEC_DEFERRED;
            }
        }

        vx_scan_prefetch_cold(cfg, idx + 1u);
        atomic_fetch_add_explicit(&g->chunks_scanned, 1u, memory_order_relaxed);
        return VX_OP_HAVE_MORE_OUTPUT;
    }
}

static void vx_scan_destroy_local(void *lstate) { (void)lstate; }

static void vx_scan_destroy_global(void *gstate) {
    vx_scan_gstate_t *g = (vx_scan_gstate_t *)gstate;
    if (!g) return;
    vx_owner_release(g->catalog_owner);
    free(g);
}

const vx_source_vtbl_t VX_SOURCE_TABLE_SCAN = {
    .name           = "TableScan",
    .init_global    = vx_scan_init_global,
    .init_local     = vx_scan_init_local,
    .next_batch     = vx_scan_next_batch,
    .destroy_local  = vx_scan_destroy_local,
    .destroy_global = vx_scan_destroy_global
};

