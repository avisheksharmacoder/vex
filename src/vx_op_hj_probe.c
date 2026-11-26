#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vx_hash_join.h"

static inline uint64_t vx_hjp_mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline uint64_t vx_hjp_hash_combine(uint64_t h, uint64_t k) {
    return vx_hjp_mix64(h ^ (k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}

static inline uint64_t vx_hjp_hash_bytes(const uint8_t *p, int32_t len) {
    uint64_t h = 14695981039346656037ULL ^ (uint64_t)(uint32_t)len;
    for (int32_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return vx_hjp_mix64(h);
}

static inline size_t vx_hjp_elem_size(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return sizeof(uint8_t);
    case VX_TYPE_I64:  return sizeof(int64_t);
    case VX_TYPE_U64:  return sizeof(uint64_t);
    case VX_TYPE_F64:  return sizeof(double);
    case VX_TYPE_STR:  return sizeof(vx_strview_t);
    }
    return 0;
}

static inline bool vx_hjp_vec_valid(const vx_vec_t *v, uint32_t r) {
    if (!v->has_nulls || !v->validity) return true;
    uint32_t idx = (v->kind == VX_VEC_CONST) ? 0u : r;
    return ((v->validity[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
}

static inline bool vx_hjp_bcol_valid(const vx_hj_col_t *c, uint32_t r) {
    if (!c->has_nulls || !c->validity) return true;
    return ((c->validity[r >> 6] >> (r & 63u)) & 1ULL) != 0;
}

static inline uint32_t vx_hjp_vidx(const vx_vec_t *v, uint32_t r) {
    if (v->kind == VX_VEC_CONST) return 0u;
    if (v->kind == VX_VEC_DICT)  return ((const uint32_t *)v->data)[r];
    return r;
}

static inline const void *vx_hjp_vbase(const vx_vec_t *v) {
    return (v->kind == VX_VEC_DICT) ? v->dict_vals : v->data;
}

typedef struct vx_hjp_gstate {
    vx_hj_table_t *ht; /* retained for duration of probe pipeline */
} vx_hjp_gstate_t;

/* Per-worker 1:N resume state (lives in lstate, surviving ctx->scratch rewinds) */
typedef struct vx_hjp_lstate {
    bool        in_progress;
    bool        probe_matched;
    uint32_t    probe_i;                 /* cursor into in->sel [0..in->sel.count] */
    uint32_t    build_cur;               /* 0 = start next probe_i; else build_row + 1 */
    uint64_t    cur_hash;
    vx_arena_t *mat_arena;               /* batch-scoped arena for late materialization of `in` */
    uint32_t    probe_idx[VX_BATCH_CAP]; /* matched physical row indices in `in` */
    uint32_t    build_idx[VX_BATCH_CAP]; /* matched build rows, or UINT32_MAX for LEFT NULL */
} vx_hjp_lstate_t;

static bool vx_hjp_hash_probe_row(const vx_batch_t *in, const vx_hj_probe_cfg_t *cfg,
                                  uint32_t r, uint64_t *out_hash) {
    uint64_t h = 0x517cc1b727220a95ULL;
    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        const vx_vec_t *kc = &in->cols[cfg->probe_keys[k].batch_col_idx];
        if (!vx_hjp_vec_valid(kc, r)) return false;

        uint32_t    vi   = vx_hjp_vidx(kc, r);
        const void *base = vx_hjp_vbase(kc);
        uint64_t    kh   = 0;

        switch (cfg->probe_keys[k].type) {
        case VX_TYPE_BOOL: kh = vx_hjp_mix64(((const uint8_t *)base)[vi]); break;
        case VX_TYPE_I64:  kh = vx_hjp_mix64((uint64_t)((const int64_t *)base)[vi]); break;
        case VX_TYPE_U64:  kh = vx_hjp_mix64(((const uint64_t *)base)[vi]); break;
        case VX_TYPE_F64: {
            double d = ((const double *)base)[vi];
            if (isnan(d)) return false; /* NaN != NaN */
            uint64_t bits = 0;
            if (d != 0.0) memcpy(&bits, &d, sizeof(bits));
            kh = vx_hjp_mix64(bits);
            break;
        }
        case VX_TYPE_STR: {
            const vx_strview_t *sv = &((const vx_strview_t *)base)[vi];
            const uint8_t      *p  = vx_strview_ptr(sv, kc->strpool);
            kh = vx_hjp_hash_bytes(p, sv->inl.len);
            break;
        }
        }
        h = vx_hjp_hash_combine(h, kh);
    }
    *out_hash = h;
    return true;
}

static bool vx_hjp_keys_equal(const vx_hj_table_t *ht, const vx_batch_t *in,
                              const vx_hj_probe_cfg_t *cfg, uint32_t b_row, uint32_t p_row) {
    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        const vx_vec_t    *pc   = &in->cols[cfg->probe_keys[k].batch_col_idx];
        const vx_hj_col_t *bc   = &ht->key_cols[k];
        uint32_t           vi   = vx_hjp_vidx(pc, p_row);
        const void        *base = vx_hjp_vbase(pc);

        switch (cfg->probe_keys[k].type) {
        case VX_TYPE_BOOL:
            if (((const uint8_t *)base)[vi] != ((const uint8_t *)bc->data)[b_row]) return false;
            break;
        case VX_TYPE_I64:
            if (((const int64_t *)base)[vi] != ((const int64_t *)bc->data)[b_row]) return false;
            break;
        case VX_TYPE_U64:
            if (((const uint64_t *)base)[vi] != ((const uint64_t *)bc->data)[b_row]) return false;
            break;
        case VX_TYPE_F64: {
            double pv = ((const double *)base)[vi];
            double bv = ((const double *)bc->data)[b_row];
            if (pv != bv) return false; /* -0.0 == +0.0 is true in IEEE-754 */
            break;
        }
        case VX_TYPE_STR: {
            vx_strview_t psv = ((const vx_strview_t *)base)[vi];
            vx_strview_t bsv = ((const vx_strview_t *)bc->data)[b_row];
            if (!vx_strview_eq(psv, pc->strpool, bsv, &ht->strpool)) return false;
            break;
        }
        }
    }
    return true;
}

/* Gathers probe column for matched indices into ctx->scratch (handles FLAT/CONST/DICT). */
static vx_status_t vx_hjp_gather_probe_col(const vx_vec_t *src, const uint32_t *p_idx,
                                           uint32_t n, vx_arena_t *scratch, vx_vec_t *out) {
    memset(out, 0, sizeof(*out));
    out->type = src->type;
    out->kind = VX_VEC_FLAT;

    const size_t elem_sz = vx_hjp_elem_size(src->type);
    void        *dst     = vx_arena_alloc(scratch, (size_t)n * elem_sz);
    if (!dst) return VX_ERR_OOM;

    uint64_t *val = NULL;
    if (src->has_nulls && src->validity) {
        val = (uint64_t *)vx_arena_calloc(scratch, (n + 63u) >> 6, sizeof(uint64_t));
        if (!val) return VX_ERR_OOM;
    }

    const void *base    = vx_hjp_vbase(src);
    uint32_t    valid_n = 0;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = p_idx[i];
        if (val && !vx_hjp_vec_valid(src, r)) {
            memset((uint8_t *)dst + (size_t)i * elem_sz, 0, elem_sz);
            continue;
        }
        if (val) {
            val[i >> 6] |= (1ULL << (i & 63u));
            valid_n++;
        }
        uint32_t vi = vx_hjp_vidx(src, r);
        if (elem_sz == 1u)      ((uint8_t *)dst)[i]      = ((const uint8_t *)base)[vi];
        else if (elem_sz == 8u) ((uint64_t *)dst)[i]     = ((const uint64_t *)base)[vi];
        else                    ((vx_strview_t *)dst)[i] = ((const vx_strview_t *)base)[vi];
    }

    out->has_nulls = (val && valid_n < n) ? 1u : 0u;
    out->phys_len  = n;
    out->data      = dst;
    out->validity  = (val && valid_n < n) ? val : NULL;
    out->strpool   = src->strpool; /* alive for entire `in` batch */
    out->owner     = vx_owner_retain(&scratch->hdr);
    return VX_OK;
}

/* Gathers build column for matched indices (UINT32_MAX => NULL for LEFT JOIN). */
static vx_status_t vx_hjp_gather_build_col(const vx_hj_table_t *ht, const vx_hj_col_t *src,
                                           const uint32_t *b_idx, uint32_t n, bool may_have_unmatched,
                                           vx_arena_t *scratch, vx_vec_t *out) {
    memset(out, 0, sizeof(*out));
    out->type = src->type;
    out->kind = VX_VEC_FLAT;

    const size_t elem_sz = vx_hjp_elem_size(src->type);
    void        *dst     = vx_arena_alloc(scratch, (size_t)n * elem_sz);
    if (!dst) return VX_ERR_OOM;

    const bool need_val = src->has_nulls || may_have_unmatched;
    uint64_t  *val      = NULL;
    if (need_val) {
        val = (uint64_t *)vx_arena_calloc(scratch, (n + 63u) >> 6, sizeof(uint64_t));
        if (!val) return VX_ERR_OOM;
    }

    uint32_t valid_n = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t br = b_idx[i];
        if (br == UINT32_MAX || (val && !vx_hjp_bcol_valid(src, br))) {
            memset((uint8_t *)dst + (size_t)i * elem_sz, 0, elem_sz);
            continue;
        }
        if (val) {
            val[i >> 6] |= (1ULL << (i & 63u));
            valid_n++;
        }
        if (elem_sz == 1u)      ((uint8_t *)dst)[i]      = ((const uint8_t *)src->data)[br];
        else if (elem_sz == 8u) ((uint64_t *)dst)[i]     = ((const uint64_t *)src->data)[br];
        else                    ((vx_strview_t *)dst)[i] = ((const vx_strview_t *)src->data)[br];
    }

    out->has_nulls = (val && valid_n < n) ? 1u : 0u;
    out->phys_len  = n;
    out->data      = dst;
    out->validity  = (val && valid_n < n) ? val : NULL;
    out->strpool   = (src->type == VX_TYPE_STR) ? (vx_strpool_t *)&ht->strpool : NULL; /* alive for probe pipeline */
    out->owner     = vx_owner_retain(&scratch->hdr);
    return VX_OK;
}

static vx_status_t vx_hjp_init_global(const vx_op_t *op, vx_exec_ctx_t *ctx, void **gstate) {
    (void)ctx;
    const vx_hj_probe_cfg_t *cfg = (const vx_hj_probe_cfg_t *)op->plan_cfg;
    vx_hjp_gstate_t         *g   = (vx_hjp_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;
    g->ht   = cfg->ht ? (vx_hj_table_t *)vx_owner_retain(&cfg->ht->hdr) : NULL;
    *gstate = g;
    return VX_OK;
}

static vx_status_t vx_hjp_init_local(const vx_op_t *op, vx_exec_ctx_t *ctx,
                                     void *gstate, void **lstate) {
    (void)op; (void)ctx; (void)gstate;
    vx_hjp_lstate_t *l = (vx_hjp_lstate_t *)calloc(1u, sizeof(*l));
    if (!l) return VX_ERR_OOM;
    l->mat_arena = vx_arena_create(32u * 1024u, 2u * 1024u * 1024u, 0);
    if (!l->mat_arena) {
        free(l);
        return VX_ERR_OOM;
    }
    *lstate = l;
    return VX_OK;
}

static vx_op_res_t vx_hjp_execute(const vx_op_t *op, vx_exec_ctx_t *ctx,
                                  void *gstate, void *lstate,
                                  vx_batch_t *in, vx_batch_t *out) {
    const vx_hj_probe_cfg_t *cfg = (const vx_hj_probe_cfg_t *)op->plan_cfg;
    const vx_hj_table_t     *ht  = ((const vx_hjp_gstate_t *)gstate)->ht;
    vx_hjp_lstate_t         *l   = (vx_hjp_lstate_t *)lstate;

    vx_batch_reset(out);
    const uint32_t n = in->sel.count;
    if (n == 0) {
        l->in_progress = false;
        return VX_OP_NEED_MORE_INPUT;
    }

    /* Empty build table fast paths (zero probe key/payload touches on INNER/SEMI) */
    if (!ht || ht->nrows == 0) {
        l->in_progress = false;
        if (cfg->join_type == VX_JOIN_INNER || cfg->join_type == VX_JOIN_SEMI) {
            return VX_OP_NEED_MORE_INPUT;
        }
        if (cfg->join_type == VX_JOIN_ANTI) {
            /* Forward surviving probe rows zero-copy */
            out->ncols           = cfg->n_out_cols;
            out->sel             = in->sel;
            out->row_src         = in->row_src;
            out->row_src.src_owner = vx_owner_retain(in->row_src.src_owner);
            for (uint32_t c = 0; c < cfg->n_out_cols; ++c) {
                uint32_t sc = cfg->out_cols[c].src_col_idx;
                out->cols[c]       = in->cols[sc];
                out->cols[c].owner = vx_owner_retain(out->cols[c].owner);
            }
            return VX_OP_NEED_MORE_INPUT;
        }
        /* VX_JOIN_LEFT against empty build: every probe row pairs with build NULL */
        vx_arena_reset(l->mat_arena);
        for (uint32_t i = 0; i < n; ++i) {
            l->probe_idx[i] = in->sel.is_identity ? i : in->sel.idx[i];
            l->build_idx[i] = UINT32_MAX;
        }
        out->ncols           = cfg->n_out_cols;
        out->sel.count       = n;
        out->sel.is_identity = true;
        for (uint32_t c = 0; c < cfg->n_out_cols; ++c) {
            const vx_hj_out_col_t *oc = &cfg->out_cols[c];
            vx_status_t            st;
            if (oc->from_build) {
                st = vx_hjp_gather_build_col(ht, &ht->pay_cols[oc->src_col_idx],
                                             l->build_idx, n, true, ctx->scratch, &out->cols[c]);
            } else {
                st = vx_batch_materialize_col(in, oc->src_col_idx, oc->table_col_idx, l->mat_arena);
                if (st == VX_OK) {
                    st = vx_hjp_gather_probe_col(&in->cols[oc->src_col_idx], l->probe_idx, n,
                                                 ctx->scratch, &out->cols[c]);
                }
            }
            if (st != VX_OK) {
                vx_batch_reset(out);
                if (ctx->err) ctx->err->code = st;
                return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
            }
        }
        return VX_OP_NEED_MORE_INPUT;
    }

    /* Step 1: On a fresh `in` batch, reset cursor/mat_arena and materialize ONLY probe join keys */
    if (!l->in_progress) {
        vx_arena_reset(l->mat_arena);
        l->in_progress   = true;
        l->probe_i       = 0;
        l->build_cur     = 0;
        l->probe_matched = false;

        for (uint32_t k = 0; k < cfg->nkeys; ++k) {
            vx_status_t st = vx_batch_materialize_col(in, cfg->probe_keys[k].batch_col_idx,
                                                      cfg->probe_keys[k].table_col_idx,
                                                      l->mat_arena);
            if (st != VX_OK) {
                l->in_progress = false;
                if (ctx->err) ctx->err->code = st;
                return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
            }
        }
    }

    /* Step 2: Resumable 1:N probe loop capped at VX_BATCH_CAP (4,096) output rows */
    const bool id    = in->sel.is_identity;
    uint32_t   out_n = 0;

    while (l->probe_i < n && out_n < VX_BATCH_CAP) {
        uint32_t r = id ? l->probe_i : in->sel.idx[l->probe_i];

        if (l->build_cur == 0) {
            l->probe_matched = false;
            uint64_t h       = 0;
            if (!vx_hjp_hash_probe_row(in, cfg, r, &h)) {
                if (cfg->join_type == VX_JOIN_LEFT || cfg->join_type == VX_JOIN_ANTI) {
                    l->probe_idx[out_n] = r;
                    l->build_idx[out_n] = UINT32_MAX;
                    out_n++;
                }
                l->probe_i++;
                continue;
            }

            l->cur_hash  = h;
            l->build_cur = ht->head[(uint32_t)h & ht->bucket_mask];
            if (l->build_cur == 0) {
                if (cfg->join_type == VX_JOIN_LEFT || cfg->join_type == VX_JOIN_ANTI) {
                    l->probe_idx[out_n] = r;
                    l->build_idx[out_n] = UINT32_MAX;
                    out_n++;
                }
                l->probe_i++;
                continue;
            }
        }

        /* Walk chain for current probe_i; stops early if out_n hits VX_BATCH_CAP */
        while (l->build_cur != 0 && out_n < VX_BATCH_CAP) {
            uint32_t b_row = l->build_cur - 1u;
            l->build_cur   = ht->next[b_row];

            if (ht->hashes[b_row] == l->cur_hash &&
                vx_hjp_keys_equal(ht, in, cfg, b_row, r)) {
                l->probe_matched = true;
                if (cfg->join_type == VX_JOIN_SEMI) {
                    l->probe_idx[out_n++] = r;
                    l->build_cur = 0; /* emit probe row at most once */
                    break;
                }
                if (cfg->join_type == VX_JOIN_ANTI) {
                    l->build_cur = 0; /* suppress matched probe row */
                    break;
                }
                l->probe_idx[out_n] = r;
                l->build_idx[out_n] = b_row;
                out_n++;
            }
        }

        if (l->build_cur == 0) {
            /* Proof: if !probe_matched, inner loop never incremented out_n, so out_n < VX_BATCH_CAP */
            if (!l->probe_matched &&
                (cfg->join_type == VX_JOIN_LEFT || cfg->join_type == VX_JOIN_ANTI)) {
                l->probe_idx[out_n] = r;
                l->build_idx[out_n] = UINT32_MAX;
                out_n++;
            }
            l->probe_i++;
        }
    }

    const bool more_remaining = (l->probe_i < n) || (l->build_cur != 0);
    if (!more_remaining) {
        l->in_progress = false;
    }

    /* Zero matches: skip probe payload materialization entirely */
    if (out_n == 0) {
        return more_remaining ? VX_OP_HAVE_MORE_OUTPUT : VX_OP_NEED_MORE_INPUT;
    }

    /* Step 3a: SEMI / ANTI zero-copy fast path (preserves VX_VEC_DEFERRED for downstream!) */
    if (cfg->join_type == VX_JOIN_SEMI || cfg->join_type == VX_JOIN_ANTI) {
        out->ncols           = cfg->n_out_cols;
        out->sel.count       = out_n;
        out->sel.is_identity = false;
        memcpy(out->sel.idx, l->probe_idx, (size_t)out_n * sizeof(uint32_t));
        out->row_src           = in->row_src;
        out->row_src.src_owner = vx_owner_retain(in->row_src.src_owner);
        for (uint32_t c = 0; c < cfg->n_out_cols; ++c) {
            uint32_t sc        = cfg->out_cols[c].src_col_idx;
            out->cols[c]       = in->cols[sc];
            out->cols[c].owner = vx_owner_retain(out->cols[c].owner);
        }
        return VX_OP_NEED_MORE_INPUT; /* SEMI/ANTI emits <= n <= VX_BATCH_CAP rows */
    }

    /* Step 3b: INNER / LEFT — late-materialize probe payload columns only now that out_n > 0 */
    out->ncols           = cfg->n_out_cols;
    out->sel.count       = out_n;
    out->sel.is_identity = true;

    const bool may_unmatch = (cfg->join_type == VX_JOIN_LEFT);
    for (uint32_t c = 0; c < cfg->n_out_cols; ++c) {
        const vx_hj_out_col_t *oc = &cfg->out_cols[c];
        vx_status_t            st;

        if (oc->from_build) {
            st = vx_hjp_gather_build_col(ht, &ht->pay_cols[oc->src_col_idx],
                                         l->build_idx, out_n, may_unmatch,
                                         ctx->scratch, &out->cols[c]);
        } else {
            /* Uses l->mat_arena so column stays valid across HAVE_MORE_OUTPUT rewinds */
            st = vx_batch_materialize_col(in, oc->src_col_idx, oc->table_col_idx, l->mat_arena);
            if (st == VX_OK) {
                st = vx_hjp_gather_probe_col(&in->cols[oc->src_col_idx], l->probe_idx, out_n,
                                             ctx->scratch, &out->cols[c]);
            }
        }
        if (st != VX_OK) {
            vx_batch_reset(out);
            l->in_progress = false;
            if (ctx->err) ctx->err->code = st;
            return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
        }
    }

    return more_remaining ? VX_OP_HAVE_MORE_OUTPUT : VX_OP_NEED_MORE_INPUT;
}

static void vx_hjp_destroy_local(void *lstate) {
    vx_hjp_lstate_t *l = (vx_hjp_lstate_t *)lstate;
    if (!l) return;
    if (l->mat_arena) vx_owner_release(&l->mat_arena->hdr);
    free(l);
}

static void vx_hjp_destroy_global(void *gstate) {
    vx_hjp_gstate_t *g = (vx_hjp_gstate_t *)gstate;
    if (!g) return;
    if (g->ht) vx_owner_release(&g->ht->hdr);
    free(g);
}

const vx_op_vtbl_t VX_OP_HASH_JOIN_PROBE = {
    .name           = "HashJoinProbe",
    .init_global    = vx_hjp_init_global,
    .init_local     = vx_hjp_init_local,
    .execute        = vx_hjp_execute,
    .destroy_local  = vx_hjp_destroy_local,
    .destroy_global = vx_hjp_destroy_global
};

