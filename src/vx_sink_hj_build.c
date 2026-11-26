#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vx_hash_join.h"

static inline uint64_t vx_hj_mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline uint64_t vx_hj_hash_combine(uint64_t h, uint64_t k) {
    return vx_hj_mix64(h ^ (k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}

static inline uint64_t vx_hj_hash_bytes(const uint8_t *p, int32_t len) {
    uint64_t h = 14695981039346656037ULL ^ (uint64_t)(uint32_t)len;
    for (int32_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return vx_hj_mix64(h);
}

static inline size_t vx_hj_elem_size(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return sizeof(uint8_t);
    case VX_TYPE_I64:  return sizeof(int64_t);
    case VX_TYPE_U64:  return sizeof(uint64_t);
    case VX_TYPE_F64:  return sizeof(double);
    case VX_TYPE_STR:  return sizeof(vx_strview_t);
    }
    return 0;
}

static inline bool vx_hj_row_valid(const vx_vec_t *v, uint32_t r) {
    if (!v->has_nulls || !v->validity) return true;
    uint32_t idx = (v->kind == VX_VEC_CONST) ? 0u : r;
    return ((v->validity[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
}

static inline uint32_t vx_hj_vidx(const vx_vec_t *v, uint32_t r) {
    if (v->kind == VX_VEC_CONST) return 0u;
    if (v->kind == VX_VEC_DICT)  return ((const uint32_t *)v->data)[r];
    return r;
}

static inline const void *vx_hj_vbase(const vx_vec_t *v) {
    return (v->kind == VX_VEC_DICT) ? v->dict_vals : v->data;
}

/* Staged worker block: dense non-null-key build rows collected during sink(). */
typedef struct vx_hj_block {
    uint32_t      nrows;
    uint32_t      str_buf_offset; /* assigned in combine() when merging worker strpool */
    uint64_t     *hashes;
    vx_hj_col_t  *keys;           /* [nkeys] */
    vx_hj_col_t  *pays;           /* [n_payload_cols] */
} vx_hj_block_t;

typedef struct vx_hjb_gstate {
    const vx_hj_build_cfg_t *cfg;
    pthread_mutex_t          mu;
    vx_hj_block_t           *blocks;
    uint32_t                 nblocks;
    uint32_t                 cap_blocks;
    uint64_t                 total_rows;
    vx_hj_table_t           *ht;
} vx_hjb_gstate_t;

typedef struct vx_hjb_lstate {
    const vx_hj_build_cfg_t *cfg;
    vx_hj_block_t           *blocks;
    uint32_t                 nblocks;
    uint32_t                 cap_blocks;
    uint64_t                 local_rows;
    vx_strpool_t             strpool;
} vx_hjb_lstate_t;

static void vx_hj_block_free(vx_hj_block_t *blk, uint32_t nkeys, uint32_t npay) {
    free(blk->hashes);
    if (blk->keys) {
        for (uint32_t k = 0; k < nkeys; ++k) {
            free(blk->keys[k].data);
            free(blk->keys[k].validity);
        }
        free(blk->keys);
    }
    if (blk->pays) {
        for (uint32_t c = 0; c < npay; ++c) {
            free(blk->pays[c].data);
            free(blk->pays[c].validity);
        }
        free(blk->pays);
    }
}

static void vx_hj_table_destroy(vx_owner_t *owner) {
    vx_hj_table_t *ht = (vx_hj_table_t *)owner;
    if (!ht) return;
    free(ht->head);
    free(ht->next);
    free(ht->hashes);
    if (ht->key_cols) {
        for (uint32_t k = 0; k < ht->nkeys; ++k) {
            free(ht->key_cols[k].data);
            free(ht->key_cols[k].validity);
        }
        free(ht->key_cols);
    }
    if (ht->pay_cols) {
        for (uint32_t c = 0; c < ht->n_payload_cols; ++c) {
            free(ht->pay_cols[c].data);
            free(ht->pay_cols[c].validity);
        }
        free(ht->pay_cols);
    }
    vx_strpool_destroy(&ht->strpool);
    free(ht);
}

static vx_hj_table_t *vx_hj_table_create(const vx_hj_build_cfg_t *cfg) {
    vx_hj_table_t *ht = (vx_hj_table_t *)calloc(1u, sizeof(*ht));
    if (!ht) return NULL;
    vx_owner_init(&ht->hdr, VX_OWN_JOIN_HT, vx_hj_table_destroy);
    ht->nkeys          = cfg->nkeys;
    ht->n_payload_cols = cfg->n_payload_cols;
    if (vx_strpool_init(&ht->strpool, 4u) != VX_OK) {
        free(ht);
        return NULL;
    }
    return ht;
}

static vx_status_t vx_hjb_init_global(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void **gstate) {
    (void)ctx;
    const vx_hj_build_cfg_t *cfg = (const vx_hj_build_cfg_t *)sk->plan_cfg;
    vx_hjb_gstate_t         *g   = (vx_hjb_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;

    g->cfg = cfg;
    if (pthread_mutex_init(&g->mu, NULL) != 0) {
        free(g);
        return VX_ERR_OOM;
    }
    g->ht = vx_hj_table_create(cfg);
    if (!g->ht) {
        pthread_mutex_destroy(&g->mu);
        free(g);
        return VX_ERR_OOM;
    }
    *gstate = g;
    return VX_OK;
}

static vx_status_t vx_hjb_init_local(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                     void *gstate, void **lstate) {
    (void)ctx; (void)gstate;
    const vx_hj_build_cfg_t *cfg = (const vx_hj_build_cfg_t *)sk->plan_cfg;
    vx_hjb_lstate_t         *l   = (vx_hjb_lstate_t *)calloc(1u, sizeof(*l));
    if (!l) return VX_ERR_OOM;

    l->cfg = cfg;
    if (vx_strpool_init(&l->strpool, 4u) != VX_OK) {
        free(l);
        return VX_ERR_OOM;
    }
    *lstate = l;
    return VX_OK;
}

static vx_status_t vx_hjb_pack_col(const vx_vec_t *src, const uint32_t *rows, uint32_t n,
                                   bool track_nulls, vx_strpool_t *dst_pool, vx_hj_col_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->type = src->type;
    const size_t elem_sz = vx_hj_elem_size(src->type);

    dst->data = malloc((size_t)n * elem_sz);
    if (!dst->data) return VX_ERR_OOM;

    uint64_t *val = NULL;
    if (track_nulls && src->has_nulls && src->validity) {
        uint32_t nw = (n + 63u) >> 6;
        val = (uint64_t *)calloc(nw, sizeof(uint64_t));
        if (!val) {
            free(dst->data);
            dst->data = NULL;
            return VX_ERR_OOM;
        }
    }

    const void *base    = vx_hj_vbase(src);
    uint32_t    valid_n = 0;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = rows[i];
        if (val && !vx_hj_row_valid(src, r)) {
            memset((uint8_t *)dst->data + (size_t)i * elem_sz, 0, elem_sz);
            continue;
        }
        if (val) {
            val[i >> 6] |= (1ULL << (i & 63u));
            valid_n++;
        }

        uint32_t vi = vx_hj_vidx(src, r);
        switch (src->type) {
        case VX_TYPE_BOOL: ((uint8_t *)dst->data)[i]  = ((const uint8_t *)base)[vi]; break;
        case VX_TYPE_I64:  ((int64_t *)dst->data)[i]  = ((const int64_t *)base)[vi]; break;
        case VX_TYPE_U64:  ((uint64_t *)dst->data)[i] = ((const uint64_t *)base)[vi]; break;
        case VX_TYPE_F64: {
            double d = ((const double *)base)[vi];
            ((double *)dst->data)[i] = (d == 0.0) ? +0.0 : d; /* normalize -0.0 */
            break;
        }
        case VX_TYPE_STR: {
            vx_strview_t sv = ((const vx_strview_t *)base)[vi];
            if (!vx_strview_is_inline(sv)) {
                const uint8_t *p = vx_strview_ptr(&sv, src->strpool);
                vx_status_t st = vx_strpool_append(dst_pool, p, sv.inl.len, &sv);
                if (st != VX_OK) {
                    free(val);
                    free(dst->data);
                    dst->data = NULL;
                    return st;
                }
            }
            ((vx_strview_t *)dst->data)[i] = sv;
            break;
        }
        }
    }

    if (val && valid_n < n) {
        dst->validity  = val;
        dst->has_nulls = 1u;
    } else {
        free(val);
        dst->validity  = NULL;
        dst->has_nulls = 0u;
    }
    return VX_OK;
}

static vx_op_res_t vx_hjb_sink_consume(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                       void *gstate, void *lstate, vx_batch_t *in) {
    (void)sk; (void)gstate;
    if (in->sel.count == 0) return VX_OP_NEED_MORE_INPUT;

    vx_hjb_lstate_t         *l   = (vx_hjb_lstate_t *)lstate;
    const vx_hj_build_cfg_t *cfg = l->cfg;

    /* Step 1: materialize only join keys first to filter out NULL / NaN keys */
    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        vx_status_t st = vx_batch_materialize_col(in, cfg->keys[k].batch_col_idx,
                                                  cfg->keys[k].table_col_idx, ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    uint32_t live_rows[VX_BATCH_CAP];
    uint64_t live_hashes[VX_BATCH_CAP];
    uint32_t live_n = 0;

    const uint32_t n  = in->sel.count;
    const bool     id = in->sel.is_identity;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r  = id ? i : in->sel.idx[i];
        uint64_t h  = 0x517cc1b727220a95ULL;
        bool     ok = true;

        for (uint32_t k = 0; k < cfg->nkeys; ++k) {
            const vx_vec_t *kc = &in->cols[cfg->keys[k].batch_col_idx];
            if (!vx_hj_row_valid(kc, r)) { ok = false; break; }

            uint32_t    vi   = vx_hj_vidx(kc, r);
            const void *base = vx_hj_vbase(kc);
            uint64_t    kh   = 0;

            switch (cfg->keys[k].type) {
            case VX_TYPE_BOOL: kh = vx_hj_mix64(((const uint8_t *)base)[vi]); break;
            case VX_TYPE_I64:  kh = vx_hj_mix64((uint64_t)((const int64_t *)base)[vi]); break;
            case VX_TYPE_U64:  kh = vx_hj_mix64(((const uint64_t *)base)[vi]); break;
            case VX_TYPE_F64: {
                double d = ((const double *)base)[vi];
                if (isnan(d)) { ok = false; break; } /* SQL/IEEE-754: NaN != NaN */
                uint64_t bits = 0;
                if (d != 0.0) memcpy(&bits, &d, sizeof(bits)); /* -0.0 -> 0ULL */
                kh = vx_hj_mix64(bits);
                break;
            }
            case VX_TYPE_STR: {
                const vx_strview_t *sv = &((const vx_strview_t *)base)[vi];
                const uint8_t      *p  = vx_strview_ptr(sv, kc->strpool);
                kh = vx_hj_hash_bytes(p, sv->inl.len);
                break;
            }
            }
            if (!ok) break;
            h = vx_hj_hash_combine(h, kh);
        }

        if (ok) {
            live_rows[live_n]   = r;
            live_hashes[live_n] = h;
            live_n++;
        }
    }

    if (live_n == 0) return VX_OP_NEED_MORE_INPUT; /* skip build payload columns completely */

    /* Step 2: materialize payload columns only for batches with valid join keys */
    for (uint32_t c = 0; c < cfg->n_payload_cols; ++c) {
        vx_status_t st = vx_batch_materialize_col(in, cfg->payload_batch_cols[c],
                                                  cfg->payload_table_cols[c], ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    if (l->nblocks == l->cap_blocks) {
        uint32_t       ncap = l->cap_blocks ? l->cap_blocks * 2u : 16u;
        vx_hj_block_t *nb   = (vx_hj_block_t *)realloc(l->blocks, (size_t)ncap * sizeof(*nb));
        if (!nb) return VX_OP_OOM;
        l->blocks     = nb;
        l->cap_blocks = ncap;
    }

    vx_hj_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.nrows  = live_n;
    blk.hashes = (uint64_t *)malloc((size_t)live_n * sizeof(*blk.hashes));
    blk.keys   = (vx_hj_col_t *)calloc(cfg->nkeys, sizeof(*blk.keys));
    if (cfg->n_payload_cols > 0) {
        blk.pays = (vx_hj_col_t *)calloc(cfg->n_payload_cols, sizeof(*blk.pays));
    }
    if (!blk.hashes || !blk.keys || (cfg->n_payload_cols > 0 && !blk.pays)) {
        vx_hj_block_free(&blk, cfg->nkeys, cfg->n_payload_cols);
        return VX_OP_OOM;
    }
    memcpy(blk.hashes, live_hashes, (size_t)live_n * sizeof(*blk.hashes));

    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        vx_status_t st = vx_hjb_pack_col(&in->cols[cfg->keys[k].batch_col_idx],
                                         live_rows, live_n, false, &l->strpool, &blk.keys[k]);
        if (st != VX_OK) {
            vx_hj_block_free(&blk, cfg->nkeys, cfg->n_payload_cols);
            return VX_OP_OOM;
        }
    }
    for (uint32_t c = 0; c < cfg->n_payload_cols; ++c) {
        vx_status_t st = vx_hjb_pack_col(&in->cols[cfg->payload_batch_cols[c]],
                                         live_rows, live_n, true, &l->strpool, &blk.pays[c]);
        if (st != VX_OK) {
            vx_hj_block_free(&blk, cfg->nkeys, cfg->n_payload_cols);
            return VX_OP_OOM;
        }
    }

    l->blocks[l->nblocks++] = blk;
    l->local_rows          += live_n;
    return VX_OP_NEED_MORE_INPUT;
}

/* Transfers worker blocks and strpool buffer handles into global state zero-copy. */
static vx_status_t vx_hjb_combine(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                  void *gstate, void *lstate) {
    (void)sk; (void)ctx;
    vx_hjb_gstate_t *g = (vx_hjb_gstate_t *)gstate;
    vx_hjb_lstate_t *l = (vx_hjb_lstate_t *)lstate;
    if (l->nblocks == 0) return VX_OK;

    pthread_mutex_lock(&g->mu);

    uint32_t buf_base = g->ht->strpool.nbufs;
    for (uint32_t b = 0; b < l->strpool.nbufs; ++b) {
        int32_t     idx = 0;
        vx_owner_t *own = l->strpool.owners ? l->strpool.owners[b] : NULL;
        vx_status_t st  = vx_strpool_attach_external(&g->ht->strpool, l->strpool.ptrs[b],
                                                     l->strpool.lens[b], own, &idx);
        if (st != VX_OK) {
            pthread_mutex_unlock(&g->mu);
            return st;
        }
    }

    uint32_t need = g->nblocks + l->nblocks;
    if (need > g->cap_blocks) {
        uint32_t ncap = g->cap_blocks ? g->cap_blocks : 16u;
        while (ncap < need) ncap *= 2u;
        vx_hj_block_t *nb = (vx_hj_block_t *)realloc(g->blocks, (size_t)ncap * sizeof(*nb));
        if (!nb) {
            pthread_mutex_unlock(&g->mu);
            return VX_ERR_OOM;
        }
        g->blocks     = nb;
        g->cap_blocks = ncap;
    }

    for (uint32_t i = 0; i < l->nblocks; ++i) {
        l->blocks[i].str_buf_offset = buf_base;
        g->blocks[g->nblocks++]     = l->blocks[i];
    }
    g->total_rows += l->local_rows;
    l->nblocks     = 0;
    l->local_rows  = 0;

    pthread_mutex_unlock(&g->mu);
    return VX_OK;
}

static void vx_hj_copy_validity_off(uint64_t *dst, uint32_t dst_off,
                                    const uint64_t *src, uint32_t nrows) {
    if (!src) {
        for (uint32_t i = 0; i < nrows; ++i) {
            uint32_t bit = dst_off + i;
            dst[bit >> 6] |= (1ULL << (bit & 63u));
        }
        return;
    }
    for (uint32_t i = 0; i < nrows; ++i) {
        if ((src[i >> 6] >> (i & 63u)) & 1ULL) {
            uint32_t bit = dst_off + i;
            dst[bit >> 6] |= (1ULL << (bit & 63u));
        }
    }
}

static vx_status_t vx_hjb_merge_col_group(const vx_hjb_gstate_t *g, bool is_key, uint32_t col_idx,
                                          vx_type_t type, uint32_t total_rows, vx_hj_col_t *dst,
                                          size_t *bytes_acc) {
    const size_t elem_sz   = vx_hj_elem_size(type);
    bool         any_nulls = false;
    if (!is_key) {
        for (uint32_t b = 0; b < g->nblocks; ++b) {
            if (g->blocks[b].pays[col_idx].has_nulls) { any_nulls = true; break; }
        }
    }

    dst->type      = type;
    dst->has_nulls = any_nulls ? 1u : 0u;
    dst->data      = malloc((size_t)total_rows * elem_sz);
    if (!dst->data) return VX_ERR_OOM;
    *bytes_acc += (size_t)total_rows * elem_sz;

    if (any_nulls) {
        uint32_t nw   = (total_rows + 63u) >> 6;
        dst->validity = (uint64_t *)calloc(nw, sizeof(uint64_t));
        if (!dst->validity) return VX_ERR_OOM;
        *bytes_acc += (size_t)nw * sizeof(uint64_t);
    }

    uint32_t row_off = 0;
    for (uint32_t b = 0; b < g->nblocks; ++b) {
        const vx_hj_block_t *blk = &g->blocks[b];
        const vx_hj_col_t   *sc  = is_key ? &blk->keys[col_idx] : &blk->pays[col_idx];
        if (type == VX_TYPE_STR) {
            vx_strview_t       *dsv = (vx_strview_t *)dst->data + row_off;
            const vx_strview_t *ssv = (const vx_strview_t *)sc->data;
            for (uint32_t i = 0; i < blk->nrows; ++i) {
                vx_strview_t sv = ssv[i];
                if (!vx_strview_is_inline(sv)) sv.out.buf_idx += (int32_t)blk->str_buf_offset;
                dsv[i] = sv;
            }
        } else {
            memcpy((uint8_t *)dst->data + (size_t)row_off * elem_sz,
                   sc->data, (size_t)blk->nrows * elem_sz);
        }
        if (any_nulls) {
            vx_hj_copy_validity_off(dst->validity, row_off, sc->validity, blk->nrows);
        }
        row_off += blk->nrows;
    }
    return VX_OK;
}

static vx_status_t vx_hjb_finalize(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate) {
    (void)sk; (void)ctx;
    vx_hjb_gstate_t         *g   = (vx_hjb_gstate_t *)gstate;
    const vx_hj_build_cfg_t *cfg = g->cfg;
    vx_hj_table_t           *ht  = g->ht;

    if (g->total_rows == 0) {
        ht->nrows = 0;
        if (cfg->n_payload_cols > 0) {
            ht->pay_cols = (vx_hj_col_t *)calloc(cfg->n_payload_cols, sizeof(*ht->pay_cols));
            if (!ht->pay_cols) return VX_ERR_OOM;
            for (uint32_t c = 0; c < cfg->n_payload_cols; ++c) {
                ht->pay_cols[c].type = cfg->payload_types[c];
            }
        }
        return VX_OK;
    }
    if (g->total_rows >= (uint64_t)UINT32_MAX) return VX_ERR_OOM;

    const uint32_t total = (uint32_t)g->total_rows;
    uint32_t       bcap  = 16u;
    while (bcap < total * 2u && bcap < (1u << 30)) bcap <<= 1;

    ht->nrows       = total;
    ht->bucket_mask = bcap - 1u;
    ht->head        = (uint32_t *)calloc(bcap, sizeof(*ht->head));
    ht->next        = (uint32_t *)calloc(total, sizeof(*ht->next));
    ht->hashes      = (uint64_t *)malloc((size_t)total * sizeof(*ht->hashes));
    ht->key_cols    = (vx_hj_col_t *)calloc(cfg->nkeys, sizeof(*ht->key_cols));
    if (cfg->n_payload_cols > 0) {
        ht->pay_cols = (vx_hj_col_t *)calloc(cfg->n_payload_cols, sizeof(*ht->pay_cols));
    }
    if (!ht->head || !ht->next || !ht->hashes || !ht->key_cols ||
        (cfg->n_payload_cols > 0 && !ht->pay_cols)) {
        return VX_ERR_OOM;
    }

    size_t bytes = (size_t)bcap * sizeof(uint32_t) +
                   (size_t)total * (sizeof(uint32_t) + sizeof(uint64_t));

    /* Flatten hashes and build intrusive hash chains */
    uint32_t row_off = 0;
    for (uint32_t b = 0; b < g->nblocks; ++b) {
        const vx_hj_block_t *blk = &g->blocks[b];
        memcpy(ht->hashes + row_off, blk->hashes, (size_t)blk->nrows * sizeof(uint64_t));
        for (uint32_t i = 0; i < blk->nrows; ++i) {
            uint32_t r   = row_off + i;
            uint32_t bkt = (uint32_t)ht->hashes[r] & ht->bucket_mask;
            ht->next[r]   = ht->head[bkt];
            ht->head[bkt] = r + 1u;
        }
        row_off += blk->nrows;
    }

    /* Flatten key and payload columns */
    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        vx_status_t st = vx_hjb_merge_col_group(g, true, k, cfg->keys[k].type,
                                                total, &ht->key_cols[k], &bytes);
        if (st != VX_OK) return st;
    }
    for (uint32_t c = 0; c < cfg->n_payload_cols; ++c) {
        vx_status_t st = vx_hjb_merge_col_group(g, false, c, cfg->payload_types[c],
                                                total, &ht->pay_cols[c], &bytes);
        if (st != VX_OK) return st;
    }

    ht->bytes_used = bytes;

    /* Release staging blocks immediately to halve peak build memory */
    for (uint32_t b = 0; b < g->nblocks; ++b) {
        vx_hj_block_free(&g->blocks[b], cfg->nkeys, cfg->n_payload_cols);
    }
    free(g->blocks);
    g->blocks     = NULL;
    g->nblocks    = 0;
    g->cap_blocks = 0;
    return VX_OK;
}

static void vx_hjb_destroy_local(void *lstate) {
    vx_hjb_lstate_t *l = (vx_hjb_lstate_t *)lstate;
    if (!l) return;
    for (uint32_t b = 0; b < l->nblocks; ++b) {
        vx_hj_block_free(&l->blocks[b], l->cfg->nkeys, l->cfg->n_payload_cols);
    }
    free(l->blocks);
    vx_strpool_destroy(&l->strpool);
    free(l);
}

static void vx_hjb_destroy_global(void *gstate) {
    vx_hjb_gstate_t *g = (vx_hjb_gstate_t *)gstate;
    if (!g) return;
    for (uint32_t b = 0; b < g->nblocks; ++b) {
        vx_hj_block_free(&g->blocks[b], g->cfg->nkeys, g->cfg->n_payload_cols);
    }
    free(g->blocks);
    if (g->ht) vx_owner_release(&g->ht->hdr);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

vx_hj_table_t *vx_hj_build_take(void *sink_gstate) {
    vx_hjb_gstate_t *g = (vx_hjb_gstate_t *)sink_gstate;
    if (!g) return NULL;
    vx_hj_table_t *ht = g->ht;
    g->ht = NULL;
    vx_hjb_destroy_global(g);
    return ht;
}

const vx_sink_vtbl_t VX_SINK_HASH_JOIN_BUILD = {
    .name           = "HashJoinBuild",
    .init_global    = vx_hjb_init_global,
    .init_local     = vx_hjb_init_local,
    .sink           = vx_hjb_sink_consume,
    .combine        = vx_hjb_combine,
    .finalize       = vx_hjb_finalize,
    .destroy_local  = vx_hjb_destroy_local,
    .destroy_global = vx_hjb_destroy_global
};
