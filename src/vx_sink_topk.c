#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vx_sort.h"

/* --- Bounded Max-Heap (root heap[0] is the WORST row currently inside Top-K) --- */
typedef struct vx_topk_heap {
    vx_sort_row_t **rows;
    size_t          len;
    size_t          cap; /* == K = offset + limit */
} vx_topk_heap_t;

static void vx_heap_sift_up(const vx_sort_cfg_t *cfg, vx_topk_heap_t *h, size_t idx) {
    vx_sort_row_t *x = h->rows[idx];
    while (idx > 0) {
        size_t parent = (idx - 1u) >> 1;
        if (vx_sort_row_cmp(cfg, h->rows[parent], x) >= 0) break;
        h->rows[idx] = h->rows[parent];
        idx = parent;
    }
    h->rows[idx] = x;
}

static void vx_heap_sift_down(const vx_sort_cfg_t *cfg, vx_topk_heap_t *h, size_t idx) {
    vx_sort_row_t *x = h->rows[idx];
    for (;;) {
        size_t l = (idx << 1) + 1u;
        if (l >= h->len) break;
        size_t r    = l + 1u;
        size_t best = (r < h->len && vx_sort_row_cmp(cfg, h->rows[r], h->rows[l]) > 0) ? r : l;
        if (vx_sort_row_cmp(cfg, x, h->rows[best]) >= 0) break;
        h->rows[idx] = h->rows[best];
        idx = best;
    }
    h->rows[idx] = x;
}

/* Takes ownership of `row` if accepted; frees `row` if rejected, frees evicted root if replaced. */
static void vx_heap_push_owned(const vx_sort_cfg_t *cfg, vx_topk_heap_t *h, vx_sort_row_t *row) {
    if (h->len < h->cap) {
        h->rows[h->len] = row;
        vx_heap_sift_up(cfg, h, h->len);
        h->len++;
        return;
    }
    if (vx_sort_row_cmp(cfg, row, h->rows[0]) < 0) {
        free(h->rows[0]);
        h->rows[0] = row;
        vx_heap_sift_down(cfg, h, 0u);
    } else {
        free(row);
    }
}

/* Key-only comparison of batch row `r` against heap root `worst` (avoids payload fetches). */
static int vx_topk_cmp_batch_vs_row(const vx_sort_cfg_t *cfg, const vx_batch_t *in,
                                    uint32_t phys_row, const vx_sort_row_t *worst) {
    const uint32_t        nc = cfg->ncols;
    const vx_sort_cell_t *cw = vx_sr_cells(worst, nc);
    const uint8_t        *sw = vx_sr_strdata(worst, nc);

    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        const vx_sort_key_t *ks = &cfg->keys[k];
        const uint32_t       oc = ks->out_col_idx;
        const vx_vec_t      *v  = &in->cols[cfg->batch_col_ids[oc]];

        const bool va = vx_svec_valid(v, phys_row);
        const bool vb = vx_sr_is_valid(worst, oc);
        if (!va || !vb) {
            if (va == vb) continue;
            return (!va) ? (ks->nulls_first ? -1 : 1) : (ks->nulls_first ? 1 : -1);
        }

        uint32_t    vi   = vx_svec_vidx(v, phys_row);
        const void *base = vx_svec_base(v);
        int         cmp  = 0;

        switch (ks->type) {
        case VX_TYPE_BOOL: {
            uint8_t a = ((const uint8_t *)base)[vi];
            cmp = (a < cw[oc].b) ? -1 : (a > cw[oc].b ? 1 : 0);
            break;
        }
        case VX_TYPE_I64: {
            int64_t a = ((const int64_t *)base)[vi];
            cmp = (a < cw[oc].i64) ? -1 : (a > cw[oc].i64 ? 1 : 0);
            break;
        }
        case VX_TYPE_U64: {
            uint64_t a = ((const uint64_t *)base)[vi];
            cmp = (a < cw[oc].u64) ? -1 : (a > cw[oc].u64 ? 1 : 0);
            break;
        }
        case VX_TYPE_F64:
            cmp = vx_cmp_f64_total(((const double *)base)[vi], cw[oc].f64);
            break;
        case VX_TYPE_STR: {
            const vx_strview_t *sv = &((const vx_strview_t *)base)[vi];
            cmp = vx_cmp_bytes_lex(vx_strview_ptr(sv, v->strpool), sv->inl.len,
                                   sw + cw[oc].str.off, cw[oc].str.len);
            break;
        }
        }
        if (cmp != 0) return ks->desc ? -cmp : cmp;
    }
    return 0;
}

typedef struct vx_topk_gstate {
    const vx_sort_cfg_t *cfg;
    pthread_mutex_t      mu;
    size_t               k_cap;      /* offset + limit */
    vx_topk_heap_t       heap;
    vx_sort_row_t      **sorted;     /* finalized slice [offset .. len) */
    size_t               n_sorted;
} vx_topk_gstate_t;

typedef struct vx_topk_lstate {
    const vx_sort_cfg_t *cfg;
    vx_topk_heap_t       heap;
} vx_topk_lstate_t;

static vx_status_t vx_topk_init_global(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void **gstate) {
    (void)ctx;
    const vx_sort_cfg_t *cfg = (const vx_sort_cfg_t *)sk->plan_cfg;
    if (cfg->offset > SIZE_MAX - cfg->limit) return VX_ERR_OOM;

    vx_topk_gstate_t *g = (vx_topk_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;
    g->cfg   = cfg;
    g->k_cap = (size_t)(cfg->offset + cfg->limit);
    if (pthread_mutex_init(&g->mu, NULL) != 0) { free(g); return VX_ERR_OOM; }

    if (g->k_cap > 0) {
        g->heap.rows = (vx_sort_row_t **)calloc(g->k_cap, sizeof(*g->heap.rows));
        if (!g->heap.rows) { pthread_mutex_destroy(&g->mu); free(g); return VX_ERR_OOM; }
        g->heap.cap = g->k_cap;
    }
    *gstate = g;
    return VX_OK;
}

static vx_status_t vx_topk_init_local(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                      void *gstate, void **lstate) {
    (void)ctx;
    const vx_sort_cfg_t    *cfg = (const vx_sort_cfg_t *)sk->plan_cfg;
    const vx_topk_gstate_t *g   = (const vx_topk_gstate_t *)gstate;
    vx_topk_lstate_t       *l   = (vx_topk_lstate_t *)calloc(1u, sizeof(*l));
    if (!l) return VX_ERR_OOM;
    l->cfg = cfg;
    if (g->k_cap > 0) {
        l->heap.rows = (vx_sort_row_t **)calloc(g->k_cap, sizeof(*l->heap.rows));
        if (!l->heap.rows) { free(l); return VX_ERR_OOM; }
        l->heap.cap = g->k_cap;
    }
    *lstate = l;
    return VX_OK;
}

static vx_op_res_t vx_topk_consume(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                   void *gstate, void *lstate, vx_batch_t *in) {
    (void)sk; (void)gstate;
    vx_topk_lstate_t    *l   = (vx_topk_lstate_t *)lstate;
    const vx_sort_cfg_t *cfg = l->cfg;

    if (l->heap.cap == 0) return VX_OP_FINISHED; /* [:0] */
    if (in->sel.count == 0) return VX_OP_NEED_MORE_INPUT;

    /* Step 1: Materialize ONLY sort-key columns first */
    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        uint32_t    oc = cfg->keys[k].out_col_idx;
        vx_status_t st = vx_batch_materialize_col(in, cfg->batch_col_ids[oc],
                                                  cfg->table_col_ids[oc], ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    /* Step 2: Pre-filter against current heap root before touching payload columns */
    uint32_t       cands[VX_BATCH_CAP];
    uint32_t       ncands = 0;
    const uint32_t n      = in->sel.count;
    const bool     id     = in->sel.is_identity;

    if (l->heap.len < l->heap.cap) {
        for (uint32_t i = 0; i < n; ++i) cands[ncands++] = id ? i : in->sel.idx[i];
    } else {
        const vx_sort_row_t *worst = l->heap.rows[0];
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t r = id ? i : in->sel.idx[i];
            if (vx_topk_cmp_batch_vs_row(cfg, in, r, worst) < 0) {
                cands[ncands++] = r;
            }
        }
    }

    if (ncands == 0) return VX_OP_NEED_MORE_INPUT; /* 0 payload pages touched */

    /* Step 3: Late-materialize remaining payload columns only for winning batches */
    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        vx_status_t st = vx_batch_materialize_col(in, cfg->batch_col_ids[c],
                                                  cfg->table_col_ids[c], ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    /* Step 4: Pack surviving rows and push into O(log K) max-heap */
    for (uint32_t i = 0; i < ncands; ++i) {
        uint32_t r = cands[i];
        /* Re-check against tightening heap root before allocating */
        if (l->heap.len == l->heap.cap &&
            vx_topk_cmp_batch_vs_row(cfg, in, r, l->heap.rows[0]) >= 0) {
            continue;
        }
        uint32_t       sb   = 0;
        size_t         tot  = vx_sort_row_measure(cfg, in, r, &sb);
        vx_sort_row_t *row  = (vx_sort_row_t *)malloc(tot);
        if (!row) return VX_OP_OOM;
        vx_sort_row_pack(cfg, in, r, tot, sb, row);
        vx_heap_push_owned(cfg, &l->heap, row);
    }

    return VX_OP_NEED_MORE_INPUT;
}

static vx_status_t vx_topk_combine(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                   void *gstate, void *lstate) {
    (void)sk; (void)ctx;
    vx_topk_gstate_t *g = (vx_topk_gstate_t *)gstate;
    vx_topk_lstate_t *l = (vx_topk_lstate_t *)lstate;
    if (l->heap.len == 0) return VX_OK;

    pthread_mutex_lock(&g->mu);
    for (size_t i = 0; i < l->heap.len; ++i) {
        vx_heap_push_owned(g->cfg, &g->heap, l->heap.rows[i]);
    }
    l->heap.len = 0; /* ownership transferred or freed */
    pthread_mutex_unlock(&g->mu);
    return VX_OK;
}

static vx_status_t vx_topk_finalize(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate) {
    (void)sk; (void)ctx;
    vx_topk_gstate_t *g = (vx_topk_gstate_t *)gstate;
    if (g->heap.len == 0) return VX_OK;

    vx_status_t st = vx_sort_rows_stable(g->cfg, g->heap.rows, g->heap.len);
    if (st != VX_OK) return st;

    size_t off = (size_t)g->cfg->offset;
    if (off >= g->heap.len) {
        g->sorted   = NULL;
        g->n_sorted = 0;
    } else {
        g->sorted   = g->heap.rows + off;
        g->n_sorted = g->heap.len - off;
        if (g->cfg->limit > 0 && g->n_sorted > (size_t)g->cfg->limit) {
            g->n_sorted = (size_t)g->cfg->limit;
        }
    }
    return VX_OK;
}

static void vx_topk_destroy_local(void *lstate) {
    vx_topk_lstate_t *l = (vx_topk_lstate_t *)lstate;
    if (!l) return;
    for (size_t i = 0; i < l->heap.len; ++i) free(l->heap.rows[i]);
    free(l->heap.rows);
    free(l);
}

static void vx_topk_destroy_global(void *gstate) {
    vx_topk_gstate_t *g = (vx_topk_gstate_t *)gstate;
    if (!g) return;
    for (size_t i = 0; i < g->heap.len; ++i) free(g->heap.rows[i]);
    free(g->heap.rows);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

const vx_sink_vtbl_t VX_SINK_TOPK = {
    .name           = "TopKSink",
    .init_global    = vx_topk_init_global,
    .init_local     = vx_topk_init_local,
    .sink           = vx_topk_consume,
    .combine        = vx_topk_combine,
    .finalize       = vx_topk_finalize,
    .destroy_local  = vx_topk_destroy_local,
    .destroy_global = vx_topk_destroy_global
};

/* --- Ordered Single-Stream TopKScan Source (plan_cfg = finalized vx_topk_gstate_t*) --- */
typedef struct vx_topk_scan_g {
    const vx_topk_gstate_t *topk;
    size_t                  cursor;
} vx_topk_scan_g_t;

static vx_status_t vx_topks_init_g(const vx_source_t *s, vx_exec_ctx_t *ctx, void **gs) {
    (void)ctx;
    vx_topk_scan_g_t *g = (vx_topk_scan_g_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;
    g->topk = (const vx_topk_gstate_t *)s->plan_cfg;
    *gs = g;
    return VX_OK;
}
static vx_status_t vx_topks_init_l(const vx_source_t *s, vx_exec_ctx_t *c, void *g, void **l) {
    (void)s; (void)c; (void)g; *l = NULL; return VX_OK;
}
static vx_op_res_t vx_topks_next(const vx_source_t *s, vx_exec_ctx_t *ctx,
                                 void *gs, void *ls, vx_batch_t *out) {
    (void)s; (void)ls;
    if (ctx->worker_id != 0) return VX_OP_FINISHED; /* preserve strict sorted order */
    vx_topk_scan_g_t *g = (vx_topk_scan_g_t *)gs;
    if (g->cursor >= g->topk->n_sorted) return VX_OP_FINISHED;

    size_t   rem = g->topk->n_sorted - g->cursor;
    uint32_t n   = (rem > VX_BATCH_CAP) ? VX_BATCH_CAP : (uint32_t)rem;
    vx_status_t st = vx_sort_unpack_batch(
        g->topk->cfg, (const vx_sort_row_t *const *)(g->topk->sorted + g->cursor),
        n, ctx->scratch, out);
    if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    g->cursor += n;
    return VX_OP_HAVE_MORE_OUTPUT;
}
static void vx_topks_destroy_l(void *ls) { (void)ls; }
static void vx_topks_destroy_g(void *gs) { free(gs); }

const vx_source_vtbl_t VX_SOURCE_TOPK_SCAN = {
    .name           = "TopKScan",
    .init_global    = vx_topks_init_g,
    .init_local     = vx_topks_init_l,
    .next_batch     = vx_topks_next,
    .destroy_local  = vx_topks_destroy_l,
    .destroy_global = vx_topks_destroy_g
};

