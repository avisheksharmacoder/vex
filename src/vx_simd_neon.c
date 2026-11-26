#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#include <stdalign.h>
#include "vx_simd_str_common.h"
#include "vx_simd_num_common.h"

/* Packs two 64-bit boolean mask vectors {m0, m1} and {m2, m3} into a 4-bit bitmask. */
static inline uint32_t vx_neon_mask4_u64(uint64x2_t m01, uint64x2_t m23) {
    static const uint32_t k_w[4] = { 1u, 2u, 4u, 8u };
    uint32x4_t m32 = vuzp1q_u32(vreinterpretq_u32_u64(m01), vreinterpretq_u32_u64(m23));
    return vaddvq_u32(vandq_u32(m32, vld1q_u32(k_w)));
}

uint32_t vx_simd_neon_filter_str_eq(const vx_strview_t *col, const vx_strpool_t *pool,
                                    const uint64_t *validity, const vx_sel_t *in_sel,
                                    vx_sel_t *out_sel, const uint8_t *rhs, int32_t rhs_len,
                                    bool negate) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const vx_strview_t target = vx_make_str_target(rhs, rhs_len);
    const bool         is_inl = (rhs_len <= VX_STR_INLINE_MAX);
    const bool         in_id  = in_sel->is_identity;
    const uint64x2_t   t_lo   = vdupq_n_u64(target.raw.lo);
    const uint64x2_t   t_hi   = vdupq_n_u64(target.raw.hi);
    const uint64x2_t   t_128  = vld1q_u64(&target.raw.lo);

    uint32_t out_n = 0;
    uint32_t i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (valid4 == 0) continue;

            /* vld2q_u64 deinterleaves: .val[0] = {lo0, lo1}, .val[1] = {hi0, hi1} */
            uint64x2x2_t p01 = vld2q_u64(&col[i].raw.lo);
            uint64x2x2_t p23 = vld2q_u64(&col[i + 2u].raw.lo);

            uint64x2_t lo_eq01 = vceqq_u64(p01.val[0], t_lo);
            uint64x2_t lo_eq23 = vceqq_u64(p23.val[0], t_lo);

            uint32_t eq_mask;
            if (is_inl) {
                uint64x2_t m01 = vandq_u64(lo_eq01, vceqq_u64(p01.val[1], t_hi));
                uint64x2_t m23 = vandq_u64(lo_eq23, vceqq_u64(p23.val[1], t_hi));
                eq_mask = vx_neon_mask4_u64(m01, m23);
            } else {
                uint32_t pref = vx_neon_mask4_u64(lo_eq01, lo_eq23);
                uint32_t cand = pref & valid4;
                eq_mask = 0;
                while (cand) {
                    uint32_t k = vx_ctz32(cand);
                    cand &= cand - 1u;
                    if (vx_str_tail_eq(&col[i + k], pool, rhs, rhs_len)) {
                        eq_mask |= (1u << k);
                    }
                }
            }

            uint32_t hit = (negate ? ~eq_mask : eq_mask) & valid4;
            while (hit) {
                uint32_t k = vx_ctz32(hit);
                hit &= hit - 1u;
                out_sel->idx[out_n++] = i + k;
            }
        }
    } else {
        for (; i + 4u <= n; i += 4u) {
            uint32_t r[4] = {
                in_sel->idx[i + 0u], in_sel->idx[i + 1u],
                in_sel->idx[i + 2u], in_sel->idx[i + 3u]
            };
            uint32_t valid4 = 0xFu;
            if (validity) {
                valid4 = ((uint32_t)vx_valid_bit(validity, r[0]) << 0) |
                         ((uint32_t)vx_valid_bit(validity, r[1]) << 1) |
                         ((uint32_t)vx_valid_bit(validity, r[2]) << 2) |
                         ((uint32_t)vx_valid_bit(validity, r[3]) << 3);
                if (valid4 == 0) continue;
            }

            uint32_t eq_mask;
            if (is_inl) {
                uint64x2_t e0  = vceqq_u64(vld1q_u64(&col[r[0]].raw.lo), t_128);
                uint64x2_t e1  = vceqq_u64(vld1q_u64(&col[r[1]].raw.lo), t_128);
                uint64x2_t e2  = vceqq_u64(vld1q_u64(&col[r[2]].raw.lo), t_128);
                uint64x2_t e3  = vceqq_u64(vld1q_u64(&col[r[3]].raw.lo), t_128);
                uint64x2_t m01 = vandq_u64(vuzp1q_u64(e0, e1), vuzp2q_u64(e0, e1));
                uint64x2_t m23 = vandq_u64(vuzp1q_u64(e2, e3), vuzp2q_u64(e2, e3));
                eq_mask = vx_neon_mask4_u64(m01, m23);
            } else {
                uint64_t lo_arr[4] = {
                    col[r[0]].raw.lo, col[r[1]].raw.lo,
                    col[r[2]].raw.lo, col[r[3]].raw.lo
                };
                uint64x2_t c01 = vceqq_u64(vld1q_u64(&lo_arr[0]), t_lo);
                uint64x2_t c23 = vceqq_u64(vld1q_u64(&lo_arr[2]), t_lo);
                uint32_t pref  = vx_neon_mask4_u64(c01, c23);
                uint32_t cand  = pref & valid4;
                eq_mask = 0;
                while (cand) {
                    uint32_t k = vx_ctz32(cand);
                    cand &= cand - 1u;
                    if (vx_str_tail_eq(&col[r[k]], pool, rhs, rhs_len)) {
                        eq_mask |= (1u << k);
                    }
                }
            }

            uint32_t hit = (negate ? ~eq_mask : eq_mask) & valid4;
            while (hit) {
                uint32_t k = vx_ctz32(hit);
                hit &= hit - 1u;
                out_sel->idx[out_n++] = r[k];
            }
        }
    }

    out_n = vx_str_eq_tail(col, pool, validity, in_sel, out_sel, target,
                           rhs, rhs_len, negate, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

/* 64B (1 cache line) LUT mapping 2-bit validity mask (0..3) -> 2x64-bit lane masks */
static const alignas(16) uint64_t k_neon_mask2[4][2] = {
    {  0ULL,  0ULL }, { ~0ULL,  0ULL },
    {  0ULL, ~0ULL }, { ~0ULL, ~0ULL }
};

static inline uint64x2_t vx_neon_not_u64(uint64x2_t x) {
    return vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(x)));
}

static inline uint32_t vx_neon_num_mask4(uint64x2_t m01, uint64x2_t m23) {
    static const uint32_t k_w[4] = { 1u, 2u, 4u, 8u };
    uint32x4_t m32 = vuzp1q_u32(vreinterpretq_u32_u64(m01), vreinterpretq_u32_u64(m23));
    return vaddvq_u32(vandq_u32(m32, vld1q_u32(k_w)));
}

/* --- filter_i64 --- */
static inline uint64x2_t vx_neon_cmp_i64(int64x2_t v, int64x2_t rhs, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return vceqq_s64(v, rhs);
    case VX_CMP_NE: return vx_neon_not_u64(vceqq_s64(v, rhs));
    case VX_CMP_LT: return vcltq_s64(v, rhs);
    case VX_CMP_LE: return vcleq_s64(v, rhs);
    case VX_CMP_GT: return vcgtq_s64(v, rhs);
    case VX_CMP_GE: return vcgeq_s64(v, rhs);
    }
    return vdupq_n_u64(0);
}

static inline uint32_t vx_neon_filter_i64_impl(const int64_t *col, const uint64_t *validity,
                                               const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                               vx_cmp_op_t op, int64_t rhs) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const bool      in_id = in_sel->is_identity;
    const int64x2_t vrhs  = vdupq_n_s64(rhs);
    uint32_t        out_n = 0;
    uint32_t        i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (!valid4) continue;

            uint64x2_t m01 = vx_neon_cmp_i64(vld1q_s64(&col[i + 0u]), vrhs, op);
            uint64x2_t m23 = vx_neon_cmp_i64(vld1q_s64(&col[i + 2u]), vrhs, op);
            uint32_t   hit = vx_neon_num_mask4(m01, m23) & valid4;
            out_n = vx_emit_mask4_dense(out_sel->idx, out_n, i, hit);
        }
    } else {
        for (; i + 4u <= n; i += 4u) {
            uint32_t r[4] = {
                in_sel->idx[i + 0u], in_sel->idx[i + 1u],
                in_sel->idx[i + 2u], in_sel->idx[i + 3u]
            };
            uint32_t valid4 = 0xFu;
            if (validity) {
                valid4 = ((uint32_t)vx_num_valid_bit(validity, r[0]) << 0) |
                         ((uint32_t)vx_num_valid_bit(validity, r[1]) << 1) |
                         ((uint32_t)vx_num_valid_bit(validity, r[2]) << 2) |
                         ((uint32_t)vx_num_valid_bit(validity, r[3]) << 3);
                if (!valid4) continue;
            }

            int64_t    tmp[4] = { col[r[0]], col[r[1]], col[r[2]], col[r[3]] };
            uint64x2_t m01    = vx_neon_cmp_i64(vld1q_s64(&tmp[0]), vrhs, op);
            uint64x2_t m23    = vx_neon_cmp_i64(vld1q_s64(&tmp[2]), vrhs, op);
            uint32_t   hit    = vx_neon_num_mask4(m01, m23) & valid4;
            out_n = vx_emit_mask4_sparse(out_sel->idx, out_n, r, hit);
        }
    }

    out_n = vx_filter_i64_tail(col, validity, in_sel, out_sel, op, rhs, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

uint32_t vx_simd_neon_filter_i64(const int64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, int64_t rhs) {
    switch (op) {
    case VX_CMP_EQ: return vx_neon_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_EQ, rhs);
    case VX_CMP_NE: return vx_neon_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_NE, rhs);
    case VX_CMP_LT: return vx_neon_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_LT, rhs);
    case VX_CMP_LE: return vx_neon_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_LE, rhs);
    case VX_CMP_GT: return vx_neon_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_GT, rhs);
    case VX_CMP_GE: return vx_neon_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_GE, rhs);
    }
    out_sel->count = 0; out_sel->is_identity = true; return 0;
}

/* --- filter_f64 (exact IEEE-754 NaN & -0.0 parity) --- */
static inline uint64x2_t vx_neon_cmp_f64(float64x2_t v, float64x2_t rhs, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return vceqq_f64(v, rhs);                  /* NaN -> false */
    case VX_CMP_NE: return vx_neon_not_u64(vceqq_f64(v, rhs)); /* NaN -> true  */
    case VX_CMP_LT: return vcltq_f64(v, rhs);                  /* NaN -> false */
    case VX_CMP_LE: return vcleq_f64(v, rhs);                  /* NaN -> false */
    case VX_CMP_GT: return vcgtq_f64(v, rhs);                  /* NaN -> false */
    case VX_CMP_GE: return vcgeq_f64(v, rhs);                  /* NaN -> false */
    }
    return vdupq_n_u64(0);
}

static inline uint32_t vx_neon_filter_f64_impl(const double *col, const uint64_t *validity,
                                               const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                               vx_cmp_op_t op, double rhs) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const bool        in_id = in_sel->is_identity;
    const float64x2_t vrhs  = vdupq_n_f64(rhs);
    uint32_t          out_n = 0;
    uint32_t          i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (!valid4) continue;

            uint64x2_t m01 = vx_neon_cmp_f64(vld1q_f64(&col[i + 0u]), vrhs, op);
            uint64x2_t m23 = vx_neon_cmp_f64(vld1q_f64(&col[i + 2u]), vrhs, op);
            uint32_t   hit = vx_neon_num_mask4(m01, m23) & valid4;
            out_n = vx_emit_mask4_dense(out_sel->idx, out_n, i, hit);
        }
    } else {
        for (; i + 4u <= n; i += 4u) {
            uint32_t r[4] = {
                in_sel->idx[i + 0u], in_sel->idx[i + 1u],
                in_sel->idx[i + 2u], in_sel->idx[i + 3u]
            };
            uint32_t valid4 = 0xFu;
            if (validity) {
                valid4 = ((uint32_t)vx_num_valid_bit(validity, r[0]) << 0) |
                         ((uint32_t)vx_num_valid_bit(validity, r[1]) << 1) |
                         ((uint32_t)vx_num_valid_bit(validity, r[2]) << 2) |
                         ((uint32_t)vx_num_valid_bit(validity, r[3]) << 3);
                if (!valid4) continue;
            }

            double     tmp[4] = { col[r[0]], col[r[1]], col[r[2]], col[r[3]] };
            uint64x2_t m01    = vx_neon_cmp_f64(vld1q_f64(&tmp[0]), vrhs, op);
            uint64x2_t m23    = vx_neon_cmp_f64(vld1q_f64(&tmp[2]), vrhs, op);
            uint32_t   hit    = vx_neon_num_mask4(m01, m23) & valid4;
            out_n = vx_emit_mask4_sparse(out_sel->idx, out_n, r, hit);
        }
    }

    out_n = vx_filter_f64_tail(col, validity, in_sel, out_sel, op, rhs, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

uint32_t vx_simd_neon_filter_f64(const double *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, double rhs) {
    switch (op) {
    case VX_CMP_EQ: return vx_neon_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_EQ, rhs);
    case VX_CMP_NE: return vx_neon_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_NE, rhs);
    case VX_CMP_LT: return vx_neon_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_LT, rhs);
    case VX_CMP_LE: return vx_neon_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_LE, rhs);
    case VX_CMP_GT: return vx_neon_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_GT, rhs);
    case VX_CMP_GE: return vx_neon_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_GE, rhs);
    }
    out_sel->count = 0; out_sel->is_identity = true; return 0;
}

/* --- sum_f64 (4-lane tree reduction matching AVX2 & scalar bit-for-bit) --- */
static inline float64x2_t vx_neon_mask_f64(float64x2_t v, uint32_t m2) {
    uint64x2_t mask = vld1q_u64(k_neon_mask2[m2]);
    return vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(v), mask));
}

double vx_simd_neon_sum_f64(const double *col, const uint64_t *validity,
                            const vx_sel_t *sel, uint64_t *out_valid_n) {
    const uint32_t n = sel->count;
    if (n == 0) {
        if (out_valid_n) *out_valid_n = 0;
        return 0.0;
    }

    const float64x2_t z0    = vdupq_n_f64(0.0);
    float64x2_t       acc01 = z0;
    float64x2_t       acc23 = z0;
    uint64_t          valid_n = 0;
    uint32_t          i       = 0;

    if (sel->is_identity) {
        if (!validity) {
            for (; i + 4u <= n; i += 4u) {
                acc01 = vaddq_f64(acc01, vld1q_f64(&col[i + 0u]));
                acc23 = vaddq_f64(acc23, vld1q_f64(&col[i + 2u]));
            }
            valid_n = i;
        } else {
            for (; i + 4u <= n; i += 4u) {
                uint32_t valid4 = (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu);
                if (!valid4) {
                    acc01 = vaddq_f64(acc01, z0);
                    acc23 = vaddq_f64(acc23, z0);
                    continue;
                }
                acc01 = vaddq_f64(acc01, vx_neon_mask_f64(vld1q_f64(&col[i + 0u]), valid4 & 3u));
                acc23 = vaddq_f64(acc23, vx_neon_mask_f64(vld1q_f64(&col[i + 2u]), valid4 >> 2));
                valid_n += vx_popcnt32(valid4);
            }
        }
    } else {
        if (!validity) {
            for (; i + 4u <= n; i += 4u) {
                double tmp[4] = {
                    col[sel->idx[i + 0u]], col[sel->idx[i + 1u]],
                    col[sel->idx[i + 2u]], col[sel->idx[i + 3u]]
                };
                acc01 = vaddq_f64(acc01, vld1q_f64(&tmp[0]));
                acc23 = vaddq_f64(acc23, vld1q_f64(&tmp[2]));
            }
            valid_n = i;
        } else {
            for (; i + 4u <= n; i += 4u) {
                uint32_t r0 = sel->idx[i + 0u], r1 = sel->idx[i + 1u];
                uint32_t r2 = sel->idx[i + 2u], r3 = sel->idx[i + 3u];
                uint32_t valid4 = ((uint32_t)vx_num_valid_bit(validity, r0) << 0) |
                                  ((uint32_t)vx_num_valid_bit(validity, r1) << 1) |
                                  ((uint32_t)vx_num_valid_bit(validity, r2) << 2) |
                                  ((uint32_t)vx_num_valid_bit(validity, r3) << 3);
                if (!valid4) {
                    acc01 = vaddq_f64(acc01, z0);
                    acc23 = vaddq_f64(acc23, z0);
                    continue;
                }
                double tmp[4] = { col[r0], col[r1], col[r2], col[r3] };
                acc01 = vaddq_f64(acc01, vx_neon_mask_f64(vld1q_f64(&tmp[0]), valid4 & 3u));
                acc23 = vaddq_f64(acc23, vx_neon_mask_f64(vld1q_f64(&tmp[2]), valid4 >> 2));
                valid_n += vx_popcnt32(valid4);
            }
        }
    }

    double s01 = vgetq_lane_f64(acc01, 0) + vgetq_lane_f64(acc01, 1);
    double s23 = vgetq_lane_f64(acc23, 0) + vgetq_lane_f64(acc23, 1);
    double sum = s01 + s23;
    sum = vx_sum_f64_tail(col, validity, sel, i, n, sum, &valid_n);
    if (out_valid_n) *out_valid_n = valid_n;
    return sum;
}

/* --- filter_u64 (native unsigned 64-bit NEON comparisons) --- */
static inline uint64x2_t vx_neon_cmp_u64(uint64x2_t v, uint64x2_t rhs, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return vceqq_u64(v, rhs);
    case VX_CMP_NE: return vx_neon_not_u64(vceqq_u64(v, rhs));
    case VX_CMP_LT: return vcltq_u64(v, rhs);
    case VX_CMP_LE: return vcleq_u64(v, rhs);
    case VX_CMP_GT: return vcgtq_u64(v, rhs);
    case VX_CMP_GE: return vcgeq_u64(v, rhs);
    }
    return vdupq_n_u64(0);
}

static inline uint32_t vx_neon_filter_u64_impl(const uint64_t *col, const uint64_t *validity,
                                               const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                               vx_cmp_op_t op, uint64_t rhs) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const bool       in_id = in_sel->is_identity;
    const uint64x2_t vrhs  = vdupq_n_u64(rhs);
    uint32_t         out_n = 0;
    uint32_t         i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (!valid4) continue;

            uint64x2_t m01 = vx_neon_cmp_u64(vld1q_u64(&col[i + 0u]), vrhs, op);
            uint64x2_t m23 = vx_neon_cmp_u64(vld1q_u64(&col[i + 2u]), vrhs, op);
            uint32_t   hit = vx_neon_num_mask4(m01, m23) & valid4;
            out_n = vx_emit_mask4_dense(out_sel->idx, out_n, i, hit);
        }
    } else {
        for (; i + 4u <= n; i += 4u) {
            uint32_t r[4] = {
                in_sel->idx[i + 0u], in_sel->idx[i + 1u],
                in_sel->idx[i + 2u], in_sel->idx[i + 3u]
            };
            uint32_t valid4 = 0xFu;
            if (validity) {
                valid4 = ((uint32_t)vx_num_valid_bit(validity, r[0]) << 0) |
                         ((uint32_t)vx_num_valid_bit(validity, r[1]) << 1) |
                         ((uint32_t)vx_num_valid_bit(validity, r[2]) << 2) |
                         ((uint32_t)vx_num_valid_bit(validity, r[3]) << 3);
                if (!valid4) continue;
            }

            uint64_t   tmp[4] = { col[r[0]], col[r[1]], col[r[2]], col[r[3]] };
            uint64x2_t m01    = vx_neon_cmp_u64(vld1q_u64(&tmp[0]), vrhs, op);
            uint64x2_t m23    = vx_neon_cmp_u64(vld1q_u64(&tmp[2]), vrhs, op);
            uint32_t   hit    = vx_neon_num_mask4(m01, m23) & valid4;
            out_n = vx_emit_mask4_sparse(out_sel->idx, out_n, r, hit);
        }
    }

    out_n = vx_filter_u64_tail(col, validity, in_sel, out_sel, op, rhs, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

uint32_t vx_simd_neon_filter_u64(const uint64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, uint64_t rhs) {
    switch (op) {
    case VX_CMP_EQ: return vx_neon_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_EQ, rhs);
    case VX_CMP_NE: return vx_neon_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_NE, rhs);
    case VX_CMP_LT: return vx_neon_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_LT, rhs);
    case VX_CMP_LE: return vx_neon_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_LE, rhs);
    case VX_CMP_GT: return vx_neon_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_GT, rhs);
    case VX_CMP_GE: return vx_neon_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_GE, rhs);
    }
    out_sel->count = 0; out_sel->is_identity = true; return 0;
}

/* --- minmax_f64 (native IEEE-754 FMIN/FMAX: NaN-propagating & -0.0 < +0.0) --- */
vx_agg_minmax_f64_t vx_simd_neon_minmax_f64(const double *col, const uint64_t *validity,
                                            const vx_sel_t *sel) {
    const uint32_t n = sel->count;
    if (n == 0) {
        return (vx_agg_minmax_f64_t){ .min_v = 0.0, .max_v = 0.0, .valid_n = 0 };
    }

    const float64x2_t v_pos_inf = vdupq_n_f64((double)INFINITY);
    const float64x2_t v_neg_inf = vdupq_n_f64(-(double)INFINITY);

    float64x2_t min01   = v_pos_inf;
    float64x2_t min23   = v_pos_inf;
    float64x2_t max01   = v_neg_inf;
    float64x2_t max23   = v_neg_inf;
    uint64_t    valid_n = 0;
    uint32_t    i       = 0;

    if (sel->is_identity) {
        if (!validity) {
            for (; i + 4u <= n; i += 4u) {
                float64x2_t v01 = vld1q_f64(&col[i + 0u]);
                float64x2_t v23 = vld1q_f64(&col[i + 2u]);
                min01 = vminq_f64(min01, v01);
                min23 = vminq_f64(min23, v23);
                max01 = vmaxq_f64(max01, v01);
                max23 = vmaxq_f64(max23, v23);
            }
            valid_n = i;
        } else {
            for (; i + 4u <= n; i += 4u) {
                uint32_t valid4 = (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu);
                if (!valid4) continue;

                float64x2_t v01 = vld1q_f64(&col[i + 0u]);
                float64x2_t v23 = vld1q_f64(&col[i + 2u]);
                uint64x2_t  m01 = vld1q_u64(k_neon_mask2[valid4 & 3u]);
                uint64x2_t  m23 = vld1q_u64(k_neon_mask2[valid4 >> 2]);

                /* Pure bitwise select keeps NULL NaNs out of vminq/vmaxq */
                min01 = vminq_f64(min01, vbslq_f64(m01, v01, v_pos_inf));
                min23 = vminq_f64(min23, vbslq_f64(m23, v23, v_pos_inf));
                max01 = vmaxq_f64(max01, vbslq_f64(m01, v01, v_neg_inf));
                max23 = vmaxq_f64(max23, vbslq_f64(m23, v23, v_neg_inf));
                valid_n += vx_popcnt32(valid4);
            }
        }
    } else {
        for (; i + 4u <= n; i += 4u) {
            uint32_t r0 = sel->idx[i + 0u], r1 = sel->idx[i + 1u];
            uint32_t r2 = sel->idx[i + 2u], r3 = sel->idx[i + 3u];
            uint32_t valid4 = 0xFu;
            if (validity) {
                valid4 = ((uint32_t)vx_num_valid_bit(validity, r0) << 0) |
                         ((uint32_t)vx_num_valid_bit(validity, r1) << 1) |
                         ((uint32_t)vx_num_valid_bit(validity, r2) << 2) |
                         ((uint32_t)vx_num_valid_bit(validity, r3) << 3);
                if (!valid4) continue;
            }

            double      tmp[4] = { col[r0], col[r1], col[r2], col[r3] };
            float64x2_t v01    = vld1q_f64(&tmp[0]);
            float64x2_t v23    = vld1q_f64(&tmp[2]);
            if (valid4 != 0xFu) {
                uint64x2_t m01 = vld1q_u64(k_neon_mask2[valid4 & 3u]);
                uint64x2_t m23 = vld1q_u64(k_neon_mask2[valid4 >> 2]);
                min01 = vminq_f64(min01, vbslq_f64(m01, v01, v_pos_inf));
                min23 = vminq_f64(min23, vbslq_f64(m23, v23, v_pos_inf));
                max01 = vmaxq_f64(max01, vbslq_f64(m01, v01, v_neg_inf));
                max23 = vmaxq_f64(max23, vbslq_f64(m23, v23, v_neg_inf));
            } else {
                min01 = vminq_f64(min01, v01);
                min23 = vminq_f64(min23, v23);
                max01 = vmaxq_f64(max01, v01);
                max23 = vmaxq_f64(max23, v23);
            }
            valid_n += vx_popcnt32(valid4);
        }
    }

    float64x2_t mn2 = vminq_f64(min01, min23);
    float64x2_t mx2 = vmaxq_f64(max01, max23);

    vx_agg_minmax_f64_t acc = {
        .min_v   = vx_f64_min_ieee(vgetq_lane_f64(mn2, 0), vgetq_lane_f64(mn2, 1)),
        .max_v   = vx_f64_max_ieee(vgetq_lane_f64(mx2, 0), vgetq_lane_f64(mx2, 1)),
        .valid_n = valid_n
    };
    return vx_minmax_f64_tail(col, validity, sel, i, n, acc);
}
#else
typedef int vx_simd_neon_unused_t;
#endif /* __aarch64__ */

