#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "src/vx_types.h"
#include "src/vx_owner.h"
#include "src/vx_arena.h"
#include "src/vx_str.h"
#include "src/vx_chunk.h"
#include "src/vx_batch.h"
#include "src/vx_op.h"
#include "src/vx_sort.h"

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "ASSERT_EQ failed at %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while(0)

#define ASSERT_TRUE(c) do { \
    if (!(c)) { \
        fprintf(stderr, "ASSERT_TRUE failed at %s:%d: %s is false\n", __FILE__, __LINE__, #c); \
        exit(1); \
    } \
} while(0)

/* --- Test 1: TopKSink & TopKScan (ASC and DESC) --- */
static void test_topk_basic_and_limit(vx_arena_t *scratch) {
    printf("Testing TopKSink and TopKScan (ASC and DESC)...\n");

    /* 10 integers in scrambled order */
    int64_t vals[10] = { 45, 12, 89, 3, 27, 64, 12, 99, 5, 33 };
    vx_vec_t bcols[1] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 10, .data = vals }
    };
    vx_batch_t batch = {
        .cols = bcols,
        .ncols = 1,
        .cap_cols = 1,
        .sel = { .count = 10, .is_identity = true }
    };

    vx_type_t col_types[] = { VX_TYPE_I64 };
    uint32_t  col_ids[]   = { 0 };
    vx_sort_key_t key_asc = { .out_col_idx = 0, .type = VX_TYPE_I64, .desc = false, .nulls_first = false };

    vx_sort_cfg_t cfg_asc = {
        .ncols = 1,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = &key_asc,
        .nkeys = 1,
        .offset = 0,
        .limit = 4, /* Top 4 smallest */
        .spill_dir = "/tmp"
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_TOPK, .plan_cfg = (void *)&cfg_asc };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *gstate = NULL, *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    vx_op_res_t res = sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    /* Stream through TopKScan */
    vx_source_t src = { .vtbl = &VX_SOURCE_TOPK_SCAN, .plan_cfg = gstate };
    void *src_g = NULL, *src_l = NULL;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    vx_vec_t out_cols[1];
    vx_batch_t out = { .cols = out_cols, .ncols = 0, .cap_cols = 1 };
    res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, 4);

    const int64_t *d = (const int64_t *)out.cols[0].data;
    ASSERT_EQ(d[0], 3);
    ASSERT_EQ(d[1], 5);
    ASSERT_EQ(d[2], 12);
    ASSERT_EQ(d[3], 12);

    res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_FINISHED);

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);

    /* Test DESC: Top 3 largest */
    vx_sort_key_t key_desc = { .out_col_idx = 0, .type = VX_TYPE_I64, .desc = true, .nulls_first = false };
    vx_sort_cfg_t cfg_desc = {
        .ncols = 1,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = &key_desc,
        .nkeys = 1,
        .offset = 0,
        .limit = 3,
        .spill_dir = "/tmp"
    };

    sink.plan_cfg = (void *)&cfg_desc;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);
    sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    src.plan_cfg = gstate;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, 3);
    d = (const int64_t *)out.cols[0].data;
    ASSERT_EQ(d[0], 99);
    ASSERT_EQ(d[1], 89);
    ASSERT_EQ(d[2], 64);

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);

    printf("  [PASS] test_topk_basic_and_limit\n");
}

/* --- Test 2: TopK Offset and Limit Slicing [offset : offset + limit] --- */
static void test_topk_offset_slice(vx_arena_t *scratch) {
    printf("Testing TopKSink offset slicing [2 : 5]...\n");

    /* 6 values: [10, 20, 30, 40, 50, 60] shuffled */
    int64_t vals[6] = { 60, 20, 10, 50, 30, 40 };
    vx_vec_t bcols[1] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 6, .data = vals }
    };
    vx_batch_t batch = {
        .cols = bcols,
        .ncols = 1,
        .cap_cols = 1,
        .sel = { .count = 6, .is_identity = true }
    };

    vx_type_t col_types[] = { VX_TYPE_I64 };
    uint32_t  col_ids[]   = { 0 };
    vx_sort_key_t key_asc = { .out_col_idx = 0, .type = VX_TYPE_I64, .desc = false, .nulls_first = false };

    /* Sorted: [10, 20, 30, 40, 50, 60]
     * offset = 2, limit = 3 -> should produce [30, 40, 50]
     */
    vx_sort_cfg_t cfg = {
        .ncols = 1,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = &key_asc,
        .nkeys = 1,
        .offset = 2,
        .limit = 3,
        .spill_dir = "/tmp"
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_TOPK, .plan_cfg = (void *)&cfg };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *gstate = NULL, *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    vx_source_t src = { .vtbl = &VX_SOURCE_TOPK_SCAN, .plan_cfg = gstate };
    void *src_g = NULL, *src_l = NULL;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    vx_vec_t out_cols[1];
    vx_batch_t out = { .cols = out_cols, .ncols = 0, .cap_cols = 1 };
    vx_op_res_t res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, 3);

    const int64_t *d = (const int64_t *)out.cols[0].data;
    ASSERT_EQ(d[0], 30);
    ASSERT_EQ(d[1], 40);
    ASSERT_EQ(d[2], 50);

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);

    printf("  [PASS] test_topk_offset_slice\n");
}

/* --- Test 3: IEEE-754 Total Ordering & NULLS FIRST/LAST --- */
static void test_sort_ieee754_total_ordering(vx_arena_t *scratch) {
    printf("Testing IEEE-754 Total Ordering (-Inf < finite < +Inf < NaN) & NULLS FIRST/LAST...\n");

    /* Values: [+0.0, -Inf, NaN, 10.0, -0.0, +Inf, -10.0, NULL] */
    double fvals[8] = { +0.0, -(double)INFINITY, (double)NAN, 10.0, -0.0, (double)INFINITY, -10.0, 0.0 };
    uint64_t val_bm = 0x7F; /* first 7 valid, 8th is NULL */

    vx_vec_t bcols[1] = {
        { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .has_nulls = 1, .phys_len = 8, .data = fvals, .validity = &val_bm }
    };
    vx_batch_t batch = {
        .cols = bcols,
        .ncols = 1,
        .cap_cols = 1,
        .sel = { .count = 8, .is_identity = true }
    };

    vx_type_t col_types[] = { VX_TYPE_F64 };
    uint32_t  col_ids[]   = { 0 };

    /* 1. ASC, nulls_first = false:
     * Expected order:
     * [-Inf, -10.0, (-0.0 == +0.0), (+0.0 == -0.0), 10.0, +Inf, NaN, NULL]
     */
    vx_sort_key_t key_asc_nl = { .out_col_idx = 0, .type = VX_TYPE_F64, .desc = false, .nulls_first = false };
    vx_sort_cfg_t cfg1 = {
        .ncols = 1,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = &key_asc_nl,
        .nkeys = 1,
        .offset = 0,
        .limit = 8,
        .spill_dir = "/tmp"
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_TOPK, .plan_cfg = (void *)&cfg1 };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *gstate = NULL, *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    vx_source_t src = { .vtbl = &VX_SOURCE_TOPK_SCAN, .plan_cfg = gstate };
    void *src_g = NULL, *src_l = NULL;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    vx_vec_t out_cols[1];
    vx_batch_t out = { .cols = out_cols, .ncols = 0, .cap_cols = 1 };
    vx_op_res_t res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, 8);

    const double *fd = (const double *)out.cols[0].data;
    ASSERT_TRUE(isinf(fd[0]) && fd[0] < 0); /* -Inf */
    ASSERT_EQ(fd[1], -10.0);
    ASSERT_TRUE(fd[2] == 0.0);             /* -0.0 or +0.0 */
    ASSERT_TRUE(fd[3] == 0.0);             /* +0.0 or -0.0 */
    ASSERT_EQ(fd[4], 10.0);
    ASSERT_TRUE(isinf(fd[5]) && fd[5] > 0); /* +Inf */
    ASSERT_TRUE(isnan(fd[6]));              /* NaN */
    ASSERT_EQ(out.cols[0].has_nulls, 1);
    ASSERT_TRUE(out.cols[0].validity != NULL);
    bool is_last_null = ((out.cols[0].validity[0] >> 7) & 1ULL) == 0;
    ASSERT_TRUE(is_last_null);              /* NULL at index 7 */

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);

    /* 2. ASC, nulls_first = true: NULL must appear at index 0 */
    vx_sort_key_t key_asc_nf = { .out_col_idx = 0, .type = VX_TYPE_F64, .desc = false, .nulls_first = true };
    cfg1.keys = &key_asc_nf;
    sink.plan_cfg = (void *)&cfg1;

    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);
    sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    src.plan_cfg = gstate;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, 8);

    bool is_first_null = ((out.cols[0].validity[0] >> 0) & 1ULL) == 0;
    ASSERT_TRUE(is_first_null); /* NULL at index 0 */

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);

    printf("  [PASS] test_sort_ieee754_total_ordering\n");
}

/* --- Test 4: ExternalSort In-Memory & Multi-Batch --- */
static void test_external_sort_in_memory(vx_arena_t *scratch) {
    printf("Testing ExternalSortSink and ExternalSortScan (In-Memory)...\n");

    const uint32_t N = 1000;
    int64_t *vals = (int64_t *)malloc(N * sizeof(int64_t));
    for (uint32_t i = 0; i < N; ++i) {
        vals[i] = (int64_t)((N - i) * 3); /* descending: 3000, 2997, ... */
    }

    vx_vec_t bcols[1] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = N, .data = vals }
    };
    vx_batch_t batch = {
        .cols = bcols,
        .ncols = 1,
        .cap_cols = 1,
        .sel = { .count = N, .is_identity = true }
    };

    vx_type_t col_types[] = { VX_TYPE_I64 };
    uint32_t  col_ids[]   = { 0 };
    vx_sort_key_t key_asc = { .out_col_idx = 0, .type = VX_TYPE_I64, .desc = false, .nulls_first = false };

    vx_sort_cfg_t cfg = {
        .ncols = 1,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = &key_asc,
        .nkeys = 1,
        .offset = 0,
        .limit = 0, /* unbounded */
        .spill_dir = "/tmp"
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_EXT_SORT, .plan_cfg = (void *)&cfg };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *gstate = NULL, *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    vx_source_t src = { .vtbl = &VX_SOURCE_EXT_SORT_SCAN, .plan_cfg = gstate };
    void *src_g = NULL, *src_l = NULL;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    vx_vec_t out_cols[1];
    vx_batch_t out = { .cols = out_cols, .ncols = 0, .cap_cols = 1 };
    vx_op_res_t res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, N);

    const int64_t *d = (const int64_t *)out.cols[0].data;
    for (uint32_t i = 0; i < N; ++i) {
        ASSERT_EQ(d[i], (int64_t)((i + 1) * 3));
    }

    res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_FINISHED);

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);
    free(vals);

    printf("  [PASS] test_external_sort_in_memory\n");
}

/* --- Test 5: ExternalSort Spilling to Disk --- */
static void test_external_sort_spilling(vx_arena_t *scratch) {
    printf("Testing ExternalSort with active disk spilling & k-way merge...\n");

    /* Create 30,000 rows with a tiny budget to force multiple spills */
    const uint32_t N = 30000;
    int64_t *vals = (int64_t *)malloc(N * sizeof(int64_t));
    for (uint32_t i = 0; i < N; ++i) {
        vals[i] = (int64_t)(N - i);
    }

    vx_type_t col_types[] = { VX_TYPE_I64 };
    uint32_t  col_ids[]   = { 0 };
    vx_sort_key_t key_asc = { .out_col_idx = 0, .type = VX_TYPE_I64, .desc = false, .nulls_first = false };

    vx_sort_cfg_t cfg = {
        .ncols = 1,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = &key_asc,
        .nkeys = 1,
        .offset = 0,
        .limit = 0,
        .spill_dir = "/tmp"
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_EXT_SORT, .plan_cfg = (void *)&cfg };
    vx_err_t err = {0};
    /* Set work_mem_bytes to minimum allowed (256 KB) so it spills frequently */
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 256 * 1024,
        .err = &err
    };
    void *gstate = NULL, *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    /* Push in batches of 4,000 */
    for (uint32_t i = 0; i < N; i += 4000) {
        uint32_t bsz = (N - i > 4000) ? 4000 : (N - i);
        vx_vec_t bcols[1] = {
            { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = bsz, .data = vals + i }
        };
        vx_batch_t batch = {
            .cols = bcols,
            .ncols = 1,
            .cap_cols = 1,
            .sel = { .count = bsz, .is_identity = true }
        };
        sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    }

    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    /* Scan output and verify strict ascending sequence */
    vx_source_t src = { .vtbl = &VX_SOURCE_EXT_SORT_SCAN, .plan_cfg = gstate };
    void *src_g = NULL, *src_l = NULL;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    vx_vec_t out_cols[1];
    vx_batch_t out = { .cols = out_cols, .ncols = 0, .cap_cols = 1 };

    uint32_t total_emitted = 0;
    int64_t  expected_val  = 1;

    for (;;) {
        vx_arena_reset(scratch);
        vx_op_res_t res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
        if (res == VX_OP_FINISHED) break;
        ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);

        const int64_t *d = (const int64_t *)out.cols[0].data;
        for (uint32_t i = 0; i < out.sel.count; ++i) {
            ASSERT_EQ(d[i], expected_val);
            expected_val++;
        }
        total_emitted += out.sel.count;
        vx_batch_reset(&out);
    }

    ASSERT_EQ(total_emitted, N);

    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);
    free(vals);

    printf("  [PASS] test_external_sort_spilling\n");
}

/* --- Test 6: Multi-Key Sort with Strings and Tie-Breakers --- */
static void test_multikey_strings(vx_arena_t *scratch) {
    printf("Testing Multi-Key Sort (Category STR ASC, Value I64 DESC)...\n");

    vx_strpool_t pool;
    vx_strpool_init(&pool, 4);

    vx_strview_t svs[5];
    svs[0] = vx_strview_inline((const uint8_t *)"fruit", 5);
    svs[1] = vx_strview_inline((const uint8_t *)"dairy", 5);
    svs[2] = vx_strview_inline((const uint8_t *)"fruit", 5);
    svs[3] = vx_strview_inline((const uint8_t *)"bakery", 6);
    const char *long_cat = "vegetables_organic_farm_fresh";
    vx_strpool_append(&pool, (const uint8_t *)long_cat, (int32_t)strlen(long_cat), &svs[4]);

    int64_t prices[5] = { 10, 50, 25, 100, 30 };

    vx_vec_t bcols[2] = {
        { .type = VX_TYPE_STR, .kind = VX_VEC_FLAT, .phys_len = 5, .data = svs, .strpool = &pool },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 5, .data = prices }
    };
    vx_batch_t batch = {
        .cols = bcols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 5, .is_identity = true }
    };

    vx_type_t col_types[] = { VX_TYPE_STR, VX_TYPE_I64 };
    uint32_t  col_ids[]   = { 0, 1 };

    /* Key 0: Category STR ASC
     * Key 1: Price I64 DESC
     */
    vx_sort_key_t keys[2] = {
        { .out_col_idx = 0, .type = VX_TYPE_STR, .desc = false, .nulls_first = false },
        { .out_col_idx = 1, .type = VX_TYPE_I64, .desc = true,  .nulls_first = false }
    };

    vx_sort_cfg_t cfg = {
        .ncols = 2,
        .col_types = col_types,
        .batch_col_ids = col_ids,
        .table_col_ids = col_ids,
        .keys = keys,
        .nkeys = 2,
        .offset = 0,
        .limit = 5,
        .spill_dir = "/tmp"
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_TOPK, .plan_cfg = (void *)&cfg };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *gstate = NULL, *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    sink.vtbl->finalize(&sink, &ctx, gstate);
    sink.vtbl->destroy_local(lstate);

    vx_source_t src = { .vtbl = &VX_SOURCE_TOPK_SCAN, .plan_cfg = gstate };
    void *src_g = NULL, *src_l = NULL;
    src.vtbl->init_global(&src, &ctx, &src_g);
    src.vtbl->init_local(&src, &ctx, src_g, &src_l);

    vx_vec_t out_cols[2];
    vx_batch_t out = { .cols = out_cols, .ncols = 0, .cap_cols = 2 };
    vx_op_res_t res = src.vtbl->next_batch(&src, &ctx, src_g, src_l, &out);
    ASSERT_EQ(res, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out.sel.count, 5);

    /* Expected order:
     * 1. bakery (100)
     * 2. dairy (50)
     * 3. fruit (25)  <-- tie broken by price DESC (25 > 10)
     * 4. fruit (10)
     * 5. vegetables... (30)
     */
    const int64_t *out_prices = (const int64_t *)out.cols[1].data;
    ASSERT_EQ(out_prices[0], 100);
    ASSERT_EQ(out_prices[1], 50);
    ASSERT_EQ(out_prices[2], 25);
    ASSERT_EQ(out_prices[3], 10);
    ASSERT_EQ(out_prices[4], 30);

    vx_batch_reset(&out);
    src.vtbl->destroy_local(src_l);
    src.vtbl->destroy_global(src_g);
    sink.vtbl->destroy_global(gstate);
    vx_strpool_destroy(&pool);

    printf("  [PASS] test_multikey_strings\n");
}

int main(void) {
    printf("=== Starting V2 Sort & Top-K Test Suite ===\n");
    vx_arena_t *scratch = vx_arena_create(64 * 1024, 1024 * 1024, 0);
    assert(scratch != NULL);

    test_topk_basic_and_limit(scratch);
    test_topk_offset_slice(scratch);
    test_sort_ieee754_total_ordering(scratch);
    test_external_sort_in_memory(scratch);
    test_external_sort_spilling(scratch);
    test_multikey_strings(scratch);

    vx_owner_release(&scratch->hdr);
    printf("=== All V2 Sort & Top-K Tests Passed Successfully! ===\n");
    return 0;
}

