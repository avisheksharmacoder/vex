#if (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__)
#include <immintrin.h>
#include <stdalign.h>
#include "vx_simd_str_common.h"
#include "vx_simd_num_common.h"

static inline __m256i vx_avx2_pack2_128(__m128i lo, __m128i hi) {
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

/* Extracts 4-bit inline equality mask from two 256-bit cmpeq_epi64 results.
 * Each 128-bit lane matches iff both its lo and hi 64-bit halves are all-1s. */
static inline uint32_t vx_avx2_inline_mask4(__m256i eq01, __m256i eq23) {
    uint32_t mm01 = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(eq01));
    uint32_t mm23 = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(eq23));
    uint32_t m01  = mm01 & (mm01 >> 1); /* bit 0 = row 0, bit 2 = row 1 */
    uint32_t m23  = mm23 & (mm23 >> 1); /* bit 0 = row 2, bit 2 = row 3 */
    return (m01 & 1u) | ((m01 >> 1) & 2u) | ((m23 & 1u) << 2) | ((m23 & 4u) << 1);
}

/* Extracts 4-bit prefix+len mask from even 64-bit lanes (raw.lo) only. */
static inline uint32_t vx_avx2_prefix_mask4(__m256i eq01, __m256i eq23) {
    uint32_t mm01 = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(eq01));
    uint32_t mm23 = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(eq23));
    return (mm01 & 1u) | ((mm01 >> 1) & 2u) | ((mm23 & 1u) << 2) | ((mm23 & 4u) << 1);
}

uint32_t vx_simd_avx2_filter_str_eq(const vx_strview_t *col, const vx_strpool_t *pool,
                                    const uint64_t *validity, const vx_sel_t *in_sel,
                                    vx_sel_t *out_sel, const uint8_t *rhs, int32_t rhs_len,
                                    bool negate) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const vx_strview_t target  = vx_make_str_target(rhs, rhs_len);
    const bool         is_inl  = (rhs_len <= VX_STR_INLINE_MAX);
    const bool         in_id   = in_sel->is_identity;
    const __m128i      t128    = _mm_loadu_si128((const __m128i *)&target);
    const __m256i      t_inl   = _mm256_broadcastsi128_si256(t128);
    const __m256i      t_lo    = _mm256_set1_epi64x((int64_t)target.raw.lo);

    uint32_t out_n = 0;
    uint32_t i     = 0;

    if (in_id) {
        /* Dense fast path: contiguous 64B cache-line loads + 4-bit validity slices */
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (valid4 == 0) continue;

            __m256i v01 = _mm256_loadu_si256((const __m256i *)&col[i]);
            __m256i v23 = _mm256_loadu_si256((const __m256i *)&col[i + 2u]);

            uint32_t eq_mask;
            if (is_inl) {
                eq_mask = vx_avx2_inline_mask4(_mm256_cmpeq_epi64(v01, t_inl),
                                               _mm256_cmpeq_epi64(v23, t_inl));
            } else {
                uint32_t pref = vx_avx2_prefix_mask4(_mm256_cmpeq_epi64(v01, t_lo),
                                                     _mm256_cmpeq_epi64(v23, t_lo));
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
        /* Sparse path: preload all 4 row indices before writing out_sel (alias-safe) */
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
                __m256i v01 = vx_avx2_pack2_128(_mm_loadu_si128((const __m128i *)&col[r[0]]),
                                                _mm_loadu_si128((const __m128i *)&col[r[1]]));
                __m256i v23 = vx_avx2_pack2_128(_mm_loadu_si128((const __m128i *)&col[r[2]]),
                                                _mm_loadu_si128((const __m128i *)&col[r[3]]));
                eq_mask = vx_avx2_inline_mask4(_mm256_cmpeq_epi64(v01, t_inl),
                                               _mm256_cmpeq_epi64(v23, t_inl));
            } else {
                /* Only gather the 8-byte raw.lo word (len + 4B prefix) */
                __m256i lo_vec = _mm256_set_epi64x((int64_t)col[r[3]].raw.lo,
                                                    (int64_t)col[r[2]].raw.lo,
                                                    (int64_t)col[r[1]].raw.lo,
                                                    (int64_t)col[r[0]].raw.lo);
                uint32_t pref  = (uint32_t)_mm256_movemask_pd(
                                     _mm256_castsi256_pd(_mm256_cmpeq_epi64(lo_vec, t_lo)));
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

/* 512B L1-hot LUT mapping 4-bit validity mask (0..15) -> 4x64-bit lane masks */
static const alignas(32) uint64_t k_avx2_mask4[16][4] = {
    {  0ULL,  0ULL,  0ULL,  0ULL }, { ~0ULL,  0ULL,  0ULL,  0ULL },
    {  0ULL, ~0ULL,  0ULL,  0ULL }, { ~0ULL, ~0ULL,  0ULL,  0ULL },
    {  0ULL,  0ULL, ~0ULL,  0ULL }, { ~0ULL,  0ULL, ~0ULL,  0ULL },
    {  0ULL, ~0ULL, ~0ULL,  0ULL }, { ~0ULL, ~0ULL, ~0ULL,  0ULL },
    {  0ULL,  0ULL,  0ULL, ~0ULL }, { ~0ULL,  0ULL,  0ULL, ~0ULL },
    {  0ULL, ~0ULL,  0ULL, ~0ULL }, { ~0ULL, ~0ULL,  0ULL, ~0ULL },
    {  0ULL,  0ULL, ~0ULL, ~0ULL }, { ~0ULL,  0ULL, ~0ULL, ~0ULL },
    {  0ULL, ~0ULL, ~0ULL, ~0ULL }, { ~0ULL, ~0ULL, ~0ULL, ~0ULL }
};

/* --- filter_i64 --- */
static inline __m256i vx_avx2_cmp_i64(__m256i v, __m256i rhs, __m256i all1, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return _mm256_cmpeq_epi64(v, rhs);
    case VX_CMP_NE: return _mm256_xor_si256(_mm256_cmpeq_epi64(v, rhs), all1);
    case VX_CMP_GT: return _mm256_cmpgt_epi64(v, rhs);
    case VX_CMP_LT: return _mm256_cmpgt_epi64(rhs, v);
    case VX_CMP_GE: return _mm256_xor_si256(_mm256_cmpgt_epi64(rhs, v), all1);
    case VX_CMP_LE: return _mm256_xor_si256(_mm256_cmpgt_epi64(v, rhs), all1);
    }
    return _mm256_setzero_si256();
}

static inline uint32_t vx_avx2_filter_i64_impl(const int64_t *col, const uint64_t *validity,
                                               const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                               vx_cmp_op_t op, int64_t rhs) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const bool    in_id = in_sel->is_identity;
    const __m256i vrhs  = _mm256_set1_epi64x(rhs);
    const __m256i all1  = _mm256_set1_epi64x(-1LL);
    uint32_t      out_n = 0;
    uint32_t      i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (!valid4) continue;

            __m256i  v   = _mm256_loadu_si256((const __m256i *)&col[i]);
            __m256i  cmp = vx_avx2_cmp_i64(v, vrhs, all1, op);
            uint32_t hit = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(cmp)) & valid4;
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

            __m256i  v   = _mm256_set_epi64x(col[r[3]], col[r[2]], col[r[1]], col[r[0]]);
            __m256i  cmp = vx_avx2_cmp_i64(v, vrhs, all1, op);
            uint32_t hit = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(cmp)) & valid4;
            out_n = vx_emit_mask4_sparse(out_sel->idx, out_n, r, hit);
        }
    }

    out_n = vx_filter_i64_tail(col, validity, in_sel, out_sel, op, rhs, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

uint32_t vx_simd_avx2_filter_i64(const int64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, int64_t rhs) {
    switch (op) {
    case VX_CMP_EQ: return vx_avx2_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_EQ, rhs);
    case VX_CMP_NE: return vx_avx2_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_NE, rhs);
    case VX_CMP_LT: return vx_avx2_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_LT, rhs);
    case VX_CMP_LE: return vx_avx2_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_LE, rhs);
    case VX_CMP_GT: return vx_avx2_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_GT, rhs);
    case VX_CMP_GE: return vx_avx2_filter_i64_impl(col, validity, in_sel, out_sel, VX_CMP_GE, rhs);
    }
    out_sel->count = 0; out_sel->is_identity = true; return 0;
}

/* --- filter_f64 (exact IEEE-754 NaN & -0.0 parity) --- */
static inline __m256d vx_avx2_cmp_f64(__m256d v, __m256d rhs, vx_cmp_op_t op) {
    switch (op) {
    case VX_CMP_EQ: return _mm256_cmp_pd(v, rhs, _CMP_EQ_OQ);  /* NaN -> false */
    case VX_CMP_NE: return _mm256_cmp_pd(v, rhs, _CMP_NEQ_UQ); /* NaN -> true  */
    case VX_CMP_LT: return _mm256_cmp_pd(v, rhs, _CMP_LT_OQ);  /* NaN -> false */
    case VX_CMP_LE: return _mm256_cmp_pd(v, rhs, _CMP_LE_OQ);  /* NaN -> false */
    case VX_CMP_GT: return _mm256_cmp_pd(v, rhs, _CMP_GT_OQ);  /* NaN -> false */
    case VX_CMP_GE: return _mm256_cmp_pd(v, rhs, _CMP_GE_OQ);  /* NaN -> false */
    }
    return _mm256_setzero_pd();
}

static inline uint32_t vx_avx2_filter_f64_impl(const double *col, const uint64_t *validity,
                                               const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                               vx_cmp_op_t op, double rhs) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const bool    in_id = in_sel->is_identity;
    const __m256d vrhs  = _mm256_set1_pd(rhs);
    uint32_t      out_n = 0;
    uint32_t      i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (!valid4) continue;

            __m256d  v   = _mm256_loadu_pd(&col[i]);
            __m256d  cmp = vx_avx2_cmp_f64(v, vrhs, op);
            uint32_t hit = (uint32_t)_mm256_movemask_pd(cmp) & valid4;
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

            __m256d  v   = _mm256_set_pd(col[r[3]], col[r[2]], col[r[1]], col[r[0]]);
            __m256d  cmp = vx_avx2_cmp_f64(v, vrhs, op);
            uint32_t hit = (uint32_t)_mm256_movemask_pd(cmp) & valid4;
            out_n = vx_emit_mask4_sparse(out_sel->idx, out_n, r, hit);
        }
    }

    out_n = vx_filter_f64_tail(col, validity, in_sel, out_sel, op, rhs, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

uint32_t vx_simd_avx2_filter_f64(const double *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, double rhs) {
    switch (op) {
    case VX_CMP_EQ: return vx_avx2_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_EQ, rhs);
    case VX_CMP_NE: return vx_avx2_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_NE, rhs);
    case VX_CMP_LT: return vx_avx2_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_LT, rhs);
    case VX_CMP_LE: return vx_avx2_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_LE, rhs);
    case VX_CMP_GT: return vx_avx2_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_GT, rhs);
    case VX_CMP_GE: return vx_avx2_filter_f64_impl(col, validity, in_sel, out_sel, VX_CMP_GE, rhs);
    }
    out_sel->count = 0; out_sel->is_identity = true; return 0;
}

/* --- sum_f64 (4-lane deterministic tree reduction, bitwise NULL masking) --- */
double vx_simd_avx2_sum_f64(const double *col, const uint64_t *validity,
                            const vx_sel_t *sel, uint64_t *out_valid_n) {
    const uint32_t n = sel->count;
    if (n == 0) {
        if (out_valid_n) *out_valid_n = 0;
        return 0.0;
    }

    __m256d  acc     = _mm256_setzero_pd();
    uint64_t valid_n = 0;
    uint32_t i       = 0;

    if (sel->is_identity) {
        if (!validity) {
            for (; i + 4u <= n; i += 4u) {
                acc = _mm256_add_pd(acc, _mm256_loadu_pd(&col[i]));
            }
            valid_n = i;
        } else {
            for (; i + 4u <= n; i += 4u) {
                uint32_t valid4 = (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu);
                if (!valid4) {
                    acc = _mm256_add_pd(acc, _mm256_setzero_pd());
                    continue;
                }
                __m256d v    = _mm256_loadu_pd(&col[i]);
                __m256d mask = _mm256_castsi256_pd(
                                   _mm256_load_si256((const __m256i *)k_avx2_mask4[valid4]));
                acc     = _mm256_add_pd(acc, _mm256_and_pd(v, mask));
                valid_n += vx_popcnt32(valid4);
            }
        }
    } else {
        if (!validity) {
            for (; i + 4u <= n; i += 4u) {
                __m256d v = _mm256_set_pd(col[sel->idx[i + 3u]], col[sel->idx[i + 2u]],
                                          col[sel->idx[i + 1u]], col[sel->idx[i + 0u]]);
                acc = _mm256_add_pd(acc, v);
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
                    acc = _mm256_add_pd(acc, _mm256_setzero_pd());
                    continue;
                }
                __m256d v    = _mm256_set_pd(col[r3], col[r2], col[r1], col[r0]);
                __m256d mask = _mm256_castsi256_pd(
                                   _mm256_load_si256((const __m256i *)k_avx2_mask4[valid4]));
                acc     = _mm256_add_pd(acc, _mm256_and_pd(v, mask));
                valid_n += vx_popcnt32(valid4);
            }
        }
    }

    alignas(32) double lanes[4];
    _mm256_store_pd(lanes, acc);
    double sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    sum = vx_sum_f64_tail(col, validity, sel, i, n, sum, &valid_n);
    if (out_valid_n) *out_valid_n = valid_n;
    return sum;
}

/* --- filter_u64 (XOR INT64_MIN bias for ordered comparisons) --- */
static inline __m256i vx_avx2_cmp_u64(__m256i v, __m256i vrhs, __m256i vrhs_b,
                                      __m256i sign_bit, __m256i all1, vx_cmp_op_t op) {
    if (op == VX_CMP_EQ) return _mm256_cmpeq_epi64(v, vrhs);
    if (op == VX_CMP_NE) return _mm256_xor_si256(_mm256_cmpeq_epi64(v, vrhs), all1);

    /* Order-preserving map: [0, UINT64_MAX] -> [INT64_MIN, INT64_MAX] */
    __m256i vb = _mm256_xor_si256(v, sign_bit);
    switch (op) {
    case VX_CMP_GT: return _mm256_cmpgt_epi64(vb, vrhs_b);
    case VX_CMP_LT: return _mm256_cmpgt_epi64(vrhs_b, vb);
    case VX_CMP_GE: return _mm256_xor_si256(_mm256_cmpgt_epi64(vrhs_b, vb), all1);
    case VX_CMP_LE: return _mm256_xor_si256(_mm256_cmpgt_epi64(vb, vrhs_b), all1);
    default:        return _mm256_setzero_si256();
    }
}

static inline uint32_t vx_avx2_filter_u64_impl(const uint64_t *col, const uint64_t *validity,
                                               const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                               vx_cmp_op_t op, uint64_t rhs) {
    const uint32_t n = in_sel->count;
    if (n == 0) {
        out_sel->count       = 0;
        out_sel->is_identity = true;
        return 0;
    }

    const bool    in_id    = in_sel->is_identity;
    const __m256i sign_bit = _mm256_set1_epi64x(INT64_MIN);
    const __m256i all1     = _mm256_set1_epi64x(-1LL);
    const __m256i vrhs     = _mm256_set1_epi64x(vx_u64_to_i64_bits(rhs));
    const __m256i vrhs_b   = _mm256_xor_si256(vrhs, sign_bit);

    uint32_t out_n = 0;
    uint32_t i     = 0;

    if (in_id) {
        for (; i + 4u <= n; i += 4u) {
            uint32_t valid4 = validity
                ? (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu)
                : 0xFu;
            if (!valid4) continue;

            __m256i  v   = _mm256_loadu_si256((const __m256i *)&col[i]);
            __m256i  cmp = vx_avx2_cmp_u64(v, vrhs, vrhs_b, sign_bit, all1, op);
            uint32_t hit = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(cmp)) & valid4;
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

            __m256i  v   = _mm256_set_epi64x(vx_u64_to_i64_bits(col[r[3]]),
                                             vx_u64_to_i64_bits(col[r[2]]),
                                             vx_u64_to_i64_bits(col[r[1]]),
                                             vx_u64_to_i64_bits(col[r[0]]));
            __m256i  cmp = vx_avx2_cmp_u64(v, vrhs, vrhs_b, sign_bit, all1, op);
            uint32_t hit = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(cmp)) & valid4;
            out_n = vx_emit_mask4_sparse(out_sel->idx, out_n, r, hit);
        }
    }

    out_n = vx_filter_u64_tail(col, validity, in_sel, out_sel, op, rhs, i, n, out_n);
    out_sel->count       = out_n;
    out_sel->is_identity = (in_id && out_n == n);
    return out_n;
}

uint32_t vx_simd_avx2_filter_u64(const uint64_t *col, const uint64_t *validity,
                                 const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                 vx_cmp_op_t op, uint64_t rhs) {
    switch (op) {
    case VX_CMP_EQ: return vx_avx2_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_EQ, rhs);
    case VX_CMP_NE: return vx_avx2_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_NE, rhs);
    case VX_CMP_LT: return vx_avx2_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_LT, rhs);
    case VX_CMP_LE: return vx_avx2_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_LE, rhs);
    case VX_CMP_GT: return vx_avx2_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_GT, rhs);
    case VX_CMP_GE: return vx_avx2_filter_u64_impl(col, validity, in_sel, out_sel, VX_CMP_GE, rhs);
    }
    out_sel->count = 0; out_sel->is_identity = true; return 0;
}

/* --- minmax_f64 (exact IEEE-754 NaN propagation, -0.0 < +0.0, NULL-safe) --- */
vx_agg_minmax_f64_t vx_simd_avx2_minmax_f64(const double *col, const uint64_t *validity,
                                            const vx_sel_t *sel) {
    const uint32_t n = sel->count;
    if (n == 0) {
        return (vx_agg_minmax_f64_t){ .min_v = 0.0, .max_v = 0.0, .valid_n = 0 };
    }

    const __m256d v_pos_inf = _mm256_set1_pd((double)INFINITY);
    const __m256d v_neg_inf = _mm256_set1_pd(-(double)INFINITY);
    const __m256i v_neg0_i  = _mm256_set1_epi64x(INT64_MIN);       /* -0.0 bit pattern */
    const __m256i v_pos0_i  = _mm256_setzero_si256();              /* +0.0 bit pattern */

    __m256d  vmin      = v_pos_inf;
    __m256d  vmax      = v_neg_inf;
    __m256d  any_nan   = _mm256_setzero_pd();
    __m256i  seen_neg0 = _mm256_setzero_si256();
    __m256i  seen_pos0 = _mm256_setzero_si256();
    uint64_t valid_n   = 0;
    uint32_t i         = 0;

    if (sel->is_identity) {
        if (!validity) {
            for (; i + 4u <= n; i += 4u) {
                __m256d v  = _mm256_loadu_pd(&col[i]);
                __m256i vi = _mm256_castpd_si256(v);
                vmin       = _mm256_min_pd(vmin, v);
                vmax       = _mm256_max_pd(vmax, v);
                any_nan    = _mm256_or_pd(any_nan, _mm256_cmp_pd(v, v, _CMP_UNORD_Q));
                seen_neg0  = _mm256_or_si256(seen_neg0, _mm256_cmpeq_epi64(vi, v_neg0_i));
                seen_pos0  = _mm256_or_si256(seen_pos0, _mm256_cmpeq_epi64(vi, v_pos0_i));
            }
            valid_n = i;
        } else {
            for (; i + 4u <= n; i += 4u) {
                uint32_t valid4 = (uint32_t)((validity[i >> 6] >> (i & 63u)) & 0xFu);
                if (!valid4) continue;

                __m256d v    = _mm256_loadu_pd(&col[i]);
                __m256d mask = _mm256_castsi256_pd(
                                   _mm256_load_si256((const __m256i *)k_avx2_mask4[valid4]));
                /* Replace NULL lanes with +Inf / -Inf: neither triggers NaN, -0.0, or +0.0 */
                __m256d v_lo = _mm256_blendv_pd(v_pos_inf, v, mask);
                __m256d v_hi = _mm256_blendv_pd(v_neg_inf, v, mask);
                __m256i vi   = _mm256_castpd_si256(v_lo);

                vmin      = _mm256_min_pd(vmin, v_lo);
                vmax      = _mm256_max_pd(vmax, v_hi);
                any_nan   = _mm256_or_pd(any_nan, _mm256_cmp_pd(v_lo, v_lo, _CMP_UNORD_Q));
                seen_neg0 = _mm256_or_si256(seen_neg0, _mm256_cmpeq_epi64(vi, v_neg0_i));
                seen_pos0 = _mm256_or_si256(seen_pos0, _mm256_cmpeq_epi64(vi, v_pos0_i));
                valid_n  += vx_popcnt32(valid4);
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

            __m256d v    = _mm256_set_pd(col[r3], col[r2], col[r1], col[r0]);
            __m256d v_lo = v;
            __m256d v_hi = v;
            if (valid4 != 0xFu) {
                __m256d mask = _mm256_castsi256_pd(
                                   _mm256_load_si256((const __m256i *)k_avx2_mask4[valid4]));
                v_lo = _mm256_blendv_pd(v_pos_inf, v, mask);
                v_hi = _mm256_blendv_pd(v_neg_inf, v, mask);
            }
            __m256i vi = _mm256_castpd_si256(v_lo);

            vmin      = _mm256_min_pd(vmin, v_lo);
            vmax      = _mm256_max_pd(vmax, v_hi);
            any_nan   = _mm256_or_pd(any_nan, _mm256_cmp_pd(v_lo, v_lo, _CMP_UNORD_Q));
            seen_neg0 = _mm256_or_si256(seen_neg0, _mm256_cmpeq_epi64(vi, v_neg0_i));
            seen_pos0 = _mm256_or_si256(seen_pos0, _mm256_cmpeq_epi64(vi, v_pos0_i));
            valid_n  += vx_popcnt32(valid4);
        }
    }

    vx_agg_minmax_f64_t acc = {
        .min_v   = (double)INFINITY,
        .max_v   = -(double)INFINITY,
        .valid_n = valid_n
    };

    if (valid_n > 0) {
        if (_mm256_movemask_pd(any_nan) != 0) {
            acc.min_v = (double)NAN;
            acc.max_v = (double)NAN;
        } else {
            alignas(32) double lo[4], hi[4];
            _mm256_store_pd(lo, vmin);
            _mm256_store_pd(hi, vmax);

            double mn = vx_f64_min_ieee(vx_f64_min_ieee(lo[0], lo[1]),
                                        vx_f64_min_ieee(lo[2], lo[3]));
            double mx = vx_f64_max_ieee(vx_f64_max_ieee(hi[0], hi[1]),
                                        vx_f64_max_ieee(hi[2], hi[3]));

            /* Reconcile -0.0 vs +0.0 lost by AVX2 non-commutative vminpd/vmaxpd */
            if (mn == 0.0) mn = (_mm256_movemask_epi8(seen_neg0) != 0) ? -0.0 : +0.0;
            if (mx == 0.0) mx = (_mm256_movemask_epi8(seen_pos0) != 0) ? +0.0 : -0.0;
            acc.min_v = mn;
            acc.max_v = mx;
        }
    }

    return vx_minmax_f64_tail(col, validity, sel, i, n, acc);
}

/* ============================================================================
 * V1 Backwards-Compatibility AVX2 Dispatcher
 * ============================================================================ */
static const uint32_t k_avx2_lut4[16][4] = {
    {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0},
    {2, 0, 0, 0}, {0, 2, 0, 0}, {1, 2, 0, 0}, {0, 1, 2, 0},
    {3, 0, 0, 0}, {0, 3, 0, 0}, {1, 3, 0, 0}, {0, 1, 3, 0},
    {2, 3, 0, 0}, {0, 2, 3, 0}, {1, 2, 3, 0}, {0, 1, 2, 3}
};
static const uint8_t k_avx2_cnt4[16] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
};

#define VX_DEFINE_AVX2_F64(NAME, IMM, SCALAR_EXPR)                              \
    static uint32_t NAME(const double *restrict col, uint32_t n,                \
                         double val, uint32_t *restrict sel_out) {              \
        uint32_t out_n = 0;                                                     \
        uint32_t i = 0;                                                         \
        __m256d vval = _mm256_set1_pd(val);                                     \
        for (; i + 4U <= n; i += 4U) {                                          \
            __m256d v = _mm256_loadu_pd(col + i);                               \
            int m = _mm256_movemask_pd(_mm256_cmp_pd(v, vval, IMM));            \
            __m128i offs = _mm_loadu_si128((const __m128i *)k_avx2_lut4[m]);    \
            __m128i idxs = _mm_add_epi32(_mm_set1_epi32((int)i), offs);         \
            _mm_storeu_si128((__m128i *)(sel_out + out_n), idxs);               \
            out_n += k_avx2_cnt4[m];                                            \
        }                                                                       \
        for (; i < n; i++) {                                                    \
            double x = col[i];                                                  \
            sel_out[out_n] = i;                                                 \
            out_n += (uint32_t)(SCALAR_EXPR);                                   \
        }                                                                       \
        return out_n;                                                           \
    }

VX_DEFINE_AVX2_F64(avx2_f64_eq, _CMP_EQ_OQ,  x == val)
VX_DEFINE_AVX2_F64(avx2_f64_ne, _CMP_NEQ_UQ, x != val)
VX_DEFINE_AVX2_F64(avx2_f64_lt, _CMP_LT_OQ,  x <  val)
VX_DEFINE_AVX2_F64(avx2_f64_le, _CMP_LE_OQ,  x <= val)
VX_DEFINE_AVX2_F64(avx2_f64_gt, _CMP_GT_OQ,  x >  val)
VX_DEFINE_AVX2_F64(avx2_f64_ge, _CMP_GE_OQ,  x >= val)

uint32_t vx_filter_f64_avx2_dispatch(const double *col, uint32_t n,
                                     vx_cmp_op_t op, double val, uint32_t *sel_out) {
    switch (op) {
        case VX_CMP_EQ: return avx2_f64_eq(col, n, val, sel_out);
        case VX_CMP_NE: return avx2_f64_ne(col, n, val, sel_out);
        case VX_CMP_LT: return avx2_f64_lt(col, n, val, sel_out);
        case VX_CMP_LE: return avx2_f64_le(col, n, val, sel_out);
        case VX_CMP_GT: return avx2_f64_gt(col, n, val, sel_out);
        case VX_CMP_GE: return avx2_f64_ge(col, n, val, sel_out);
        default:        return 0;
    }
}
#else
#include "../include/vx_simd.h"

uint32_t vx_filter_f64_avx2_dispatch(const double *col, uint32_t n,
                                     vx_cmp_op_t op, double val, uint32_t *sel_out) {
    uint32_t out_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        double x = col[i];
        bool match = false;
        switch (op) {
            case VX_CMP_EQ: match = (x == val); break;
            case VX_CMP_NE: match = (x != val); break;
            case VX_CMP_LT: match = (x <  val); break;
            case VX_CMP_LE: match = (x <= val); break;
            case VX_CMP_GT: match = (x >  val); break;
            case VX_CMP_GE: match = (x >= val); break;
            default:        break;
        }
        sel_out[out_n] = i;
        out_n += (uint32_t)match;
    }
    return out_n;
}

#endif /* (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__) */
