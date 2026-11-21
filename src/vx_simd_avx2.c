#include "vx_simd.h"

#if defined(__AVX2__)
#include <immintrin.h>

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

#endif

