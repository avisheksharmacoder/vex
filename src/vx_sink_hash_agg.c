#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vx_sink_hash_agg.h"

/* --- Hashing, Canonicalization & IEEE-754 Helpers --- */
static inline uint64_t vx_mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline uint64_t vx_hash_combine(uint64_t h, uint64_t k) {
    return vx_mix64(h ^ (k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}

static inline uint64_t vx_canon_f64_bits(double d) {
    if (d == 0.0) return 0ULL;                 /* -0.0 and +0.0 group together */
    if (isnan(d)) return 0x7ff8000000000000ULL;/* all NaN payloads group together */
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static inline uint64_t vx_hash_bytes(const uint8_t *p, int32_t len) {
    uint64_t h = 14695981039346656037ULL ^ (uint64_t)(uint32_t)len;
    for (int32_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return vx_mix64(h);
}

static inline double vx_agg_f64_min(double a, double b) {
    if (isnan(a) || isnan(b)) return (double)NAN;
    if (a < b) return a;
    if (b < a) return b;
    return signbit(a) ? a : b;
}

static inline double vx_agg_f64_max(double a, double b) {
    if (isnan(a) || isnan(b)) return (double)NAN;
    if (a > b) return a;
    if (b > a) return b;
    return signbit(a) ? b : a;
}

/* Neumaier compensated addition; guards non-finite operands against Inf - Inf = NaN. */
static inline void vx_f64_neumaier_add(double *sum, double *comp, double x) {
    if (!isfinite(*sum) || !isfinite(x)) {
        *sum += x;
        *comp = 0.0;
        return;
    }
    double t = *sum + x;
    if (!isfinite(t)) {
        *sum  = t;
        *comp = 0.0;
        return;
    }
    if (fabs(*sum) >= fabs(x)) {
        *comp += (*sum - t) + x;
    } else {
        *comp += (x - t) + *sum;
    }
    *sum = t;
}

static inline bool vx_vec_row_valid(const vx_vec_t *v, uint32_t phys_row) {
    if (!v->has_nulls || !v->validity) return true;
    uint32_t idx = (v->kind == VX_VEC_CONST) ? 0u : phys_row;
    return ((v->validity[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
}

static inline uint32_t vx_vec_val_idx(const vx_vec_t *v, uint32_t phys_row) {
    if (v->kind == VX_VEC_CONST) return 0u;
    if (v->kind == VX_VEC_DICT)  return ((const uint32_t *)v->data)[phys_row];
    return phys_row;
}

static inline const void *vx_vec_val_base(const vx_vec_t *v) {
    return (v->kind == VX_VEC_DICT) ? v->dict_vals : v->data;
}

/* --- vx_agg_ht_t Core --- */
static vx_status_t vx_agg_ht_init(vx_agg_ht_t *ht, uint32_t nkeys, uint32_t naggs) {
    memset(ht, 0, sizeof(*ht));
    ht->nkeys = nkeys;
    ht->naggs = naggs;
    return vx_strpool_init(&ht->strpool, 4u);
}

static void vx_agg_ht_destroy(vx_agg_ht_t *ht) {
    free(ht->buckets);
    free(ht->hashes);
    free(ht->keys);
    free(ht->aggs);
    vx_strpool_destroy(&ht->strpool);
    memset(ht, 0, sizeof(*ht));
}

static vx_status_t vx_agg_ht_rehash(vx_agg_ht_t *ht, uint32_t new_cap) {
    vx_agg_bucket_t *nb = (vx_agg_bucket_t *)calloc(new_cap, sizeof(*nb));
    if (!nb) return VX_ERR_OOM;

    const uint32_t mask = new_cap - 1u;
    for (uint32_t gid = 0; gid < ht->ngroups; ++gid) {
        uint64_t h   = ht->hashes[gid];
        uint32_t pos = (uint32_t)h & mask;
        while (nb[pos].gid_plus1 != 0) pos = (pos + 1u) & mask;
        nb[pos].hash      = h;
        nb[pos].gid_plus1 = gid + 1u;
    }

    free(ht->buckets);
    ht->buckets = nb;
    ht->cap     = new_cap;
    return VX_OK;
}

static vx_status_t vx_agg_ht_reserve_group(vx_agg_ht_t *ht) {
    if (ht->ngroups < ht->cap_groups) return VX_OK;
    uint32_t ncap = ht->cap_groups ? ht->cap_groups * 2u : 32u;

    uint64_t *nh = (uint64_t *)realloc(ht->hashes, (size_t)ncap * sizeof(*nh));
    if (!nh) return VX_ERR_OOM;
    ht->hashes = nh;

    if (ht->nkeys > 0) {
        vx_key_val_t *nk = (vx_key_val_t *)realloc(
            ht->keys, (size_t)ncap * (size_t)ht->nkeys * sizeof(*nk));
        if (!nk) return VX_ERR_OOM;
        ht->keys = nk;
    }
    if (ht->naggs > 0) {
        vx_agg_state_t *na = (vx_agg_state_t *)realloc(
            ht->aggs, (size_t)ncap * (size_t)ht->naggs * sizeof(*na));
        if (!na) return VX_ERR_OOM;
        ht->aggs = na;
    }

    ht->cap_groups = ncap;
    return VX_OK;
}

static uint64_t vx_key_hash(const vx_key_spec_t *specs, const vx_key_val_t *kv,
                            uint32_t nkeys, const vx_strpool_t *const *strpools) {
    uint64_t h = 0x517cc1b727220a95ULL;
    for (uint32_t k = 0; k < nkeys; ++k) {
        if (kv[k].is_null) {
            h = vx_hash_combine(h, 0xdeadbeef00000000ULL ^ k);
            continue;
        }
        uint64_t kh = 0;
        switch (specs[k].type) {
        case VX_TYPE_BOOL: kh = vx_mix64(kv[k].v.b); break;
        case VX_TYPE_I64:  kh = vx_mix64((uint64_t)kv[k].v.i64); break;
        case VX_TYPE_U64:  kh = vx_mix64(kv[k].v.u64); break;
        case VX_TYPE_F64:  kh = vx_mix64(vx_canon_f64_bits(kv[k].v.f64)); break;
        case VX_TYPE_STR: {
            const uint8_t *p = vx_strview_ptr(&kv[k].v.str, strpools ? strpools[k] : NULL);
            kh = vx_hash_bytes(p, kv[k].v.str.inl.len);
            break;
        }
        }
        h = vx_hash_combine(h, kh);
    }
    return h;
}

static bool vx_key_eq(const vx_key_spec_t *specs, uint32_t nkeys,
                      const vx_key_val_t *a, const vx_strpool_t *pa,
                      const vx_key_val_t *b, const vx_strpool_t *const *pbs) {
    for (uint32_t k = 0; k < nkeys; ++k) {
        if (a[k].is_null != b[k].is_null) return false;
        if (a[k].is_null) continue;
        switch (specs[k].type) {
        case VX_TYPE_BOOL: if (a[k].v.b != b[k].v.b) return false; break;
        case VX_TYPE_I64:  if (a[k].v.i64 != b[k].v.i64) return false; break;
        case VX_TYPE_U64:  if (a[k].v.u64 != b[k].v.u64) return false; break;
        case VX_TYPE_F64:
            if (vx_canon_f64_bits(a[k].v.f64) != vx_canon_f64_bits(b[k].v.f64)) return false;
            break;
        case VX_TYPE_STR:
            if (!vx_strview_eq(a[k].v.str, pa, b[k].v.str, pbs ? pbs[k] : NULL)) return false;
            break;
        }
    }
    return true;
}

/* Finds or inserts group key; copies out-of-line key strings into ht->strpool on insert. */
static vx_status_t vx_agg_ht_get_or_insert(vx_agg_ht_t *ht, const vx_key_spec_t *specs,
                                           const vx_key_val_t *key,
                                           const vx_strpool_t *const *key_pools,
                                           uint64_t hash, uint32_t *out_gid) {
    if (ht->nkeys == 0) {
        if (ht->ngroups == 0) {
            vx_status_t st = vx_agg_ht_reserve_group(ht);
            if (st != VX_OK) return st;
            ht->hashes[0] = 0;
            if (ht->naggs > 0) memset(ht->aggs, 0, (size_t)ht->naggs * sizeof(*ht->aggs));
            ht->ngroups = 1u;
        }
        *out_gid = 0u;
        return VX_OK;
    }

    if (ht->cap == 0 || ((size_t)(ht->ngroups + 1u) * 10u >= (size_t)ht->cap * 7u)) {
        uint32_t ncap = ht->cap ? ht->cap * 2u : 64u;
        vx_status_t st = vx_agg_ht_rehash(ht, ncap);
        if (st != VX_OK) return st;
    }

    const uint32_t mask = ht->cap - 1u;
    uint32_t       pos  = (uint32_t)hash & mask;
    for (;;) {
        vx_agg_bucket_t *b = &ht->buckets[pos];
        if (b->gid_plus1 == 0) {
            vx_status_t st = vx_agg_ht_reserve_group(ht);
            if (st != VX_OK) return st;

            uint32_t      gid = ht->ngroups;
            vx_key_val_t *dst = &ht->keys[(size_t)gid * ht->nkeys];
            for (uint32_t k = 0; k < ht->nkeys; ++k) {
                dst[k] = key[k];
                if (!key[k].is_null && specs[k].type == VX_TYPE_STR &&
                    !vx_strview_is_inline(key[k].v.str)) {
                    const uint8_t *p = vx_strview_ptr(&key[k].v.str,
                                                      key_pools ? key_pools[k] : NULL);
                    st = vx_strpool_append(&ht->strpool, p, key[k].v.str.inl.len, &dst[k].v.str);
                    if (st != VX_OK) return st;
                }
            }

            ht->hashes[gid] = hash;
            if (ht->naggs > 0) {
                memset(&ht->aggs[(size_t)gid * ht->naggs], 0,
                       (size_t)ht->naggs * sizeof(*ht->aggs));
            }
            ht->ngroups++;
            b->hash      = hash;
            b->gid_plus1 = gid + 1u;
            *out_gid     = gid;
            return VX_OK;
        }
        if (b->hash == hash) {
            uint32_t gid = b->gid_plus1 - 1u;
            if (vx_key_eq(specs, ht->nkeys, &ht->keys[(size_t)gid * ht->nkeys],
                          &ht->strpool, key, key_pools)) {
                *out_gid = gid;
                return VX_OK;
            }
        }
        pos = (pos + 1u) & mask;
    }
}

/* --- Per-Row Update & Partition Combine --- */
static vx_status_t vx_agg_update_row(vx_agg_state_t *st, const vx_agg_spec_t *sp,
                                     const vx_vec_t *col, uint32_t phys_row,
                                     vx_strpool_t *dst_pool) {
    if (sp->kind == VX_AGG_COUNT_STAR) {
        st->count++;
        return VX_OK;
    }
    if (!vx_vec_row_valid(col, phys_row)) return VX_OK;

    if (sp->kind == VX_AGG_COUNT) {
        st->count++;
        return VX_OK;
    }

    const uint32_t vi   = vx_vec_val_idx(col, phys_row);
    const void    *base = vx_vec_val_base(col);
    const bool     first = (st->count == 0);
    st->count++;

    switch (sp->type) {
    case VX_TYPE_I64: {
        int64_t x = ((const int64_t *)base)[vi];
        if (sp->kind == VX_AGG_SUM) {
            st->v.i64 = (int64_t)((uint64_t)st->v.i64 + (uint64_t)x);
        } else if (sp->kind == VX_AGG_MIN) {
            st->v.i64 = first ? x : (x < st->v.i64 ? x : st->v.i64);
        } else {
            st->v.i64 = first ? x : (x > st->v.i64 ? x : st->v.i64);
        }
        break;
    }
    case VX_TYPE_U64: {
        uint64_t x = ((const uint64_t *)base)[vi];
        if (sp->kind == VX_AGG_SUM) {
            st->v.u64 += x;
        } else if (sp->kind == VX_AGG_MIN) {
            st->v.u64 = first ? x : (x < st->v.u64 ? x : st->v.u64);
        } else {
            st->v.u64 = first ? x : (x > st->v.u64 ? x : st->v.u64);
        }
        break;
    }
    case VX_TYPE_F64: {
        double x = ((const double *)base)[vi];
        if (sp->kind == VX_AGG_SUM) {
            if (first) {
                st->v.f64_sum.sum  = x;
                st->v.f64_sum.comp = 0.0;
            } else {
                vx_f64_neumaier_add(&st->v.f64_sum.sum, &st->v.f64_sum.comp, x);
            }
        } else if (sp->kind == VX_AGG_MIN) {
            st->v.f64 = first ? x : vx_agg_f64_min(st->v.f64, x);
        } else {
            st->v.f64 = first ? x : vx_agg_f64_max(st->v.f64, x);
        }
        break;
    }
    case VX_TYPE_BOOL: {
        uint64_t x = ((const uint8_t *)base)[vi] ? 1ULL : 0ULL;
        if (sp->kind == VX_AGG_SUM)      st->v.u64 += x;
        else if (sp->kind == VX_AGG_MIN) st->v.u64 = first ? x : (x < st->v.u64 ? x : st->v.u64);
        else                             st->v.u64 = first ? x : (x > st->v.u64 ? x : st->v.u64);
        break;
    }
    case VX_TYPE_STR: {
        vx_strview_t sv = ((const vx_strview_t *)base)[vi];
        bool take = first;
        if (!first) {
            int cmp = vx_strview_cmp(sv, col->strpool, st->v.str, dst_pool);
            take = (sp->kind == VX_AGG_MIN) ? (cmp < 0) : (cmp > 0);
        }
        if (take) {
            if (vx_strview_is_inline(sv)) {
                st->v.str = sv;
            } else {
                const uint8_t *p = vx_strview_ptr(&sv, col->strpool);
                vx_status_t err = vx_strpool_append(dst_pool, p, sv.inl.len, &st->v.str);
                if (err != VX_OK) return err;
            }
        }
        break;
    }
    }
    return VX_OK;
}

static vx_status_t vx_agg_merge_state(vx_agg_state_t *dst, const vx_agg_state_t *src,
                                      const vx_agg_spec_t *sp,
                                      vx_strpool_t *dst_pool, const vx_strpool_t *src_pool) {
    if (src->count == 0) return VX_OK;
    const bool first = (dst->count == 0);
    dst->count += src->count;

    if (sp->kind == VX_AGG_COUNT_STAR || sp->kind == VX_AGG_COUNT) return VX_OK;

    switch (sp->type) {
    case VX_TYPE_I64:
        if (sp->kind == VX_AGG_SUM)
            dst->v.i64 = (int64_t)((uint64_t)dst->v.i64 + (uint64_t)src->v.i64);
        else if (sp->kind == VX_AGG_MIN)
            dst->v.i64 = first ? src->v.i64 : (src->v.i64 < dst->v.i64 ? src->v.i64 : dst->v.i64);
        else
            dst->v.i64 = first ? src->v.i64 : (src->v.i64 > dst->v.i64 ? src->v.i64 : dst->v.i64);
        break;
    case VX_TYPE_U64:
    case VX_TYPE_BOOL:
        if (sp->kind == VX_AGG_SUM)      dst->v.u64 += src->v.u64;
        else if (sp->kind == VX_AGG_MIN) dst->v.u64 = first ? src->v.u64 : (src->v.u64 < dst->v.u64 ? src->v.u64 : dst->v.u64);
        else                             dst->v.u64 = first ? src->v.u64 : (src->v.u64 > dst->v.u64 ? src->v.u64 : dst->v.u64);
        break;
    case VX_TYPE_F64:
        if (sp->kind == VX_AGG_SUM) {
            if (first) {
                dst->v.f64_sum = src->v.f64_sum;
            } else {
                vx_f64_neumaier_add(&dst->v.f64_sum.sum, &dst->v.f64_sum.comp, src->v.f64_sum.sum);
                if (isfinite(dst->v.f64_sum.sum)) {
                    vx_f64_neumaier_add(&dst->v.f64_sum.sum, &dst->v.f64_sum.comp, src->v.f64_sum.comp);
                }
            }
        } else if (sp->kind == VX_AGG_MIN) {
            dst->v.f64 = first ? src->v.f64 : vx_agg_f64_min(dst->v.f64, src->v.f64);
        } else {
            dst->v.f64 = first ? src->v.f64 : vx_agg_f64_max(dst->v.f64, src->v.f64);
        }
        break;
    case VX_TYPE_STR: {
        bool take = first;
        if (!first) {
            int cmp = vx_strview_cmp(src->v.str, src_pool, dst->v.str, dst_pool);
            take = (sp->kind == VX_AGG_MIN) ? (cmp < 0) : (cmp > 0);
        }
        if (take) {
            if (vx_strview_is_inline(src->v.str)) {
                dst->v.str = src->v.str;
            } else {
                const uint8_t *p = vx_strview_ptr(&src->v.str, src_pool);
                vx_status_t err = vx_strpool_append(dst_pool, p, src->v.str.inl.len, &dst->v.str);
                if (err != VX_OK) return err;
            }
        }
        break;
    }
    }
    return VX_OK;
}

/* --- vx_sink_vtbl_t Implementation --- */
static vx_status_t vx_agg_init_global(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void **gstate) {
    (void)ctx;
    const vx_hash_agg_cfg_t *cfg = (const vx_hash_agg_cfg_t *)sk->plan_cfg;
    vx_agg_gstate_t         *g   = (vx_agg_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;

    g->cfg = cfg;
    uint32_t inited = 0;
    for (uint32_t p = 0; p < VX_AGG_PARTITIONS; ++p) {
        if (pthread_mutex_init(&g->parts[p].mu, NULL) != 0) goto fail;
        if (vx_agg_ht_init(&g->parts[p].ht, cfg->nkeys, cfg->naggs) != VX_OK) {
            pthread_mutex_destroy(&g->parts[p].mu);
            goto fail;
        }
        inited = p + 1u;
    }
    *gstate = g;
    return VX_OK;

fail:
    for (uint32_t p = 0; p < inited; ++p) {
        vx_agg_ht_destroy(&g->parts[p].ht);
        pthread_mutex_destroy(&g->parts[p].mu);
    }
    free(g);
    return VX_ERR_OOM;
}

static vx_status_t vx_agg_init_local(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                     void *gstate, void **lstate) {
    (void)ctx; (void)gstate;
    const vx_hash_agg_cfg_t *cfg = (const vx_hash_agg_cfg_t *)sk->plan_cfg;
    vx_agg_lstate_t         *l   = (vx_agg_lstate_t *)calloc(1u, sizeof(*l));
    if (!l) return VX_ERR_OOM;

    l->cfg = cfg;
    if (vx_agg_ht_init(&l->ht, cfg->nkeys, cfg->naggs) != VX_OK) {
        free(l);
        return VX_ERR_OOM;
    }
    if (cfg->nkeys > 0) {
        l->tmp_key = (vx_key_val_t *)calloc(cfg->nkeys, sizeof(*l->tmp_key));
        if (!l->tmp_key) {
            vx_agg_ht_destroy(&l->ht);
            free(l);
            return VX_ERR_OOM;
        }
    }
    *lstate = l;
    return VX_OK;
}

static vx_op_res_t vx_agg_sink_consume(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                       void *gstate, void *lstate, vx_batch_t *in) {
    (void)sk; (void)gstate;
    if (in->sel.count == 0) return VX_OP_NEED_MORE_INPUT;

    vx_agg_lstate_t         *l   = (vx_agg_lstate_t *)lstate;
    const vx_hash_agg_cfg_t *cfg = l->cfg;

    /* Late materialization: only fault in key & agg columns after upstream filters ran */
    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        vx_status_t st = vx_batch_materialize_col(in, cfg->keys[k].batch_col_idx,
                                                  cfg->keys[k].table_col_idx, ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }
    for (uint32_t a = 0; a < cfg->naggs; ++a) {
        if (cfg->aggs[a].kind == VX_AGG_COUNT_STAR) continue;
        vx_status_t st = vx_batch_materialize_col(in, cfg->aggs[a].batch_col_idx,
                                                  cfg->aggs[a].table_col_idx, ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    const vx_strpool_t **key_pools = NULL;
    if (cfg->nkeys > 0) {
        key_pools = (const vx_strpool_t **)vx_arena_alloc(
            ctx->scratch, (size_t)cfg->nkeys * sizeof(*key_pools));
        if (!key_pools) return VX_OP_OOM;
        for (uint32_t k = 0; k < cfg->nkeys; ++k) {
            key_pools[k] = in->cols[cfg->keys[k].batch_col_idx].strpool;
        }
    }

    const uint32_t n  = in->sel.count;
    const bool     id = in->sel.is_identity;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = id ? i : in->sel.idx[i];
        uint64_t h = 0;

        for (uint32_t k = 0; k < cfg->nkeys; ++k) {
            const vx_vec_t *col = &in->cols[cfg->keys[k].batch_col_idx];
            vx_key_val_t   *kv  = &l->tmp_key[k];
            if (!vx_vec_row_valid(col, r)) {
                kv->is_null = 1u;
                memset(&kv->v, 0, sizeof(kv->v));
                continue;
            }
            kv->is_null = 0u;
            uint32_t    vi   = vx_vec_val_idx(col, r);
            const void *base = vx_vec_val_base(col);
            switch (cfg->keys[k].type) {
            case VX_TYPE_BOOL: kv->v.b   = ((const uint8_t *)base)[vi]; break;
            case VX_TYPE_I64:  kv->v.i64 = ((const int64_t *)base)[vi]; break;
            case VX_TYPE_U64:  kv->v.u64 = ((const uint64_t *)base)[vi]; break;
            case VX_TYPE_F64: {
                double d = ((const double *)base)[vi];
                kv->v.f64 = (d == 0.0) ? +0.0 : (isnan(d) ? (double)NAN : d);
                break;
            }
            case VX_TYPE_STR:  kv->v.str = ((const vx_strview_t *)base)[vi]; break;
            }
        }

        if (cfg->nkeys > 0) {
            h = vx_key_hash(cfg->keys, l->tmp_key, cfg->nkeys, key_pools);
        }

        uint32_t gid = 0;
        vx_status_t st = vx_agg_ht_get_or_insert(&l->ht, cfg->keys, l->tmp_key, key_pools, h, &gid);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;

        vx_agg_state_t *dst_aggs = &l->ht.aggs[(size_t)gid * cfg->naggs];
        for (uint32_t a = 0; a < cfg->naggs; ++a) {
            const vx_vec_t *acol = (cfg->aggs[a].kind == VX_AGG_COUNT_STAR)
                ? NULL : &in->cols[cfg->aggs[a].batch_col_idx];
            st = vx_agg_update_row(&dst_aggs[a], &cfg->aggs[a], acol, r, &l->ht.strpool);
            if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
        }
    }

    return VX_OP_NEED_MORE_INPUT;
}

/* Merges thread-local groups into 64 lock-striped global partitions (<= 64 locks per worker). */
static vx_status_t vx_agg_combine(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                  void *gstate, void *lstate) {
    (void)sk; (void)ctx;
    vx_agg_gstate_t         *g   = (vx_agg_gstate_t *)gstate;
    vx_agg_lstate_t         *l   = (vx_agg_lstate_t *)lstate;
    const vx_hash_agg_cfg_t *cfg = g->cfg;
    const uint32_t           ng  = l->ht.ngroups;
    if (ng == 0) return VX_OK;

    const vx_strpool_t **src_pools = NULL;
    if (cfg->nkeys > 0) {
        src_pools = (const vx_strpool_t **)malloc((size_t)cfg->nkeys * sizeof(*src_pools));
        if (!src_pools) return VX_ERR_OOM;
        for (uint32_t k = 0; k < cfg->nkeys; ++k) src_pools[k] = &l->ht.strpool;
    }

    /* Group local gids into 64 partition buckets using an intrusive index list */
    uint32_t  head[VX_AGG_PARTITIONS];
    uint32_t *next = (uint32_t *)malloc((size_t)ng * sizeof(*next));
    if (!next) {
        free(src_pools);
        return VX_ERR_OOM;
    }
    for (uint32_t p = 0; p < VX_AGG_PARTITIONS; ++p) head[p] = UINT32_MAX;

    for (uint32_t gid = 0; gid < ng; ++gid) {
        uint32_t p = (cfg->nkeys == 0) ? 0u : (uint32_t)(l->ht.hashes[gid] >> VX_AGG_PART_SHIFT);
        next[gid]  = head[p];
        head[p]    = gid;
    }

    vx_status_t st = VX_OK;
    for (uint32_t p = 0; p < VX_AGG_PARTITIONS && st == VX_OK; ++p) {
        if (head[p] == UINT32_MAX) continue;
        pthread_mutex_lock(&g->parts[p].mu);
        vx_agg_ht_t *dst_ht = &g->parts[p].ht;

        for (uint32_t lgid = head[p]; lgid != UINT32_MAX; lgid = next[lgid]) {
            uint32_t ggid = 0;
            const vx_key_val_t *lkey = cfg->nkeys ? &l->ht.keys[(size_t)lgid * cfg->nkeys] : NULL;
            st = vx_agg_ht_get_or_insert(dst_ht, cfg->keys, lkey, src_pools,
                                         l->ht.hashes[lgid], &ggid);
            if (st != VX_OK) break;

            vx_agg_state_t       *da = &dst_ht->aggs[(size_t)ggid * cfg->naggs];
            const vx_agg_state_t *sa = &l->ht.aggs[(size_t)lgid * cfg->naggs];
            for (uint32_t a = 0; a < cfg->naggs; ++a) {
                st = vx_agg_merge_state(&da[a], &sa[a], &cfg->aggs[a],
                                        &dst_ht->strpool, &l->ht.strpool);
                if (st != VX_OK) break;
            }
            if (st != VX_OK) break;
        }
        pthread_mutex_unlock(&g->parts[p].mu);
    }

    free(next);
    free(src_pools);
    return st;
}

static vx_status_t vx_agg_finalize(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate) {
    (void)sk; (void)ctx;
    vx_agg_gstate_t *g = (vx_agg_gstate_t *)gstate;
    /* Ungrouped aggregation on an empty table still yields 1 row (count=0, sum/min/max=NULL) */
    if (g->cfg->nkeys == 0 && g->parts[0].ht.ngroups == 0) {
        uint32_t gid = 0;
        return vx_agg_ht_get_or_insert(&g->parts[0].ht, NULL, NULL, NULL, 0, &gid);
    }
    return VX_OK;
}

static void vx_agg_destroy_local(void *lstate) {
    vx_agg_lstate_t *l = (vx_agg_lstate_t *)lstate;
    if (!l) return;
    vx_agg_ht_destroy(&l->ht);
    free(l->tmp_key);
    free(l);
}

static void vx_agg_destroy_global(void *gstate) {
    vx_agg_gstate_t *g = (vx_agg_gstate_t *)gstate;
    if (!g) return;
    for (uint32_t p = 0; p < VX_AGG_PARTITIONS; ++p) {
        vx_agg_ht_destroy(&g->parts[p].ht);
        pthread_mutex_destroy(&g->parts[p].mu);
    }
    free(g);
}

const vx_sink_vtbl_t VX_SINK_HASH_AGG = {
    .name           = "HashAggSink",
    .init_global    = vx_agg_init_global,
    .init_local     = vx_agg_init_local,
    .sink           = vx_agg_sink_consume,
    .combine        = vx_agg_combine,
    .finalize       = vx_agg_finalize,
    .destroy_local  = vx_agg_destroy_local,
    .destroy_global = vx_agg_destroy_global
};

