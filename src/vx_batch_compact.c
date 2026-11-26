#include <limits.h>
#include <string.h>
#include "vx_batch.h"

static inline bool vx_bit_test(const uint64_t *bits, uint32_t idx) {
    return !bits || ((bits[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
}

static inline uint32_t vx_popcnt64(uint64_t x) {
#if defined(_MSC_VER)
    return (uint32_t)__popcnt64(x);
#else
    return (uint32_t)__builtin_popcountll(x);
#endif
}

static inline size_t vx_type_elem_size(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return sizeof(uint8_t);
    case VX_TYPE_I64:  return sizeof(int64_t);
    case VX_TYPE_U64:  return sizeof(uint64_t);
    case VX_TYPE_F64:  return sizeof(double);
    case VX_TYPE_STR:  return sizeof(vx_strview_t);
    }
    return 0;
}

/* Zero-copy batch transfer: retains every column and row_src owner independently. */
void vx_batch_forward_shallow(const vx_batch_t *in, vx_batch_t *out) {
    vx_batch_reset(out);
    out->sel = in->sel;
    out->row_src = in->row_src;
    out->row_src.src_owner = vx_owner_retain(in->row_src.src_owner);

    const uint32_t nc = (in->ncols < out->cap_cols) ? in->ncols : out->cap_cols;
    out->ncols = nc;
    for (uint32_t c = 0; c < nc; ++c) {
        out->cols[c] = in->cols[c];
        out->cols[c].owner = vx_owner_retain(in->cols[c].owner);
    }
}

/* Packs validity bitmap for `sel` into dst_arena. Sets *out_val = NULL if no nulls survive. */
static vx_status_t vx_compact_validity(const uint64_t *src_val, const vx_sel_t *sel,
                                       vx_arena_t *dst_arena, uint64_t **out_val,
                                       uint8_t *out_has_nulls) {
    const uint32_t n = sel->count;
    if (!src_val || n == 0) {
        *out_val       = NULL;
        *out_has_nulls = 0;
        return VX_OK;
    }

    const uint32_t nwords = (n + 63u) >> 6;
    uint64_t *dst = (uint64_t *)vx_arena_calloc(dst_arena, nwords, sizeof(uint64_t));
    if (!dst) return VX_ERR_OOM;

    uint32_t valid_n = 0;
    if (sel->is_identity) {
        memcpy(dst, src_val, (size_t)nwords * sizeof(uint64_t));
        uint32_t rem = n & 63u;
        if (rem) dst[nwords - 1u] &= (1ULL << rem) - 1ULL;
        for (uint32_t w = 0; w < nwords; ++w) valid_n += vx_popcnt64(dst[w]);
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            if (vx_bit_test(src_val, sel->idx[i])) {
                dst[i >> 6] |= (1ULL << (i & 63u));
                ++valid_n;
            }
        }
    }

    if (valid_n == n) {
        *out_val       = NULL;
        *out_has_nulls = 0;
    } else {
        *out_val       = dst;
        *out_has_nulls = 1;
    }
    return VX_OK;
}

/* Compacts VX_TYPE_STR column and packs surviving out-of-line bytes into dst_arena. */
static vx_status_t vx_compact_str_vec(const vx_vec_t *in, const vx_sel_t *sel,
                                      uint64_t *dst_val, uint8_t has_nulls,
                                      vx_arena_t *dst_arena, vx_vec_t *out) {
    const uint32_t n = sel->count;
    vx_strview_t *dst = (vx_strview_t *)vx_arena_alloc(dst_arena, (size_t)n * sizeof(vx_strview_t));
    if (!dst) return VX_ERR_OOM;

    const vx_strview_t *src_sv = (in->kind == VX_VEC_DICT)
        ? (const vx_strview_t *)in->dict_vals
        : (const vx_strview_t *)in->data;
    const uint32_t *codes = (in->kind == VX_VEC_DICT) ? (const uint32_t *)in->data : NULL;

    /* Pass 1: count required out-of-line string buffers (handles >2 GiB batch safely). */
    uint32_t nbufs     = 0;
    size_t   cur_bytes = 0;
    bool     has_ool   = false;

    for (uint32_t i = 0; i < n; ++i) {
        if (has_nulls && !vx_bit_test(dst_val, i)) continue;
        uint32_t r  = sel->is_identity ? i : sel->idx[i];
        uint32_t si = (in->kind == VX_VEC_CONST) ? 0u : (codes ? codes[r] : r);
        int32_t  ln = src_sv[si].inl.len;
        if (ln > VX_STR_INLINE_MAX) {
            has_ool = true;
            if (nbufs == 0) {
                nbufs = 1;
                cur_bytes = (size_t)ln;
            } else if (cur_bytes + (size_t)ln > (size_t)INT32_MAX) {
                ++nbufs;
                cur_bytes = (size_t)ln;
            } else {
                cur_bytes += (size_t)ln;
            }
            if (in->kind == VX_VEC_CONST) break; /* store constant string payload once */
        }
    }

    vx_strpool_t *pool = NULL;
    uint8_t     **bufs = NULL;
    int64_t      *lens = NULL;

    if (has_ool) {
        pool = (vx_strpool_t *)vx_arena_calloc(dst_arena, 1u, sizeof(*pool));
        bufs = (uint8_t **)vx_arena_calloc(dst_arena, nbufs, sizeof(*bufs));
        lens = (int64_t *)vx_arena_calloc(dst_arena, nbufs, sizeof(*lens));
        if (!pool || !bufs || !lens) return VX_ERR_OOM;

        /* Compute exact length per buffer, then allocate each buffer in dst_arena. */
        uint32_t bi = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (has_nulls && !vx_bit_test(dst_val, i)) continue;
            uint32_t r  = sel->is_identity ? i : sel->idx[i];
            uint32_t si = (in->kind == VX_VEC_CONST) ? 0u : (codes ? codes[r] : r);
            int32_t  ln = src_sv[si].inl.len;
            if (ln > VX_STR_INLINE_MAX) {
                if (lens[bi] + (int64_t)ln > (int64_t)INT32_MAX) ++bi;
                lens[bi] += (int64_t)ln;
                if (in->kind == VX_VEC_CONST) break;
            }
        }
        for (uint32_t b = 0; b < nbufs; ++b) {
            bufs[b] = (uint8_t *)vx_arena_alloc(dst_arena, (size_t)lens[b]);
            if (!bufs[b]) return VX_ERR_OOM;
        }

        pool->ptrs   = (const uint8_t **)bufs;
        pool->lens   = lens;
        pool->owners = NULL; /* out->owner pins dst_arena directly (avoids refcount cycle) */
        pool->nbufs  = nbufs;
        pool->cap    = nbufs;
    }

    /* Pass 2: materialize dense vx_strview_t slots and copy out-of-line bytes. */
    if (in->kind == VX_VEC_CONST) {
        vx_strview_t v = src_sv[0];
        if (!has_nulls && v.inl.len > VX_STR_INLINE_MAX) {
            const uint8_t *p = vx_strview_ptr(&v, in->strpool);
            memcpy(bufs[0], p, (size_t)v.inl.len);
            v.out.buf_idx = 0;
            v.out.offset  = 0;
        } else if (has_nulls) {
            memset(&v, 0, sizeof(v));
        }
        for (uint32_t i = 0; i < n; ++i) dst[i] = v;
    } else {
        uint32_t bi  = 0;
        int32_t  off = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (has_nulls && !vx_bit_test(dst_val, i)) {
                memset(&dst[i], 0, sizeof(dst[i]));
                continue;
            }
            uint32_t     r  = sel->is_identity ? i : sel->idx[i];
            uint32_t     si = codes ? codes[r] : r;
            vx_strview_t sv = src_sv[si];
            if (sv.inl.len > VX_STR_INLINE_MAX) {
                if ((int64_t)off + (int64_t)sv.inl.len > lens[bi]) {
                    ++bi;
                    off = 0;
                }
                const uint8_t *p = vx_strview_ptr(&sv, in->strpool);
                memcpy(bufs[bi] + off, p, (size_t)sv.inl.len);
                sv.out.buf_idx = (int32_t)bi;
                sv.out.offset  = off;
                off           += sv.inl.len;
            }
            dst[i] = sv;
        }
    }

    out->type      = VX_TYPE_STR;
    out->kind      = VX_VEC_FLAT;
    out->has_nulls = has_nulls;
    out->phys_len  = n;
    out->dict_size = 0;
    out->data      = dst;
    out->dict_vals = NULL;
    out->validity  = dst_val;
    out->strpool   = pool;
    out->owner     = vx_owner_retain(&dst_arena->hdr);
    return VX_OK;
}

/* Compacts a single vector using `sel` into a dense VX_VEC_FLAT backed by dst_arena. */
vx_status_t vx_vec_compact(const vx_vec_t *in, const vx_sel_t *sel,
                           vx_arena_t *dst_arena, vx_vec_t *out) {
    if (!in || !sel || !dst_arena || !out) return VX_ERR_INVALID;
    if (in->kind == VX_VEC_DEFERRED) return VX_ERR_INVALID;

    const uint32_t n = sel->count;
    memset(out, 0, sizeof(*out));
    out->type = in->type;
    out->kind = VX_VEC_FLAT;
    if (n == 0) return VX_OK;

    uint64_t *dst_val   = NULL;
    uint8_t   has_nulls = 0;

    if (in->kind == VX_VEC_CONST) {
        if (in->has_nulls && in->validity && ((in->validity[0] & 1ULL) == 0)) {
            const uint32_t nwords = (n + 63u) >> 6;
            dst_val = (uint64_t *)vx_arena_calloc(dst_arena, nwords, sizeof(uint64_t));
            if (!dst_val) return VX_ERR_OOM;
            has_nulls = 1;
        }
    } else if (in->has_nulls && in->validity) {
        vx_status_t st = vx_compact_validity(in->validity, sel, dst_arena, &dst_val, &has_nulls);
        if (st != VX_OK) return st;
    }

    if (in->type == VX_TYPE_STR) {
        return vx_compact_str_vec(in, sel, dst_val, has_nulls, dst_arena, out);
    }

    const size_t elem_sz = vx_type_elem_size(in->type);
    void        *dst     = vx_arena_alloc(dst_arena, (size_t)n * elem_sz);
    if (!dst) return VX_ERR_OOM;

    if (in->kind == VX_VEC_CONST) {
        if (has_nulls) {
            memset(dst, 0, (size_t)n * elem_sz);
        } else if (elem_sz == 1u) {
            memset(dst, *(const uint8_t *)in->data, n);
        } else {
            uint64_t  v64 = *(const uint64_t *)in->data;
            uint64_t *d64 = (uint64_t *)dst;
            for (uint32_t i = 0; i < n; ++i) d64[i] = v64;
        }
    } else if (in->kind == VX_VEC_FLAT && sel->is_identity) {
        memcpy(dst, in->data, (size_t)n * elem_sz);
    } else if (in->kind == VX_VEC_FLAT) {
        if (elem_sz == 1u) {
            const uint8_t *s = (const uint8_t *)in->data;
            uint8_t       *d = (uint8_t *)dst;
            for (uint32_t i = 0; i < n; ++i) d[i] = s[sel->idx[i]];
        } else {
            const uint64_t *s = (const uint64_t *)in->data;
            uint64_t       *d = (uint64_t *)dst;
            for (uint32_t i = 0; i < n; ++i) d[i] = s[sel->idx[i]];
        }
    } else { /* VX_VEC_DICT (M15) */
        const uint32_t *codes = (const uint32_t *)in->data;
        if (elem_sz == 1u) {
            const uint8_t *dv = (const uint8_t *)in->dict_vals;
            uint8_t       *d  = (uint8_t *)dst;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t r = sel->is_identity ? i : sel->idx[i];
                d[i] = (has_nulls && !vx_bit_test(dst_val, i)) ? 0u : dv[codes[r]];
            }
        } else {
            const uint64_t *dv = (const uint64_t *)in->dict_vals;
            uint64_t       *d  = (uint64_t *)dst;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t r = sel->is_identity ? i : sel->idx[i];
                d[i] = (has_nulls && !vx_bit_test(dst_val, i)) ? 0ULL : dv[codes[r]];
            }
        }
    }

    out->has_nulls = has_nulls;
    out->phys_len  = n;
    out->data      = dst;
    out->validity  = dst_val;
    out->owner     = vx_owner_retain(&dst_arena->hdr);
    return VX_OK;
}

vx_status_t vx_batch_compact(const vx_batch_t *in, vx_batch_t *out, vx_arena_t *dst_arena) {
    if (!in || !out || !dst_arena || out->cap_cols < in->ncols) {
        return VX_ERR_INVALID;
    }

    vx_batch_reset(out);
    out->ncols           = in->ncols;
    out->sel.count       = in->sel.count;
    out->sel.is_identity = true;

    for (uint32_t c = 0; c < in->ncols; ++c) {
        vx_status_t st = vx_vec_compact(&in->cols[c], &in->sel, dst_arena, &out->cols[c]);
        if (st != VX_OK) {
            vx_batch_reset(out); /* release refcounts on 0..c-1 on OOM */
            return st;
        }
    }
    return VX_OK;
}

