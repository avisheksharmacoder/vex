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
#include "src/vx_simd.h"
#include "src/vx_sink_hash_agg.h"

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "ASSERT_EQ failed at %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while(0)

/* Use real vx_arena_t from vx_arena_v2.c */

/* --- Builtin Tests --- */
static void test_builtin_len(vx_arena_t *arena) {
    printf("Testing builtin len()...\n");
    vx_strpool_t pool;
    vx_strpool_init(&pool, 4);

    vx_strview_t sv[5];
    /* sv[0]: empty "" */
    memset(&sv[0], 0, sizeof(sv[0]));
    /* sv[1]: "hello" (5 bytes, 5 codepoints) */
    sv[1] = vx_strview_inline((const uint8_t *)"hello", 5);
    /* sv[2]: "héllo" (6 bytes, 5 codepoints) */
    sv[2] = vx_strview_inline((const uint8_t *)"h\xc3\xa9llo", 6);
    /* sv[3]: "🚀" (4 bytes, 1 codepoint) */
    sv[3] = vx_strview_inline((const uint8_t *)"\xf0\x9f\x9a\x80", 4);
    /* sv[4]: "long_string_with_many_chars_over_12_bytes" */
    const char *long_s = "long_string_with_many_chars_over_12_bytes";
    vx_strpool_append(&pool, (const uint8_t *)long_s, (int32_t)strlen(long_s), &sv[4]);

    uint64_t val = 0x1F; /* all 5 valid */
    vx_vec_t in_vec = {
        .type = VX_TYPE_STR,
        .kind = VX_VEC_FLAT,
        .has_nulls = 0,
        .phys_len = 5,
        .data = sv,
        .validity = &val,
        .strpool = &pool
    };

    vx_sel_t sel = { .count = 5, .is_identity = true };
    vx_vec_t out_vec;
    vx_status_t st = vx_builtin_eval(VX_FN_LEN, &in_vec, 1, &sel, arena, &out_vec);
    ASSERT_EQ(st, VX_OK);
    ASSERT_EQ(out_vec.type, VX_TYPE_I64);

    const int64_t *lens = (const int64_t *)out_vec.data;
    ASSERT_EQ(lens[0], 0);
    ASSERT_EQ(lens[1], 5);
    ASSERT_EQ(lens[2], 5);
    ASSERT_EQ(lens[3], 1);
    ASSERT_EQ(lens[4], (int64_t)strlen(long_s));

    vx_owner_release(out_vec.owner);
    vx_strpool_destroy(&pool);
    printf("  [PASS] test_builtin_len\n");
}

static void test_builtin_lower_upper(vx_arena_t *arena) {
    printf("Testing builtin lower() and upper()...\n");
    vx_strpool_t pool;
    vx_strpool_init(&pool, 4);

    vx_strview_t sv[3];
    /* Inline <= 12 bytes */
    sv[0] = vx_strview_inline((const uint8_t *)"HeLLo World", 11);
    /* Out-of-line > 12 bytes */
    const char *s1 = "ThIs Is A MuCh LoNgEr StRiNg WiTh 12345 Numbers!";
    vx_strpool_append(&pool, (const uint8_t *)s1, (int32_t)strlen(s1), &sv[1]);
    /* UTF-8 accented string */
    sv[2] = vx_strview_inline((const uint8_t *)"CaF\xc3\xa9", 5);

    vx_vec_t in_vec = {
        .type = VX_TYPE_STR,
        .kind = VX_VEC_FLAT,
        .has_nulls = 0,
        .phys_len = 3,
        .data = sv,
        .validity = NULL,
        .strpool = &pool
    };

    vx_sel_t sel = { .count = 3, .is_identity = true };
    vx_vec_t out_lower, out_upper;

    vx_status_t st = vx_builtin_eval(VX_FN_LOWER, &in_vec, 1, &sel, arena, &out_lower);
    ASSERT_EQ(st, VX_OK);
    const vx_strview_t *lsv = (const vx_strview_t *)out_lower.data;
    const uint8_t *p0 = vx_strview_ptr(&lsv[0], out_lower.strpool);
    assert(memcmp(p0, "hello world", 11) == 0);
    const uint8_t *p1 = vx_strview_ptr(&lsv[1], out_lower.strpool);
    assert(memcmp(p1, "this is a much longer string with 12345 numbers!", strlen(s1)) == 0);

    st = vx_builtin_eval(VX_FN_UPPER, &in_vec, 1, &sel, arena, &out_upper);
    ASSERT_EQ(st, VX_OK);
    const vx_strview_t *usv = (const vx_strview_t *)out_upper.data;
    const uint8_t *up0 = vx_strview_ptr(&usv[0], out_upper.strpool);
    assert(memcmp(up0, "HELLO WORLD", 11) == 0);

    vx_owner_release(out_lower.owner);
    vx_owner_release(out_upper.owner);
    vx_strpool_destroy(&pool);
    printf("  [PASS] test_builtin_lower_upper\n");
}

static void test_builtin_startswith_contains(vx_arena_t *arena) {
    printf("Testing builtin startswith() and contains()...\n");
    vx_strpool_t pool;
    vx_strpool_init(&pool, 4);

    vx_strview_t hays[4], ndls[4];
    hays[0] = vx_strview_inline((const uint8_t *)"abcdefgh", 8);
    ndls[0] = vx_strview_inline((const uint8_t *)"abc", 3);

    hays[1] = vx_strview_inline((const uint8_t *)"abcdefgh", 8);
    ndls[1] = vx_strview_inline((const uint8_t *)"cde", 3);

    const char *long_hay = "prefix_long_haystack_string_test_middle_end";
    vx_strpool_append(&pool, (const uint8_t *)long_hay, (int32_t)strlen(long_hay), &hays[2]);
    ndls[2] = vx_strview_inline((const uint8_t *)"prefix_", 7);

    hays[3] = hays[2];
    ndls[3] = vx_strview_inline((const uint8_t *)"nomatch", 7);

    vx_vec_t hay_vec = {
        .type = VX_TYPE_STR, .kind = VX_VEC_FLAT, .phys_len = 4, .data = hays, .strpool = &pool
    };
    vx_vec_t ndl_vec = {
        .type = VX_TYPE_STR, .kind = VX_VEC_FLAT, .phys_len = 4, .data = ndls, .strpool = &pool
    };

    vx_vec_t args[2] = { hay_vec, ndl_vec };
    vx_sel_t sel = { .count = 4, .is_identity = true };

    /* 1. startswith */
    vx_vec_t out_sw;
    vx_status_t st = vx_builtin_eval(VX_FN_STARTSWITH, args, 2, &sel, arena, &out_sw);
    ASSERT_EQ(st, VX_OK);
    const uint8_t *sw_res = (const uint8_t *)out_sw.data;
    ASSERT_EQ(sw_res[0], 1); /* "abcdefgh" starts with "abc" */
    ASSERT_EQ(sw_res[1], 0); /* "abcdefgh" does NOT start with "cde" */
    ASSERT_EQ(sw_res[2], 1); /* long_hay starts with "prefix_" */
    ASSERT_EQ(sw_res[3], 0); /* long_hay does NOT start with "nomatch" */

    /* 2. contains */
    vx_vec_t out_ct;
    st = vx_builtin_eval(VX_FN_CONTAINS, args, 2, &sel, arena, &out_ct);
    ASSERT_EQ(st, VX_OK);
    const uint8_t *ct_res = (const uint8_t *)out_ct.data;
    ASSERT_EQ(ct_res[0], 1); /* "abcdefgh" contains "abc" */
    ASSERT_EQ(ct_res[1], 1); /* "abcdefgh" contains "cde" */
    ASSERT_EQ(ct_res[2], 1); /* long_hay contains "prefix_" */
    ASSERT_EQ(ct_res[3], 0); /* long_hay does NOT contain "nomatch" */

    vx_owner_release(out_sw.owner);
    vx_owner_release(out_ct.owner);
    vx_strpool_destroy(&pool);
    printf("  [PASS] test_builtin_startswith_contains\n");
}

static void test_builtin_coalesce_abs_round(vx_arena_t *arena) {
    printf("Testing builtin coalesce(), abs(), and round()...\n");
    /* 1. Coalesce */
    int64_t a1[3] = { 0, 10, 0 };
    uint64_t val1 = 0x2; /* row 1 is valid */
    int64_t a2[3] = { 100, 200, 0 };
    uint64_t val2 = 0x3; /* rows 0, 1 valid */
    int64_t a3[3] = { 1000, 2000, 3000 };
    uint64_t val3 = 0x7; /* all valid */

    vx_vec_t args[3] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .has_nulls = 1, .phys_len = 3, .data = a1, .validity = &val1 },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .has_nulls = 1, .phys_len = 3, .data = a2, .validity = &val2 },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .has_nulls = 0, .phys_len = 3, .data = a3, .validity = &val3 }
    };
    vx_sel_t sel = { .count = 3, .is_identity = true };
    vx_vec_t out_coal;
    vx_status_t st = vx_builtin_eval(VX_FN_COALESCE, args, 3, &sel, arena, &out_coal);
    ASSERT_EQ(st, VX_OK);
    const int64_t *cres = (const int64_t *)out_coal.data;
    ASSERT_EQ(cres[0], 100);  /* a1 is null, a2 is 100 */
    ASSERT_EQ(cres[1], 10);   /* a1 is 10 */
    ASSERT_EQ(cres[2], 3000); /* a1, a2 null, a3 is 3000 */
    vx_owner_release(out_coal.owner);

    /* 2. Abs on i64 (including INT64_MIN) */
    int64_t i_vals[4] = { 5, -5, 0, INT64_MIN };
    vx_vec_t abs_in = {
        .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = i_vals, .validity = NULL
    };
    vx_sel_t abs_sel = { .count = 4, .is_identity = true };
    vx_vec_t out_abs;
    st = vx_builtin_eval(VX_FN_ABS, &abs_in, 1, &abs_sel, arena, &out_abs);
    ASSERT_EQ(st, VX_OK);
    const int64_t *ares = (const int64_t *)out_abs.data;
    ASSERT_EQ(ares[0], 5);
    ASSERT_EQ(ares[1], 5);
    ASSERT_EQ(ares[2], 0);
    ASSERT_EQ(ares[3], (int64_t)(0ULL - (uint64_t)INT64_MIN));
    vx_owner_release(out_abs.owner);

    /* 3. Round on f64 */
    double f_vals[4] = { 3.14159, 2.71828, -1.5, 1234.5678 };
    int64_t digits[4] = { 2, 0, 0, -2 };
    vx_vec_t r_in0 = { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = f_vals };
    vx_vec_t r_in1 = { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = digits };
    vx_vec_t r_args[2] = { r_in0, r_in1 };
    vx_vec_t out_round;
    st = vx_builtin_eval(VX_FN_ROUND, r_args, 2, &abs_sel, arena, &out_round);
    ASSERT_EQ(st, VX_OK);
    const double *rres = (const double *)out_round.data;
    ASSERT_EQ(rres[0], 3.14);
    ASSERT_EQ(rres[1], 3.0);
    ASSERT_EQ(rres[2], -2.0);
    ASSERT_EQ(rres[3], 1200.0);
    vx_owner_release(out_round.owner);

    printf("  [PASS] test_builtin_coalesce_abs_round\n");
}

/* --- HashAggSink Tests --- */
static void test_hash_agg_ungrouped_empty(vx_arena_t *scratch) {
    printf("Testing HashAggSink ungrouped on empty table...\n");
    vx_agg_spec_t aggs[3] = {
        { .kind = VX_AGG_COUNT_STAR, .type = VX_TYPE_I64, .batch_col_idx = 0 },
        { .kind = VX_AGG_COUNT,      .type = VX_TYPE_I64, .batch_col_idx = 0 },
        { .kind = VX_AGG_SUM,        .type = VX_TYPE_I64, .batch_col_idx = 0 }
    };
    vx_hash_agg_cfg_t cfg = {
        .keys = NULL, .nkeys = 0,
        .aggs = aggs, .naggs = 3
    };
    vx_sink_t sink = { .vtbl = &VX_SINK_HASH_AGG, .plan_cfg = &cfg };

    vx_exec_ctx_t ctx = {
        .worker_id = 0, .nworkers = 1, .scratch = scratch
    };

    void *gstate = NULL;
    void *lstate = NULL;
    vx_status_t st = sink.vtbl->init_global(&sink, &ctx, &gstate);
    ASSERT_EQ(st, VX_OK);
    st = sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);
    ASSERT_EQ(st, VX_OK);

    /* Combine without consuming any rows */
    st = sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    ASSERT_EQ(st, VX_OK);
    st = sink.vtbl->finalize(&sink, &ctx, gstate);
    ASSERT_EQ(st, VX_OK);

    /* Verify 1 group created with count = 0 */
    vx_agg_gstate_t *g = (vx_agg_gstate_t *)gstate;
    ASSERT_EQ(g->parts[0].ht.ngroups, 1);
    ASSERT_EQ(g->parts[0].ht.aggs[0].count, 0); /* count_star */
    ASSERT_EQ(g->parts[0].ht.aggs[1].count, 0); /* count(col) */
    ASSERT_EQ(g->parts[0].ht.aggs[2].count, 0); /* sum */

    sink.vtbl->destroy_local(lstate);
    sink.vtbl->destroy_global(gstate);
    printf("  [PASS] test_hash_agg_ungrouped_empty\n");
}

static void test_hash_agg_grouped(vx_arena_t *scratch) {
    printf("Testing HashAggSink grouped with multiple keys & Neumaier f64...\n");
    /* Key: col 0 = i64 (dept_id), col 1 = f64 (score: contains -0.0 & +0.0, and NaN)
     * Agg: col 2 = f64 (salary: sum), col 3 = i64 (bonus: min/max) */
    vx_key_spec_t keys[2] = {
        { .type = VX_TYPE_I64, .batch_col_idx = 0 },
        { .type = VX_TYPE_F64, .batch_col_idx = 1 }
    };
    vx_agg_spec_t aggs[3] = {
        { .kind = VX_AGG_COUNT_STAR, .type = VX_TYPE_I64, .batch_col_idx = 0 },
        { .kind = VX_AGG_SUM,        .type = VX_TYPE_F64, .batch_col_idx = 2 },
        { .kind = VX_AGG_MAX,        .type = VX_TYPE_I64, .batch_col_idx = 3 }
    };
    vx_hash_agg_cfg_t cfg = {
        .keys = keys, .nkeys = 2,
        .aggs = aggs, .naggs = 3
    };
    vx_sink_t sink = { .vtbl = &VX_SINK_HASH_AGG, .plan_cfg = &cfg };

    vx_exec_ctx_t ctx = {
        .worker_id = 0, .nworkers = 1, .scratch = scratch
    };

    void *gstate = NULL;
    void *lstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &gstate);
    sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);

    /* Construct batch:
     * row 0: dept=1, score=+0.0, sal=100.0, bonus=10
     * row 1: dept=1, score=-0.0, sal=200.0, bonus=20   <-- Must group with row 0!
     * row 2: dept=2, score=NAN,  sal=1e16,  bonus=5
     * row 3: dept=2, score=NAN,  sal=1.0,   bonus=15   <-- Must group with row 2!
     * row 4: dept=2, score=NAN,  sal=-1e16, bonus=25   <-- Neumaier sum test!
     */
    int64_t dept_vals[5]  = { 1, 1, 2, 2, 2 };
    double  score_vals[5] = { +0.0, -0.0, (double)NAN, (double)NAN, (double)NAN };
    double  sal_vals[5]   = { 100.0, 200.0, 1e16, 1.0, -1e16 };
    int64_t bonus_vals[5] = { 10, 20, 5, 15, 25 };

    vx_vec_t bcols[4] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 5, .data = dept_vals },
        { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .phys_len = 5, .data = score_vals },
        { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .phys_len = 5, .data = sal_vals },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 5, .data = bonus_vals }
    };
    vx_batch_t batch = {
        .cols = bcols,
        .ncols = 4,
        .sel = { .count = 5, .is_identity = true }
    };

    vx_op_res_t op_res = sink.vtbl->sink(&sink, &ctx, gstate, lstate, &batch);
    ASSERT_EQ(op_res, VX_OP_NEED_MORE_INPUT);

    vx_status_t st = sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    ASSERT_EQ(st, VX_OK);
    st = sink.vtbl->finalize(&sink, &ctx, gstate);
    ASSERT_EQ(st, VX_OK);

    /* Sum total groups across all 64 partitions: should be exactly 2 groups! */
    vx_agg_gstate_t *g = (vx_agg_gstate_t *)gstate;
    uint32_t total_groups = 0;
    for (uint32_t p = 0; p < VX_AGG_PARTITIONS; ++p) {
        total_groups += g->parts[p].ht.ngroups;
        for (uint32_t gid = 0; gid < g->parts[p].ht.ngroups; ++gid) {
            vx_key_val_t   *k = &g->parts[p].ht.keys[(size_t)gid * 2];
            vx_agg_state_t *a = &g->parts[p].ht.aggs[(size_t)gid * 3];
            if (k[0].v.i64 == 1) {
                /* Group 1 (dept=1): count=2, sal_sum=300.0, max_bonus=20 */
                ASSERT_EQ(a[0].count, 2);
                double s = a[1].v.f64_sum.sum + a[1].v.f64_sum.comp;
                ASSERT_EQ(s, 300.0);
                ASSERT_EQ(a[2].v.i64, 20);
            } else if (k[0].v.i64 == 2) {
                /* Group 2 (dept=2): count=3, sal_sum=1.0 (Neumaier accuracy!), max_bonus=25 */
                ASSERT_EQ(a[0].count, 3);
                double s = a[1].v.f64_sum.sum + a[1].v.f64_sum.comp;
                ASSERT_EQ(s, 1.0);
                ASSERT_EQ(a[2].v.i64, 25);
            }
        }
    }
    ASSERT_EQ(total_groups, 2);

    sink.vtbl->destroy_local(lstate);
    sink.vtbl->destroy_global(gstate);
    printf("  [PASS] test_hash_agg_grouped\n");
}

int main(void) {
    printf("=== Starting V2 HashAgg & Builtins Test Suite ===\n");
    vx_arena_t *scratch = vx_arena_create(64 * 1024, 1024 * 1024, 0);
    assert(scratch != NULL);

    test_builtin_len(scratch);
    test_builtin_lower_upper(scratch);
    test_builtin_startswith_contains(scratch);
    test_builtin_coalesce_abs_round(scratch);
    test_hash_agg_ungrouped_empty(scratch);
    test_hash_agg_grouped(scratch);

    vx_owner_release(&scratch->hdr);
    printf("=== All V2 HashAgg & Builtins Tests Passed Successfully! ===\n");
    return 0;
}

