#include <math.h>
#include <string.h>
#include "vx_simd.h"

static inline uint32_t vx_b_row(const vx_sel_t *sel, uint32_t i) {
    return sel->is_identity ? i : sel->idx[i];
}

static inline bool vx_b_valid(const vx_vec_t *v, uint32_t phys_row) {
    if (!v->has_nulls || !v->validity) return true;
    uint32_t idx = (v->kind == VX_VEC_CONST) ? 0u : phys_row;
    return ((v->validity[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
}

static inline uint32_t vx_b_vidx(const vx_vec_t *v, uint32_t phys_row) {
    if (v->kind == VX_VEC_CONST) return 0u;
    if (v->kind == VX_VEC_DICT)  return ((const uint32_t *)v->data)[phys_row];
    return phys_row;
}

static inline const void *vx_b_base(const vx_vec_t *v) {
    return (v->kind == VX_VEC_DICT) ? v->dict_vals : v->data;
}

static inline void vx_b_set_valid(uint64_t *val, uint32_t i) {
    val[i >> 6] |= (1ULL << (i & 63u));
}

static inline void vx_b_finalize_vec(vx_vec_t *out, vx_type_t type, uint32_t n,
                                     void *data, uint64_t *val, uint32_t valid_n,
                                     vx_strpool_t *pool, vx_arena_t *scratch) {
    out->type      = type;
    out->kind      = VX_VEC_FLAT;
    out->has_nulls = (valid_n < n) ? 1u : 0u;
    out->phys_len  = n;
    out->dict_size = 0;
    out->data      = data;
    out->dict_vals = NULL;
    out->validity  = (valid_n < n) ? val : NULL;
    out->strpool   = pool;
    out->owner     = vx_owner_retain(&scratch->hdr);
}

static vx_status_t vx_b_alloc_val(vx_arena_t *scratch, uint32_t n, uint64_t **out_val) {
    uint32_t nwords = (n + 63u) >> 6;
    *out_val = (uint64_t *)vx_arena_calloc(scratch, nwords, sizeof(uint64_t));
    return *out_val ? VX_OK : VX_ERR_OOM;
}

/* Allocates a single-buffer scratch strpool (owners=NULL; out->owner retains scratch). */
static vx_status_t vx_b_alloc_scratch_strpool(vx_arena_t *scratch, size_t total_ool_bytes,
                                              vx_strpool_t **out_pool, uint8_t **out_buf) {
    *out_pool = NULL;
    *out_buf  = NULL;
    if (total_ool_bytes == 0) return VX_OK;
    if (total_ool_bytes > (size_t)INT32_MAX) return VX_ERR_OOM;

    vx_strpool_t   *pool = (vx_strpool_t *)vx_arena_calloc(scratch, 1u, sizeof(*pool));
    const uint8_t **ptrs = (const uint8_t **)vx_arena_calloc(scratch, 1u, sizeof(*ptrs));
    int64_t        *lens = (int64_t *)vx_arena_calloc(scratch, 1u, sizeof(*lens));
    uint8_t        *buf  = (uint8_t *)vx_arena_alloc(scratch, total_ool_bytes);
    if (!pool || !ptrs || !lens || !buf) return VX_ERR_OOM;

    ptrs[0]      = buf;
    lens[0]      = (int64_t)total_ool_bytes;
    pool->ptrs   = ptrs;
    pool->lens   = lens;
    pool->owners = NULL;
    pool->nbufs  = 1u;
    pool->cap    = 1u;
    *out_pool    = pool;
    *out_buf     = buf;
    return VX_OK;
}

/* Portable memmem using memchr + memcmp (avoids GNU-only _GNU_SOURCE / Win32 gaps). */
static bool vx_bytes_contains(const uint8_t *hay, int32_t hlen,
                              const uint8_t *ndl, int32_t nlen) {
    if (nlen == 0) return true;
    if (nlen > hlen) return false;
    if (nlen == 1) return memchr(hay, ndl[0], (size_t)hlen) != NULL;

    const uint8_t *cur = hay;
    const uint8_t *end = hay + (hlen - nlen) + 1;
    while (cur < end) {
        const uint8_t *hit = (const uint8_t *)memchr(cur, ndl[0], (size_t)(end - cur));
        if (!hit) return false;
        if (memcmp(hit + 1, ndl + 1, (size_t)(nlen - 1)) == 0) return true;
        cur = hit + 1;
    }
    return false;
}

/* --- 1. len(str) -> i64 (UTF-8 codepoint count, matching Python len()) --- */
static vx_status_t vx_eval_len(const vx_vec_t *a, const vx_sel_t *sel,
                               vx_arena_t *scratch, vx_vec_t *out) {
    if (a->type != VX_TYPE_STR) return VX_ERR_SCHEMA;
    const uint32_t n = sel->count;

    int64_t  *dst = (int64_t *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
    uint64_t *val = NULL;
    if (!dst || vx_b_alloc_val(scratch, n, &val) != VX_OK) return VX_ERR_OOM;

    const vx_strview_t *svs     = (const vx_strview_t *)vx_b_base(a);
    uint32_t            valid_n = 0;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = vx_b_row(sel, i);
        if (!vx_b_valid(a, r)) { dst[i] = 0; continue; }

        const vx_strview_t *sv = &svs[vx_b_vidx(a, r)];
        const uint8_t      *p  = vx_strview_ptr(sv, a->strpool);
        int64_t             cp = 0;
        for (int32_t j = 0; j < sv->inl.len; ++j) {
            cp += ((p[j] & 0xC0u) != 0x80u); /* count UTF-8 lead bytes */
        }
        dst[i] = cp;
        vx_b_set_valid(val, i);
        valid_n++;
    }

    vx_b_finalize_vec(out, VX_TYPE_I64, n, dst, val, valid_n, NULL, scratch);
    return VX_OK;
}

/* --- 2 & 3. lower(str) / upper(str) -> str (ASCII case fold, UTF-8 safe) --- */
static vx_status_t vx_eval_ascii_case(const vx_vec_t *a, bool to_upper, const vx_sel_t *sel,
                                      vx_arena_t *scratch, vx_vec_t *out) {
    if (a->type != VX_TYPE_STR) return VX_ERR_SCHEMA;
    const uint32_t      n   = sel->count;
    const vx_strview_t *svs = (const vx_strview_t *)vx_b_base(a);

    /* Pass 1: measure total out-of-line payload bytes */
    size_t ool_bytes = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = vx_b_row(sel, i);
        if (!vx_b_valid(a, r)) continue;
        int32_t len = svs[vx_b_vidx(a, r)].inl.len;
        if (len > VX_STR_INLINE_MAX) ool_bytes += (size_t)len;
    }

    vx_strview_t *dst  = (vx_strview_t *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
    uint64_t     *val  = NULL;
    vx_strpool_t *pool = NULL;
    uint8_t      *buf  = NULL;
    if (!dst || vx_b_alloc_val(scratch, n, &val) != VX_OK ||
        vx_b_alloc_scratch_strpool(scratch, ool_bytes, &pool, &buf) != VX_OK) {
        return VX_ERR_OOM;
    }

    const uint8_t lo_ch   = to_upper ? 'a' : 'A';
    const uint8_t hi_ch   = to_upper ? 'z' : 'Z';
    const uint8_t xor_bit = 0x20u;
    int32_t       off     = 0;
    uint32_t      valid_n = 0;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = vx_b_row(sel, i);
        if (!vx_b_valid(a, r)) {
            memset(&dst[i], 0, sizeof(dst[i]));
            continue;
        }
        const vx_strview_t *sv  = &svs[vx_b_vidx(a, r)];
        const uint8_t      *src = vx_strview_ptr(sv, a->strpool);
        const int32_t       len = sv->inl.len;

        if (len <= VX_STR_INLINE_MAX) {
            memset(&dst[i], 0, sizeof(dst[i]));
            dst[i].inl.len = len;
            for (int32_t j = 0; j < len; ++j) {
                uint8_t c = src[j];
                dst[i].inl.data[j] = (c >= lo_ch && c <= hi_ch) ? (uint8_t)(c ^ xor_bit) : c;
            }
        } else {
            uint8_t *w = buf + off;
            for (int32_t j = 0; j < len; ++j) {
                uint8_t c = src[j];
                w[j] = (c >= lo_ch && c <= hi_ch) ? (uint8_t)(c ^ xor_bit) : c;
            }
            dst[i].out.len     = len;
            memcpy(dst[i].out.prefix, w, 4u);
            dst[i].out.buf_idx = 0;
            dst[i].out.offset  = off;
            off               += len;
        }
        vx_b_set_valid(val, i);
        valid_n++;
    }

    vx_b_finalize_vec(out, VX_TYPE_STR, n, dst, val, valid_n, pool, scratch);
    return VX_OK;
}

/* --- 4 & 5. startswith(str, str) / contains(str, str) -> bool --- */
static vx_status_t vx_eval_str_match(const vx_vec_t *hay, const vx_vec_t *ndl, bool is_prefix,
                                     const vx_sel_t *sel, vx_arena_t *scratch, vx_vec_t *out) {
    if (hay->type != VX_TYPE_STR || ndl->type != VX_TYPE_STR) return VX_ERR_SCHEMA;
    const uint32_t n = sel->count;

    uint8_t  *dst = (uint8_t *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
    uint64_t *val = NULL;
    if (!dst || vx_b_alloc_val(scratch, n, &val) != VX_OK) return VX_ERR_OOM;

    const vx_strview_t *h_svs   = (const vx_strview_t *)vx_b_base(hay);
    const vx_strview_t *n_svs   = (const vx_strview_t *)vx_b_base(ndl);
    uint32_t            valid_n = 0;

    /* Fast path: constant NULL needle makes every row NULL */
    if (ndl->kind == VX_VEC_CONST && !vx_b_valid(ndl, 0u)) {
        memset(dst, 0, n);
        vx_b_finalize_vec(out, VX_TYPE_BOOL, n, dst, val, 0u, NULL, scratch);
        return VX_OK;
    }

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = vx_b_row(sel, i);
        if (!vx_b_valid(hay, r) || !vx_b_valid(ndl, r)) {
            dst[i] = 0;
            continue;
        }
        const vx_strview_t *hsv = &h_svs[vx_b_vidx(hay, r)];
        const vx_strview_t *nsv = &n_svs[vx_b_vidx(ndl, r)];
        const int32_t       hl  = hsv->inl.len;
        const int32_t       nl  = nsv->inl.len;

        bool hit;
        if (nl == 0) {
            hit = true;
        } else if (nl > hl) {
            hit = false;
        } else if (is_prefix) {
            const uint8_t *np = vx_strview_ptr(nsv, ndl->strpool);
            /* Fast 4-byte prefix reject on out-of-line haystacks before touching strpool */
            if (!vx_strview_is_inline(*hsv) && nl >= 4 &&
                memcmp(hsv->out.prefix, np, 4u) != 0) {
                hit = false;
            } else {
                const uint8_t *hp = vx_strview_ptr(hsv, hay->strpool);
                hit = (memcmp(hp, np, (size_t)nl) == 0);
            }
        } else {
            const uint8_t *hp = vx_strview_ptr(hsv, hay->strpool);
            const uint8_t *np = vx_strview_ptr(nsv, ndl->strpool);
            hit = vx_bytes_contains(hp, hl, np, nl);
        }

        dst[i] = hit ? 1u : 0u;
        vx_b_set_valid(val, i);
        valid_n++;
    }

    vx_b_finalize_vec(out, VX_TYPE_BOOL, n, dst, val, valid_n, NULL, scratch);
    return VX_OK;
}

/* --- 6. coalesce(a, b, ...) -> T (supports >= 2 same-typed arguments) --- */
static vx_status_t vx_eval_coalesce(const vx_vec_t *args, uint32_t nargs, const vx_sel_t *sel,
                                    vx_arena_t *scratch, vx_vec_t *out) {
    if (nargs < 2u) return VX_ERR_SCHEMA;
    const vx_type_t type = args[0].type;
    for (uint32_t a = 1; a < nargs; ++a) {
        if (args[a].type != type) return VX_ERR_SCHEMA;
    }

    const uint32_t n   = sel->count;
    uint64_t      *val = NULL;
    if (vx_b_alloc_val(scratch, n, &val) != VX_OK) return VX_ERR_OOM;

    if (type == VX_TYPE_STR) {
        /* Pass 1: select winning arg index per row and sum out-of-line bytes */
        size_t ool_bytes = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t r = vx_b_row(sel, i);
            for (uint32_t a = 0; a < nargs; ++a) {
                if (vx_b_valid(&args[a], r)) {
                    const vx_strview_t *svs = (const vx_strview_t *)vx_b_base(&args[a]);
                    int32_t len = svs[vx_b_vidx(&args[a], r)].inl.len;
                    if (len > VX_STR_INLINE_MAX) ool_bytes += (size_t)len;
                    break;
                }
            }
        }

        vx_strview_t *dst  = (vx_strview_t *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
        vx_strpool_t *pool = NULL;
        uint8_t      *buf  = NULL;
        if (!dst || vx_b_alloc_val(scratch, n, &val) != VX_OK ||
            vx_b_alloc_scratch_strpool(scratch, ool_bytes, &pool, &buf) != VX_OK) {
            return VX_ERR_OOM;
        }

        int32_t  off     = 0;
        uint32_t valid_n = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t r     = vx_b_row(sel, i);
            bool     found = false;
            for (uint32_t a = 0; a < nargs; ++a) {
                if (!vx_b_valid(&args[a], r)) continue;
                const vx_strview_t *svs = (const vx_strview_t *)vx_b_base(&args[a]);
                vx_strview_t        sv  = svs[vx_b_vidx(&args[a], r)];
                if (!vx_strview_is_inline(sv)) {
                    const uint8_t *p = vx_strview_ptr(&sv, args[a].strpool);
                    memcpy(buf + off, p, (size_t)sv.inl.len);
                    sv.out.buf_idx = 0;
                    sv.out.offset  = off;
                    off           += sv.inl.len;
                }
                dst[i] = sv;
                vx_b_set_valid(val, i);
                valid_n++;
                found = true;
                break;
            }
            if (!found) memset(&dst[i], 0, sizeof(dst[i]));
        }
        vx_b_finalize_vec(out, VX_TYPE_STR, n, dst, val, valid_n, pool, scratch);
        return VX_OK;
    }

    const size_t elem_sz = (type == VX_TYPE_BOOL) ? sizeof(uint8_t) : sizeof(uint64_t);
    void        *dst     = vx_arena_alloc(scratch, (size_t)n * elem_sz);
    if (!dst) return VX_ERR_OOM;

    uint32_t valid_n = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r     = vx_b_row(sel, i);
        bool     found = false;
        for (uint32_t a = 0; a < nargs; ++a) {
            if (!vx_b_valid(&args[a], r)) continue;
            uint32_t    vi   = vx_b_vidx(&args[a], r);
            const void *base = vx_b_base(&args[a]);
            if (elem_sz == 1u) ((uint8_t *)dst)[i]  = ((const uint8_t *)base)[vi];
            else               ((uint64_t *)dst)[i] = ((const uint64_t *)base)[vi];
            vx_b_set_valid(val, i);
            valid_n++;
            found = true;
            break;
        }
        if (!found) {
            if (elem_sz == 1u) ((uint8_t *)dst)[i]  = 0u;
            else               ((uint64_t *)dst)[i] = 0ULL;
        }
    }

    vx_b_finalize_vec(out, type, n, dst, val, valid_n, NULL, scratch);
    return VX_OK;
}

/* --- 7. abs(i64 | f64) -> i64 | f64 --- */
static vx_status_t vx_eval_abs(const vx_vec_t *a, const vx_sel_t *sel,
                               vx_arena_t *scratch, vx_vec_t *out) {
    if (a->type != VX_TYPE_I64 && a->type != VX_TYPE_F64) return VX_ERR_SCHEMA;
    const uint32_t n   = sel->count;
    uint64_t      *val = NULL;
    if (vx_b_alloc_val(scratch, n, &val) != VX_OK) return VX_ERR_OOM;

    uint32_t valid_n = 0;
    if (a->type == VX_TYPE_I64) {
        int64_t       *dst = (int64_t *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
        if (!dst) return VX_ERR_OOM;
        const int64_t *src = (const int64_t *)vx_b_base(a);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t r = vx_b_row(sel, i);
            if (!vx_b_valid(a, r)) { dst[i] = 0; continue; }
            int64_t x = src[vx_b_vidx(a, r)];
            /* Unsigned negation avoids signed-overflow UB on INT64_MIN */
            dst[i] = (x < 0) ? (int64_t)(0ULL - (uint64_t)x) : x;
            vx_b_set_valid(val, i);
            valid_n++;
        }
        vx_b_finalize_vec(out, VX_TYPE_I64, n, dst, val, valid_n, NULL, scratch);
        return VX_OK;
    }

    double       *dst = (double *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
    if (!dst) return VX_ERR_OOM;
    const double *src = (const double *)vx_b_base(a);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = vx_b_row(sel, i);
        if (!vx_b_valid(a, r)) { dst[i] = 0.0; continue; }
        dst[i] = fabs(src[vx_b_vidx(a, r)]);
        vx_b_set_valid(val, i);
        valid_n++;
    }
    vx_b_finalize_vec(out, VX_TYPE_F64, n, dst, val, valid_n, NULL, scratch);
    return VX_OK;
}

/* --- 8. round(f64[, i64 digits]) -> f64 --- */
static inline double vx_round_digits(double x, int64_t d) {
    if (!isfinite(x) || d > 300) return x;
    if (d == 0)  return round(x);
    if (d < -300) return copysign(0.0, x);
    if (d > 0) {
        double scale = pow(10.0, (double)d);
        double y     = x * scale;
        return isfinite(y) ? (round(y) / scale) : x;
    }
    double scale = pow(10.0, (double)(-d));
    return round(x / scale) * scale;
}

static vx_status_t vx_eval_round(const vx_vec_t *args, uint32_t nargs, const vx_sel_t *sel,
                                 vx_arena_t *scratch, vx_vec_t *out) {
    if (nargs < 1u || nargs > 2u || args[0].type != VX_TYPE_F64) return VX_ERR_SCHEMA;
    if (nargs == 2u && args[1].type != VX_TYPE_I64) return VX_ERR_SCHEMA;

    const uint32_t n   = sel->count;
    double        *dst = (double *)vx_arena_alloc(scratch, (size_t)n * sizeof(*dst));
    uint64_t      *val = NULL;
    if (!dst || vx_b_alloc_val(scratch, n, &val) != VX_OK) return VX_ERR_OOM;

    const double  *src     = (const double *)vx_b_base(&args[0]);
    const int64_t *digs    = (nargs == 2u) ? (const int64_t *)vx_b_base(&args[1]) : NULL;
    uint32_t       valid_n = 0;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = vx_b_row(sel, i);
        if (!vx_b_valid(&args[0], r) || (nargs == 2u && !vx_b_valid(&args[1], r))) {
            dst[i] = 0.0;
            continue;
        }
        double  x = src[vx_b_vidx(&args[0], r)];
        int64_t d = (nargs == 2u) ? digs[vx_b_vidx(&args[1], r)] : 0;
        dst[i] = vx_round_digits(x, d);
        vx_b_set_valid(val, i);
        valid_n++;
    }

    vx_b_finalize_vec(out, VX_TYPE_F64, n, dst, val, valid_n, NULL, scratch);
    return VX_OK;
}

/* --- Top-Level Dispatcher --- */
vx_status_t vx_builtin_eval(vx_builtin_id_t fn, const vx_vec_t *args, uint32_t nargs,
                            const vx_sel_t *sel, vx_arena_t *scratch, vx_vec_t *out) {
    if (!args || !sel || !scratch || !out || nargs == 0) return VX_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    if (sel->count == 0) {
        out->kind = VX_VEC_FLAT;
        switch (fn) {
        case VX_FN_LEN:                                   out->type = VX_TYPE_I64;  break;
        case VX_FN_LOWER: case VX_FN_UPPER:               out->type = VX_TYPE_STR;  break;
        case VX_FN_STARTSWITH: case VX_FN_CONTAINS:       out->type = VX_TYPE_BOOL; break;
        case VX_FN_COALESCE: case VX_FN_ABS:              out->type = args[0].type; break;
        case VX_FN_ROUND:                                 out->type = VX_TYPE_F64;  break;
        }
        return VX_OK;
    }

    switch (fn) {
    case VX_FN_LEN:        return (nargs == 1u) ? vx_eval_len(&args[0], sel, scratch, out) : VX_ERR_SCHEMA;
    case VX_FN_LOWER:      return (nargs == 1u) ? vx_eval_ascii_case(&args[0], false, sel, scratch, out) : VX_ERR_SCHEMA;
    case VX_FN_UPPER:      return (nargs == 1u) ? vx_eval_ascii_case(&args[0], true,  sel, scratch, out) : VX_ERR_SCHEMA;
    case VX_FN_STARTSWITH: return (nargs == 2u) ? vx_eval_str_match(&args[0], &args[1], true,  sel, scratch, out) : VX_ERR_SCHEMA;
    case VX_FN_CONTAINS:   return (nargs == 2u) ? vx_eval_str_match(&args[0], &args[1], false, sel, scratch, out) : VX_ERR_SCHEMA;
    case VX_FN_COALESCE:   return vx_eval_coalesce(args, nargs, sel, scratch, out);
    case VX_FN_ABS:        return (nargs == 1u) ? vx_eval_abs(&args[0], sel, scratch, out) : VX_ERR_SCHEMA;
    case VX_FN_ROUND:      return vx_eval_round(args, nargs, sel, scratch, out);
    }
    return VX_ERR_INVALID;
}

