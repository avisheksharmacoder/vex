#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "src/vx_simd_num_common.h"

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "ASSERT_EQ failed at %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while(0)

#define ASSERT_MEMEQ(a, b, sz) do { \
    if (memcmp(&(a), &(b), (sz)) != 0) { \
        fprintf(stderr, "ASSERT_MEMEQ failed at %s:%d\n", __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

static void test_filter_i64_parity(void) {
    printf("Testing filter_i64 parity and operators...\n");
    const uint32_t N = 127; /* non-multiple of 4 to test both vector loop and tail */
    int64_t col[128];
    uint64_t validity[2] = { 0, 0 };

    for (uint32_t i = 0; i < N; ++i) {
        col[i] = (int64_t)(i * 3) - 50; /* mix of negative, zero, positive */
        /* Every 3rd row is null */
        if (i % 3 != 0) {
            validity[i >> 6] |= (1ULL << (i & 63u));
        }
    }

    vx_sel_t in_sel_dense;
    in_sel_dense.count = N;
    in_sel_dense.is_identity = true;

    vx_sel_t in_sel_sparse;
    in_sel_sparse.count = 0;
    in_sel_sparse.is_identity = false;
    for (uint32_t i = 0; i < N; i += 2) {
        in_sel_sparse.idx[in_sel_sparse.count++] = i;
    }

    vx_cmp_op_t ops[] = { VX_CMP_EQ, VX_CMP_NE, VX_CMP_LT, VX_CMP_LE, VX_CMP_GT, VX_CMP_GE };
    int64_t test_rhs[] = { -50, 0, 10, 40, 100, 999 };

    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
        for (size_t r = 0; r < sizeof(test_rhs) / sizeof(test_rhs[0]); ++r) {
            vx_cmp_op_t op = ops[o];
            int64_t rhs = test_rhs[r];

            /* 1. Dense with validity */
            vx_sel_t out_ref, out_avx2;
            memset(&out_ref, 0, sizeof(out_ref));
            memset(&out_avx2, 0, sizeof(out_avx2));

            uint32_t n_ref = vx_simd_scalar_filter_i64_ref(col, validity, &in_sel_dense, &out_ref, op, rhs);
#if defined(__x86_64__) || defined(_M_X64)
            uint32_t n_avx = vx_simd_avx2_filter_i64(col, validity, &in_sel_dense, &out_avx2, op, rhs);
            ASSERT_EQ(n_ref, n_avx);
            ASSERT_EQ(out_ref.count, out_avx2.count);
            ASSERT_EQ(out_ref.is_identity, out_avx2.is_identity);
            for (uint32_t k = 0; k < n_ref; ++k) {
                ASSERT_EQ(out_ref.idx[k], out_avx2.idx[k]);
            }
#endif

            /* 2. Dense without validity (all valid) */
            memset(&out_ref, 0, sizeof(out_ref));
            memset(&out_avx2, 0, sizeof(out_avx2));
            n_ref = vx_simd_scalar_filter_i64_ref(col, NULL, &in_sel_dense, &out_ref, op, rhs);
#if defined(__x86_64__) || defined(_M_X64)
            n_avx = vx_simd_avx2_filter_i64(col, NULL, &in_sel_dense, &out_avx2, op, rhs);
            ASSERT_EQ(n_ref, n_avx);
            for (uint32_t k = 0; k < n_ref; ++k) {
                ASSERT_EQ(out_ref.idx[k], out_avx2.idx[k]);
            }
#endif

            /* 3. Sparse with validity */
            memset(&out_ref, 0, sizeof(out_ref));
            memset(&out_avx2, 0, sizeof(out_avx2));
            n_ref = vx_simd_scalar_filter_i64_ref(col, validity, &in_sel_sparse, &out_ref, op, rhs);
#if defined(__x86_64__) || defined(_M_X64)
            n_avx = vx_simd_avx2_filter_i64(col, validity, &in_sel_sparse, &out_avx2, op, rhs);
            ASSERT_EQ(n_ref, n_avx);
            for (uint32_t k = 0; k < n_ref; ++k) {
                ASSERT_EQ(out_ref.idx[k], out_avx2.idx[k]);
            }
#endif
        }
    }
    printf("  [PASS] test_filter_i64_parity\n");
}

static void test_filter_f64_ieee754(void) {
    printf("Testing filter_f64 IEEE-754 semantics (NaN, -0.0, Inf)...\n");
    const uint32_t N = 10;
    double col[16];
    col[0] = 1.0;
    col[1] = 0.0;
    col[2] = -0.0;
    col[3] = NAN;
    col[4] = INFINITY;
    col[5] = -INFINITY;
    col[6] = 2.5;
    col[7] = -2.5;
    col[8] = NAN;
    col[9] = 10.0;

    uint64_t validity = 0x3FF; /* all 10 valid */

    vx_sel_t in_sel;
    in_sel.count = N;
    in_sel.is_identity = true;

    /* Check NaN ordered comparison: ==, <, <=, >, >= must NOT match col[3] or col[8] */
    vx_sel_t out;
#if defined(__x86_64__) || defined(_M_X64)
    vx_simd_avx2_filter_f64(col, &validity, &in_sel, &out, VX_CMP_EQ, 0.0);
    /* col[1] is 0.0, col[2] is -0.0. In IEEE-754, 0.0 == -0.0 */
    bool has_1 = false, has_2 = false, has_nan = false;
    for (uint32_t i = 0; i < out.count; ++i) {
        if (out.idx[i] == 1) has_1 = true;
        if (out.idx[i] == 2) has_2 = true;
        if (out.idx[i] == 3 || out.idx[i] == 8) has_nan = true;
    }
    assert(has_1 && has_2 && !has_nan);

    /* Check NaN with != : must match NaN! */
    vx_simd_avx2_filter_f64(col, &validity, &in_sel, &out, VX_CMP_NE, 1.0);
    /* col[0] is 1.0 -> false. col[3] and col[8] are NaN -> NaN != 1.0 is TRUE */
    bool matched_nan = false;
    for (uint32_t i = 0; i < out.count; ++i) {
        assert(out.idx[i] != 0); /* 1.0 != 1.0 is false */
        if (out.idx[i] == 3) matched_nan = true;
    }
    assert(matched_nan);

    /* Parity test against scalar reference for all ops */
    vx_cmp_op_t ops[] = { VX_CMP_EQ, VX_CMP_NE, VX_CMP_LT, VX_CMP_LE, VX_CMP_GT, VX_CMP_GE };
    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
        vx_sel_t out_ref, out_avx;
        uint32_t n_ref = vx_simd_scalar_filter_f64_ref(col, &validity, &in_sel, &out_ref, ops[o], 0.0);
        uint32_t n_avx = vx_simd_avx2_filter_f64(col, &validity, &in_sel, &out_avx, ops[o], 0.0);
        ASSERT_EQ(n_ref, n_avx);
        for (uint32_t k = 0; k < n_ref; ++k) {
            ASSERT_EQ(out_ref.idx[k], out_avx.idx[k]);
        }
    }
#endif
    printf("  [PASS] test_filter_f64_ieee754\n");
}

static void test_sum_f64_bitwise_null_masking(void) {
    printf("Testing sum_f64 bitwise NULL masking & 4-lane parity...\n");
    const uint32_t N = 16;
    double col[16];
    uint64_t validity = 0;

    for (uint32_t i = 0; i < N; ++i) {
        col[i] = (double)(i + 1);
        validity |= (1ULL << i);
    }

    /* Set lane 2 and lane 7 to NaN, but mark them as NULL in validity!
     * Invariant 2: bitwise NULL masking zeroing with bitwise AND must prevent NaN contamination! */
    col[2] = NAN;
    validity &= ~(1ULL << 2);

    col[7] = INFINITY;
    validity &= ~(1ULL << 7);

    vx_sel_t sel;
    sel.count = N;
    sel.is_identity = true;

    uint64_t valid_n_ref = 0;
    double sum_ref = vx_simd_scalar_sum_f64_ref(col, &validity, &sel, &valid_n_ref);

    ASSERT_EQ(valid_n_ref, 14);
    assert(!isnan(sum_ref));
    assert(!isinf(sum_ref));

#if defined(__x86_64__) || defined(_M_X64)
    uint64_t valid_n_avx = 0;
    double sum_avx = vx_simd_avx2_sum_f64(col, &validity, &sel, &valid_n_avx);

    ASSERT_EQ(valid_n_ref, valid_n_avx);
    ASSERT_MEMEQ(sum_ref, sum_avx, sizeof(double));
#endif

    /* Test sparse selection with tail rows */
    vx_sel_t sparse_sel;
    sparse_sel.count = 7; /* non-multiple of 4 */
    sparse_sel.is_identity = false;
    sparse_sel.idx[0] = 0;
    sparse_sel.idx[1] = 1;
    sparse_sel.idx[2] = 2; /* NULL with NaN */
    sparse_sel.idx[3] = 3;
    sparse_sel.idx[4] = 4;
    sparse_sel.idx[5] = 5;
    sparse_sel.idx[6] = 7; /* NULL with Inf */

    valid_n_ref = 0;
    sum_ref = vx_simd_scalar_sum_f64_ref(col, &validity, &sparse_sel, &valid_n_ref);
    ASSERT_EQ(valid_n_ref, 5);
    assert(!isnan(sum_ref));

#if defined(__x86_64__) || defined(_M_X64)
    valid_n_avx = 0;
    sum_avx = vx_simd_avx2_sum_f64(col, &validity, &sparse_sel, &valid_n_avx);
    ASSERT_EQ(valid_n_ref, valid_n_avx);
    ASSERT_MEMEQ(sum_ref, sum_avx, sizeof(double));
#endif

    printf("  [PASS] test_sum_f64_bitwise_null_masking\n");
}

static void test_filter_u64_parity_and_boundaries(void) {
    printf("Testing filter_u64 parity and sign boundaries...\n");
    const uint64_t boundaries[] = {
        0ULL,
        1ULL,
        (uint64_t)INT64_MAX,
        (uint64_t)INT64_MAX + 1ULL,
        UINT64_MAX - 1ULL,
        UINT64_MAX
    };
    const uint32_t num_boundaries = sizeof(boundaries) / sizeof(boundaries[0]);

    /* Test dataset with multiple copies of each boundary and interleaved values */
    const uint32_t N = 127;
    uint64_t col[128];
    uint64_t validity[2] = { 0, 0 };

    for (uint32_t i = 0; i < N; ++i) {
        if (i < num_boundaries) {
            col[i] = boundaries[i];
        } else {
            col[i] = boundaries[i % num_boundaries] ^ ((uint64_t)i * 0x9e3779b97f4a7c15ULL);
        }
        if (i % 4 != 0) {
            validity[i >> 6] |= (1ULL << (i & 63u));
        }
    }

    vx_sel_t in_sel_dense;
    in_sel_dense.count = N;
    in_sel_dense.is_identity = true;

    vx_sel_t in_sel_sparse;
    in_sel_sparse.count = 0;
    in_sel_sparse.is_identity = false;
    for (uint32_t i = 0; i < N; i += 2) {
        in_sel_sparse.idx[in_sel_sparse.count++] = i;
    }

    vx_cmp_op_t ops[] = { VX_CMP_EQ, VX_CMP_NE, VX_CMP_LT, VX_CMP_LE, VX_CMP_GT, VX_CMP_GE };

    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
        for (size_t b = 0; b < num_boundaries; ++b) {
            vx_cmp_op_t op = ops[o];
            uint64_t rhs = boundaries[b];

            /* 1. Dense with validity */
            vx_sel_t out_ref, out_avx;
            memset(&out_ref, 0, sizeof(out_ref));
            memset(&out_avx, 0, sizeof(out_avx));

            uint32_t n_ref = vx_simd_scalar_filter_u64_ref(col, validity, &in_sel_dense, &out_ref, op, rhs);
#if defined(__x86_64__) || defined(_M_X64)
            uint32_t n_avx = vx_simd_avx2_filter_u64(col, validity, &in_sel_dense, &out_avx, op, rhs);
            ASSERT_EQ(n_ref, n_avx);
            ASSERT_EQ(out_ref.count, out_avx.count);
            ASSERT_EQ(out_ref.is_identity, out_avx.is_identity);
            for (uint32_t k = 0; k < n_ref; ++k) {
                ASSERT_EQ(out_ref.idx[k], out_avx.idx[k]);
            }
#endif

            /* 2. Dense without validity (all valid) */
            memset(&out_ref, 0, sizeof(out_ref));
            memset(&out_avx, 0, sizeof(out_avx));
            n_ref = vx_simd_scalar_filter_u64_ref(col, NULL, &in_sel_dense, &out_ref, op, rhs);
#if defined(__x86_64__) || defined(_M_X64)
            n_avx = vx_simd_avx2_filter_u64(col, NULL, &in_sel_dense, &out_avx, op, rhs);
            ASSERT_EQ(n_ref, n_avx);
            for (uint32_t k = 0; k < n_ref; ++k) {
                ASSERT_EQ(out_ref.idx[k], out_avx.idx[k]);
            }
#endif

            /* 3. Sparse with validity */
            memset(&out_ref, 0, sizeof(out_ref));
            memset(&out_avx, 0, sizeof(out_avx));
            n_ref = vx_simd_scalar_filter_u64_ref(col, validity, &in_sel_sparse, &out_ref, op, rhs);
#if defined(__x86_64__) || defined(_M_X64)
            n_avx = vx_simd_avx2_filter_u64(col, validity, &in_sel_sparse, &out_avx, op, rhs);
            ASSERT_EQ(n_ref, n_avx);
            for (uint32_t k = 0; k < n_ref; ++k) {
                ASSERT_EQ(out_ref.idx[k], out_avx.idx[k]);
            }
#endif
        }
    }
    printf("  [PASS] test_filter_u64_parity_and_boundaries\n");
}

static void test_minmax_f64_ieee754(void) {
    printf("Testing minmax_f64 exact IEEE-754 semantics & NULL safety...\n");

    /* 1. Signed zero permutations */
    {
        /* Case A: [+0.0, -0.0] */
        double data_a[4] = { +0.0, -0.0, +0.0, +0.0 };
        vx_sel_t sel_a = { .count = 2, .is_identity = true };
        vx_agg_minmax_f64_t ref_a = vx_simd_scalar_minmax_f64_ref(data_a, NULL, &sel_a);
        assert(ref_a.min_v == 0.0 && signbit(ref_a.min_v) != 0); /* -0.0 */
        assert(ref_a.max_v == 0.0 && signbit(ref_a.max_v) == 0); /* +0.0 */
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_a = vx_simd_avx2_minmax_f64(data_a, NULL, &sel_a);
        ASSERT_EQ(ref_a.valid_n, avx_a.valid_n);
        ASSERT_MEMEQ(ref_a.min_v, avx_a.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_a.max_v, avx_a.max_v, sizeof(double));
#endif

        /* Case B: [-0.0, +0.0] */
        double data_b[4] = { -0.0, +0.0, +0.0, +0.0 };
        vx_agg_minmax_f64_t ref_b = vx_simd_scalar_minmax_f64_ref(data_b, NULL, &sel_a);
        assert(ref_b.min_v == 0.0 && signbit(ref_b.min_v) != 0);
        assert(ref_b.max_v == 0.0 && signbit(ref_b.max_v) == 0);
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_b = vx_simd_avx2_minmax_f64(data_b, NULL, &sel_a);
        ASSERT_MEMEQ(ref_b.min_v, avx_b.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_b.max_v, avx_b.max_v, sizeof(double));
#endif

        /* Case C: [-0.0, -5.0] */
        double data_c[4] = { -0.0, -5.0, 0.0, 0.0 };
        vx_agg_minmax_f64_t ref_c = vx_simd_scalar_minmax_f64_ref(data_c, NULL, &sel_a);
        assert(ref_c.min_v == -5.0);
        assert(ref_c.max_v == 0.0 && signbit(ref_c.max_v) != 0); /* -0.0 */
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_c = vx_simd_avx2_minmax_f64(data_c, NULL, &sel_a);
        ASSERT_MEMEQ(ref_c.min_v, avx_c.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_c.max_v, avx_c.max_v, sizeof(double));
#endif

        /* Case D: [+0.0, +5.0] */
        double data_d[4] = { +0.0, +5.0, 0.0, 0.0 };
        vx_agg_minmax_f64_t ref_d = vx_simd_scalar_minmax_f64_ref(data_d, NULL, &sel_a);
        assert(ref_d.min_v == 0.0 && signbit(ref_d.min_v) == 0); /* +0.0 */
        assert(ref_d.max_v == +5.0);
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_d = vx_simd_avx2_minmax_f64(data_d, NULL, &sel_a);
        ASSERT_MEMEQ(ref_d.min_v, avx_d.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_d.max_v, avx_d.max_v, sizeof(double));
#endif

        /* Case E: [-0.0] alone */
        double data_e[4] = { -0.0, 0.0, 0.0, 0.0 };
        vx_sel_t sel_e = { .count = 1, .is_identity = true };
        vx_agg_minmax_f64_t ref_e = vx_simd_scalar_minmax_f64_ref(data_e, NULL, &sel_e);
        assert(ref_e.min_v == 0.0 && signbit(ref_e.min_v) != 0);
        assert(ref_e.max_v == 0.0 && signbit(ref_e.max_v) != 0);
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_e = vx_simd_avx2_minmax_f64(data_e, NULL, &sel_e);
        ASSERT_MEMEQ(ref_e.min_v, avx_e.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_e.max_v, avx_e.max_v, sizeof(double));
#endif
    }

    /* 2. NULL-masked NaN */
    {
        const uint32_t N = 8;
        double col[8] = { 10.0, 20.0, (double)NAN, 40.0, (double)NAN, 60.0, 70.0, 80.0 };
        uint64_t validity = 0xFF; /* all 8 */
        validity &= ~(1ULL << 2); /* slot 2 (NAN) is NULL */
        validity &= ~(1ULL << 4); /* slot 4 (NAN) is NULL */

        vx_sel_t sel = { .count = N, .is_identity = true };
        vx_agg_minmax_f64_t ref = vx_simd_scalar_minmax_f64_ref(col, &validity, &sel);
        ASSERT_EQ(ref.valid_n, 6);
        assert(!isnan(ref.min_v));
        assert(!isnan(ref.max_v));
        assert(ref.min_v == 10.0);
        assert(ref.max_v == 80.0);

#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx = vx_simd_avx2_minmax_f64(col, &validity, &sel);
        ASSERT_EQ(ref.valid_n, avx.valid_n);
        ASSERT_EQ(ref.min_v, avx.min_v);
        ASSERT_EQ(ref.max_v, avx.max_v);
#endif
    }

    /* 3. Valid NaN at lane 0 / lane 3 / tail */
    {
        /* Lane 0 NaN */
        double col0[8] = { (double)NAN, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
        vx_sel_t sel = { .count = 8, .is_identity = true };
        vx_agg_minmax_f64_t ref0 = vx_simd_scalar_minmax_f64_ref(col0, NULL, &sel);
        ASSERT_EQ(ref0.valid_n, 8);
        assert(isnan(ref0.min_v));
        assert(isnan(ref0.max_v));
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx0 = vx_simd_avx2_minmax_f64(col0, NULL, &sel);
        ASSERT_EQ(ref0.valid_n, avx0.valid_n);
        assert(isnan(avx0.min_v));
        assert(isnan(avx0.max_v));
#endif

        /* Lane 3 NaN */
        double col3[8] = { 1.0, 2.0, 3.0, (double)NAN, 5.0, 6.0, 7.0, 8.0 };
        vx_agg_minmax_f64_t ref3 = vx_simd_scalar_minmax_f64_ref(col3, NULL, &sel);
        ASSERT_EQ(ref3.valid_n, 8);
        assert(isnan(ref3.min_v));
        assert(isnan(ref3.max_v));
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx3 = vx_simd_avx2_minmax_f64(col3, NULL, &sel);
        ASSERT_EQ(ref3.valid_n, avx3.valid_n);
        assert(isnan(avx3.min_v));
        assert(isnan(avx3.max_v));
#endif

        /* Tail NaN (length 5: 4 vector lanes + 1 tail) */
        double col_tail[8] = { 1.0, 2.0, 3.0, 4.0, (double)NAN, 0.0, 0.0, 0.0 };
        vx_sel_t sel_tail = { .count = 5, .is_identity = true };
        vx_agg_minmax_f64_t ref_tail = vx_simd_scalar_minmax_f64_ref(col_tail, NULL, &sel_tail);
        ASSERT_EQ(ref_tail.valid_n, 5);
        assert(isnan(ref_tail.min_v));
        assert(isnan(ref_tail.max_v));
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_tail = vx_simd_avx2_minmax_f64(col_tail, NULL, &sel_tail);
        ASSERT_EQ(ref_tail.valid_n, avx_tail.valid_n);
        assert(isnan(avx_tail.min_v));
        assert(isnan(avx_tail.max_v));
#endif
    }

    /* 4. Large non-multiple of 4 vector with dense and sparse selections */
    {
        const uint32_t N = 127;
        double col[128];
        uint64_t validity[2] = { 0, 0 };
        for (uint32_t i = 0; i < N; ++i) {
            col[i] = (double)(i * 7 % 100) - 45.0;
            if (i % 5 != 0) {
                validity[i >> 6] |= (1ULL << (i & 63u));
            }
        }

        vx_sel_t in_dense = { .count = N, .is_identity = true };
        vx_agg_minmax_f64_t ref_d = vx_simd_scalar_minmax_f64_ref(col, validity, &in_dense);
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_d = vx_simd_avx2_minmax_f64(col, validity, &in_dense);
        ASSERT_EQ(ref_d.valid_n, avx_d.valid_n);
        ASSERT_MEMEQ(ref_d.min_v, avx_d.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_d.max_v, avx_d.max_v, sizeof(double));
#endif

        vx_sel_t in_sparse = { .count = 0, .is_identity = false };
        for (uint32_t i = 0; i < N; i += 3) {
            in_sparse.idx[in_sparse.count++] = i;
        }
        vx_agg_minmax_f64_t ref_s = vx_simd_scalar_minmax_f64_ref(col, validity, &in_sparse);
#if defined(__x86_64__) || defined(_M_X64)
        vx_agg_minmax_f64_t avx_s = vx_simd_avx2_minmax_f64(col, validity, &in_sparse);
        ASSERT_EQ(ref_s.valid_n, avx_s.valid_n);
        ASSERT_MEMEQ(ref_s.min_v, avx_s.min_v, sizeof(double));
        ASSERT_MEMEQ(ref_s.max_v, avx_s.max_v, sizeof(double));
#endif
    }

    printf("  [PASS] test_minmax_f64_ieee754\n");
}

int main(void) {
    printf("=== Starting V2 Numeric SIMD Test Suite ===\n");
    test_filter_i64_parity();
    test_filter_f64_ieee754();
    test_sum_f64_bitwise_null_masking();
    test_filter_u64_parity_and_boundaries();
    test_minmax_f64_ieee754();
    printf("=== All V2 Numeric SIMD Tests Passed Successfully! ===\n");
    return 0;
}

