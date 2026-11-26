#pragma once
#include <string.h>
#include "vx_simd.h"

static inline uint32_t vx_ctz32(uint32_t x) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, x);
    return (uint32_t)idx;
#else
    return (uint32_t)__builtin_ctz(x);
#endif
}

static inline bool vx_valid_bit(const uint64_t *validity, uint32_t row) {
    return !validity || ((validity[row >> 6] >> (row & 63u)) & 1ULL) != 0;
}

static inline vx_strview_t vx_make_str_target(const uint8_t *rhs, int32_t rhs_len) {
    vx_strview_t t;
    memset(&t, 0, sizeof(t));
    t.inl.len = rhs_len;
    if (rhs_len <= VX_STR_INLINE_MAX) {
        if (rhs_len > 0) memcpy(t.inl.data, rhs, (size_t)rhs_len);
    } else {
        memcpy(t.out.prefix, rhs, 4u);
    }
    return t;
}

/* Called ONLY after raw.lo == target.raw.lo on a valid out-of-line row (len == rhs_len > 12). */
static inline bool vx_str_tail_eq(const vx_strview_t *sv, const vx_strpool_t *pool,
                                  const uint8_t *rhs, int32_t rhs_len) {
    const uint8_t *p = pool->ptrs[sv->out.buf_idx] + sv->out.offset;
    return memcmp(p + 4, rhs + 4, (size_t)(rhs_len - 4)) == 0;
}

/* Scalar fallback for 0..3 tail rows (safe when in_sel == out_sel). */
static inline uint32_t vx_str_eq_tail(const vx_strview_t *col, const vx_strpool_t *pool,
                                      const uint64_t *validity, const vx_sel_t *in_sel,
                                      vx_sel_t *out_sel, vx_strview_t target,
                                      const uint8_t *rhs, int32_t rhs_len, bool negate,
                                      uint32_t start_i, uint32_t n, uint32_t out_n) {
    const bool is_inline = (rhs_len <= VX_STR_INLINE_MAX);
    const bool identity  = in_sel->is_identity;

    for (uint32_t i = start_i; i < n; ++i) {
        uint32_t r = identity ? i : in_sel->idx[i];
        if (!vx_valid_bit(validity, r)) continue;

        bool eq;
        if (is_inline) {
            eq = (col[r].raw.lo == target.raw.lo) & (col[r].raw.hi == target.raw.hi);
        } else {
            eq = (col[r].raw.lo == target.raw.lo) &&
                 vx_str_tail_eq(&col[r], pool, rhs, rhs_len);
        }
        if (eq != negate) {
            out_sel->idx[out_n++] = r;
        }
    }
    return out_n;
}

