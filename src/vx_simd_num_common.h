#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdalign.h>
#include "vx_simd.h"

static inline uint32_t vx_popcnt32(uint32_t x) {
#if defined(_MSC_VER)
    return (uint32_t)__popcnt(x);
#else
    return (uint32_t)__builtin_popcount(x);
#endif
}

static inline bool vx_num_valid_bit(const uint64_t *validity, uint32_t row) {
    return !validity || ((validity[row >> 6] >> (row & 63u)) & 1ULL) != 0;
}

static inline bool vx_cmp_i64_scalar(int64_t a, int64_t b, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return a == b;
    case VX_CMP_NE: return a != b;
    case VX_CMP_LT: return a <  b;
    case VX_CMP_LE: return a <= b;
    case VX_CMP_GT: return a >  b;
    case VX_CMP_GE: return a >= b;
    }
    return false;
}

static inline bool vx_cmp_f64_scalar(double a, double b, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return a == b;
    case VX_CMP_NE: return a != b; /* IEEE-754: true if either operand is NaN */
    case VX_CMP_LT: return a <  b;
    case VX_CMP_LE: return a <= b;
    case VX_CMP_GT: return a >  b;
    case VX_CMP_GE: return a >= b;
    }
    return false;
}

/* Branchless 4-row selection emitter.
 * Invariant: out_n <= i <= VX_BATCH_CAP - 4, so all 4 writes stay within idx[0..4095]
 * and never overwrite unread in_sel->idx[>= i+4] when in_sel == out_sel. */
static inline uint32_t vx_emit_mask4_dense(uint32_t *dst, uint32_t out_n,
                                           uint32_t base_i, uint32_t hit) {
    if (!hit) return out_n;
    dst[out_n] = base_i + 0u; out_n += (hit >> 0) & 1u;
    dst[out_n] = base_i + 1u; out_n += (hit >> 1) & 1u;
    dst[out_n] = base_i + 2u; out_n += (hit >> 2) & 1u;
    dst[out_n] = base_i + 3u; out_n += (hit >> 3) & 1u;
    return out_n;
}

static inline uint32_t vx_emit_mask4_sparse(uint32_t *dst, uint32_t out_n,
                                            const uint32_t r[4], uint32_t hit) {
    if (!hit) return out_n;
    dst[out_n] = r[0]; out_n += (hit >> 0) & 1u;
    dst[out_n] = r[1]; out_n += (hit >> 1) & 1u;
    dst[out_n] = r[2]; out_n += (hit >> 2) & 1u;
    dst[out_n] = r[3]; out_n += (hit >> 3) & 1u;
    return out_n;
}

static inline uint32_t vx_filter_i64_tail(const int64_t *col, const uint64_t *validity,
                                          const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                          vx_cmp_op_t op, int64_t rhs,
                                          uint32_t i, uint32_t n, uint32_t out_n) {
    const bool id = in_sel->is_identity;
    for (; i < n; ++i) {
        uint32_t r = id ? i : in_sel->idx[i];
        if (vx_num_valid_bit(validity, r) && vx_cmp_i64_scalar(col[r], rhs, op)) {
            out_sel->idx[out_n++] = r;
        }
    }
    return out_n;
}

static inline uint32_t vx_filter_f64_tail(const double *col, const uint64_t *validity,
                                          const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                          vx_cmp_op_t op, double rhs,
                                          uint32_t i, uint32_t n, uint32_t out_n) {
    const bool id = in_sel->is_identity;
    for (; i < n; ++i) {
        uint32_t r = id ? i : in_sel->idx[i];
        if (vx_num_valid_bit(validity, r) && vx_cmp_f64_scalar(col[r], rhs, op)) {
            out_sel->idx[out_n++] = r;
        }
    }
    return out_n;
}

static inline double vx_sum_f64_tail(const double *col, const uint64_t *validity,
                                     const vx_sel_t *sel, uint32_t i, uint32_t n,
                                     double sum, uint64_t *valid_n) {
    const bool id = sel->is_identity;
    for (; i < n; ++i) {
        uint32_t r = id ? i : sel->idx[i];
        if (vx_num_valid_bit(validity, r)) {
            sum += col[r];
            ++(*valid_n);
        }
    }
    return sum;
}

/* 4-lane scalar reference for vx_simd.c: bit-exact parity with AVX2 and NEON sum_f64. */
static inline double vx_simd_scalar_sum_f64_ref(const double *col, const uint64_t *validity,
                                                const vx_sel_t *sel, uint64_t *out_valid_n) {
    const uint32_t n  = sel->count;
    const bool     id = sel->is_identity;
    double   acc[4]   = { 0.0, 0.0, 0.0, 0.0 };
    uint64_t valid_n  = 0;
    uint32_t i        = 0;

    for (; i + 4u <= n; i += 4u) {
        for (uint32_t k = 0; k < 4u; ++k) {
            uint32_t r = id ? (i + k) : sel->idx[i + k];
            if (vx_num_valid_bit(validity, r)) {
                acc[k] += col[r];
                ++valid_n;
            } else {
                acc[k] += 0.0; /* preserves exact +0.0 / -0.0 parity with masked SIMD add */
            }
        }
    }
    double sum = (acc[0] + acc[1]) + (acc[2] + acc[3]);
    sum = vx_sum_f64_tail(col, validity, sel, i, n, sum, &valid_n);
    if (out_valid_n) *out_valid_n = valid_n;
    return sum;
}

static inline uint32_t vx_simd_scalar_filter_i64_ref(const int64_t *col, const uint64_t *validity,
                                                     const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                                     vx_cmp_op_t op, int64_t rhs) {
    const uint32_t n = in_sel->count;
    const bool in_id = in_sel->is_identity;
    uint32_t out_n = vx_filter_i64_tail(col, validity, in_sel, out_sel, op, rhs, 0, n, 0);
    out_sel->count = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

static inline uint32_t vx_simd_scalar_filter_f64_ref(const double *col, const uint64_t *validity,
                                                     const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                                     vx_cmp_op_t op, double rhs) {
    const uint32_t n = in_sel->count;
    const bool in_id = in_sel->is_identity;
    uint32_t out_n = vx_filter_f64_tail(col, validity, in_sel, out_sel, op, rhs, 0, n, 0);
    out_sel->count = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

#include <limits.h>
#include <math.h>
#include <string.h>

static inline int64_t vx_u64_to_i64_bits(uint64_t u) {
    int64_t s;
    memcpy(&s, &u, sizeof(s));
    return s;
}

static inline bool vx_cmp_u64_scalar(uint64_t a, uint64_t b, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return a == b;
    case VX_CMP_NE: return a != b;
    case VX_CMP_LT: return a <  b;
    case VX_CMP_LE: return a <= b;
    case VX_CMP_GT: return a >  b;
    case VX_CMP_GE: return a >= b;
    }
    return false;
}

static inline uint32_t vx_filter_u64_tail(const uint64_t *col, const uint64_t *validity,
                                          const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                          vx_cmp_op_t op, uint64_t rhs,
                                          uint32_t i, uint32_t n, uint32_t out_n) {
    const bool id = in_sel->is_identity;
    for (; i < n; ++i) {
        uint32_t r = id ? i : in_sel->idx[i];
        if (vx_num_valid_bit(validity, r) && vx_cmp_u64_scalar(col[r], rhs, op)) {
            out_sel->idx[out_n++] = r;
        }
    }
    return out_n;
}

/* Exact IEEE-754-2019 minimum/maximum: NaN propagates; -0.0 < +0.0. */
static inline double vx_f64_min_ieee(double a, double b) {
    if (isnan(a) || isnan(b)) return (double)NAN;
    if (a < b) return a;
    if (b < a) return b;
    /* Equal values only differ in sign bit when both are +/-0.0. */
    return signbit(a) ? a : b;
}

static inline double vx_f64_max_ieee(double a, double b) {
    if (isnan(a) || isnan(b)) return (double)NAN;
    if (a > b) return a;
    if (b > a) return b;
    return signbit(a) ? b : a;
}

static inline vx_agg_minmax_f64_t vx_minmax_f64_tail(const double *col, const uint64_t *validity,
                                                     const vx_sel_t *sel, uint32_t i, uint32_t n,
                                                     vx_agg_minmax_f64_t acc) {
    const bool id = sel->is_identity;
    for (; i < n; ++i) {
        uint32_t r = id ? i : sel->idx[i];
        if (!vx_num_valid_bit(validity, r)) continue;
        double v   = col[r];
        acc.min_v  = vx_f64_min_ieee(acc.min_v, v);
        acc.max_v  = vx_f64_max_ieee(acc.max_v, v);
        acc.valid_n++;
    }
    if (acc.valid_n == 0) {
        acc.min_v = 0.0;
        acc.max_v = 0.0;
    }
    return acc;
}

/* Scalar reference for vx_simd.c and cross-ISA parity tests. */
static inline vx_agg_minmax_f64_t vx_simd_scalar_minmax_f64_ref(const double *col,
                                                                const uint64_t *validity,
                                                                const vx_sel_t *sel) {
    vx_agg_minmax_f64_t acc = { .min_v = (double)INFINITY, .max_v = -(double)INFINITY, .valid_n = 0 };
    return vx_minmax_f64_tail(col, validity, sel, 0u, sel->count, acc);
}

static inline uint32_t vx_simd_scalar_filter_u64_ref(const uint64_t *col, const uint64_t *validity,
                                                     const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                                     vx_cmp_op_t op, uint64_t rhs) {
    const uint32_t n = in_sel->count;
    const bool in_id = in_sel->is_identity;
    uint32_t out_n = vx_filter_u64_tail(col, validity, in_sel, out_sel, op, rhs, 0, n, 0);
    out_sel->count = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

#if defined(__x86_64__) || defined(_M_X64)
uint32_t vx_simd_avx2_filter_i64(const int64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, int64_t rhs);
uint32_t vx_simd_avx2_filter_u64(const uint64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, uint64_t rhs);
uint32_t vx_simd_avx2_filter_f64(const double *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, double rhs);
double   vx_simd_avx2_sum_f64(const double *col, const uint64_t *validity,
                              const vx_sel_t *sel, uint64_t *out_valid_n);
vx_agg_minmax_f64_t vx_simd_avx2_minmax_f64(const double *col, const uint64_t *validity,
                                            const vx_sel_t *sel);
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
uint32_t vx_simd_neon_filter_i64(const int64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, int64_t rhs);
uint32_t vx_simd_neon_filter_u64(const uint64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, uint64_t rhs);
uint32_t vx_simd_neon_filter_f64(const double *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, double rhs);
double   vx_simd_neon_sum_f64(const double *col, const uint64_t *validity,
                              const vx_sel_t *sel, uint64_t *out_valid_n);
vx_agg_minmax_f64_t vx_simd_neon_minmax_f64(const double *col, const uint64_t *validity,
                                            const vx_sel_t *sel);
#endif
