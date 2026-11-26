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
#include "src/vx_hash_join.h"

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

/* Helper: Build a hash table from a single batch */
static vx_hj_table_t *build_single_batch(const vx_hj_build_cfg_t *cfg, vx_batch_t *batch, vx_arena_t *scratch) {
    vx_sink_t sink = {
        .vtbl = &VX_SINK_HASH_JOIN_BUILD,
        .plan_cfg = (void *)cfg
    };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *gstate = NULL;
    vx_status_t st = sink.vtbl->init_global(&sink, &ctx, &gstate);
    ASSERT_EQ(st, VX_OK);

    void *lstate = NULL;
    st = sink.vtbl->init_local(&sink, &ctx, gstate, &lstate);
    ASSERT_EQ(st, VX_OK);

    vx_op_res_t res = sink.vtbl->sink(&sink, &ctx, gstate, lstate, batch);
    ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);

    st = sink.vtbl->combine(&sink, &ctx, gstate, lstate);
    ASSERT_EQ(st, VX_OK);

    st = sink.vtbl->finalize(&sink, &ctx, gstate);
    ASSERT_EQ(st, VX_OK);

    sink.vtbl->destroy_local(lstate);
    vx_hj_table_t *ht = vx_hj_build_take(gstate);
    ASSERT_TRUE(ht != NULL);
    return ht;
}

/* --- Test 1: Empty Build Table --- */
static void test_empty_build_table(vx_arena_t *scratch) {
    printf("Testing empty build table (INNER, LEFT, SEMI, ANTI)...\n");

    vx_join_key_t key = { .type = VX_TYPE_I64, .batch_col_idx = 0, .table_col_idx = 0 };
    uint32_t pay_cols[] = { 1 };
    uint32_t pay_tbls[] = { 1 };
    vx_type_t pay_types[] = { VX_TYPE_STR };

    vx_hj_build_cfg_t bcfg = {
        .keys = &key,
        .nkeys = 1,
        .payload_batch_cols = pay_cols,
        .payload_table_cols = pay_tbls,
        .payload_types = pay_types,
        .n_payload_cols = 1
    };

    vx_vec_t eb_cols[2];
    vx_batch_t empty_batch = {
        .cols = eb_cols,
        .ncols = 0,
        .cap_cols = 2,
        .sel = { .count = 0, .is_identity = true }
    };

    vx_hj_table_t *ht = build_single_batch(&bcfg, &empty_batch, scratch);
    ASSERT_EQ(ht->nrows, 0);

    /* Create probe batch with 3 rows */
    int64_t probe_ids[3] = { 10, 20, 30 };
    vx_vec_t pb_cols[2] = {
        {
            .type = VX_TYPE_I64,
            .kind = VX_VEC_FLAT,
            .has_nulls = 0,
            .phys_len = 3,
            .data = probe_ids,
            .validity = NULL
        }
    };
    vx_batch_t probe_batch = {
        .cols = pb_cols,
        .ncols = 1,
        .cap_cols = 2,
        .sel = { .count = 3, .is_identity = true }
    };

    vx_hj_out_col_t out_cols[] = {
        { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 },
        { .from_build = true,  .src_col_idx = 0, .table_col_idx = 1 }
    };

    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };

    /* 1. INNER join -> 0 rows */
    {
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_INNER,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = out_cols,
            .n_out_cols = 2
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &probe_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 0);

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    /* 2. SEMI join -> 0 rows */
    {
        vx_hj_out_col_t semi_out = { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 };
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_SEMI,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = &semi_out,
            .n_out_cols = 1
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &probe_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 0);

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    /* 3. ANTI join -> all 3 probe rows */
    {
        vx_hj_out_col_t anti_out = { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 };
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_ANTI,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = &anti_out,
            .n_out_cols = 1
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &probe_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 3);
        const int64_t *d = (const int64_t *)out.cols[0].data;
        ASSERT_EQ(d[0], 10);
        ASSERT_EQ(d[1], 20);
        ASSERT_EQ(d[2], 30);

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    /* 4. LEFT join -> 3 rows, build column is NULL */
    {
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_LEFT,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = out_cols,
            .n_out_cols = 2
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &probe_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 3);
        ASSERT_EQ(out.cols[1].has_nulls, 1);
        ASSERT_TRUE(out.cols[1].validity != NULL);
        ASSERT_EQ(out.cols[1].validity[0], 0); /* all NULL */

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    vx_owner_release(&ht->hdr);
    printf("  [PASS] test_empty_build_table\n");
}

/* --- Test 2: Standard Equi-Join (INNER & LEFT) with 1:1 and 1:N matches --- */
static void test_inner_and_left_equi_join(vx_arena_t *scratch) {
    printf("Testing INNER & LEFT equi-join with 1:1 and 1:N matches...\n");

    /* Build table:
     * id (i64): [1, 2, 2, 3]
     * val (f64): [10.5, 20.0, 20.5, 30.0]
     */
    int64_t b_ids[4] = { 1, 2, 2, 3 };
    double  b_vals[4] = { 10.5, 20.0, 20.5, 30.0 };

    vx_vec_t b_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = b_ids },
        { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = b_vals }
    };
    vx_batch_t b_batch = {
        .cols = b_cols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 4, .is_identity = true }
    };

    vx_join_key_t key = { .type = VX_TYPE_I64, .batch_col_idx = 0, .table_col_idx = 0 };
    uint32_t pay_cols[] = { 1 };
    uint32_t pay_tbls[] = { 1 };
    vx_type_t pay_types[] = { VX_TYPE_F64 };

    vx_hj_build_cfg_t bcfg = {
        .keys = &key,
        .nkeys = 1,
        .payload_batch_cols = pay_cols,
        .payload_table_cols = pay_tbls,
        .payload_types = pay_types,
        .n_payload_cols = 1
    };

    vx_hj_table_t *ht = build_single_batch(&bcfg, &b_batch, scratch);
    ASSERT_EQ(ht->nrows, 4);

    /* Probe table:
     * p_id (i64): [2, 3, 4]  (2 matches 2 rows, 3 matches 1 row, 4 matches 0 rows)
     */
    int64_t p_ids[3] = { 2, 3, 4 };
    vx_vec_t p_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 3, .data = p_ids }
    };
    vx_batch_t p_batch = {
        .cols = p_cols,
        .ncols = 1,
        .cap_cols = 2,
        .sel = { .count = 3, .is_identity = true }
    };

    vx_hj_out_col_t out_cols[] = {
        { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 },
        { .from_build = true,  .src_col_idx = 0, .table_col_idx = 1 }
    };

    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };

    /* 1. INNER JOIN: Total = 3 rows */
    {
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_INNER,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = out_cols,
            .n_out_cols = 2
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &p_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 3);

        const int64_t *out_pids = (const int64_t *)out.cols[0].data;
        const double  *out_vals = (const double *)out.cols[1].data;

        int count_2 = 0, count_3 = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            if (out_pids[i] == 2) {
                count_2++;
                ASSERT_TRUE(out_vals[i] == 20.0 || out_vals[i] == 20.5);
            } else if (out_pids[i] == 3) {
                count_3++;
                ASSERT_TRUE(out_vals[i] == 30.0);
            }
        }
        ASSERT_EQ(count_2, 2);
        ASSERT_EQ(count_3, 1);

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    /* 2. LEFT JOIN: Total = 4 rows */
    {
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_LEFT,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = out_cols,
            .n_out_cols = 2
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &p_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 4);

        const int64_t *out_pids = (const int64_t *)out.cols[0].data;
        ASSERT_EQ(out.cols[1].has_nulls, 1);
        ASSERT_TRUE(out.cols[1].validity != NULL);

        bool found_4_null = false;
        for (uint32_t i = 0; i < 4; ++i) {
            if (out_pids[i] == 4) {
                bool is_valid = ((out.cols[1].validity[i >> 6] >> (i & 63u)) & 1ULL) != 0;
                ASSERT_TRUE(!is_valid);
                found_4_null = true;
            } else {
                bool is_valid = ((out.cols[1].validity[i >> 6] >> (i & 63u)) & 1ULL) != 0;
                ASSERT_TRUE(is_valid);
            }
        }
        ASSERT_TRUE(found_4_null);

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    vx_owner_release(&ht->hdr);
    printf("  [PASS] test_inner_and_left_equi_join\n");
}

/* --- Test 3: Resumable 1:N Expansion Across VX_BATCH_CAP --- */
static void test_resumable_1_to_n_expansion(vx_arena_t *scratch) {
    printf("Testing resumable 1:N expansion (> 4,096 rows)...\n");

    const uint32_t N_BUILD = 5000;
    int64_t *b_ids = (int64_t *)malloc(N_BUILD * sizeof(int64_t));
    int64_t *b_vals = (int64_t *)malloc(N_BUILD * sizeof(int64_t));
    for (uint32_t i = 0; i < N_BUILD; ++i) {
        b_ids[i] = 42;
        b_vals[i] = (int64_t)i;
    }

    vx_join_key_t key = { .type = VX_TYPE_I64, .batch_col_idx = 0, .table_col_idx = 0 };
    uint32_t pay_cols[] = { 1 };
    uint32_t pay_tbls[] = { 1 };
    vx_type_t pay_types[] = { VX_TYPE_I64 };

    vx_hj_build_cfg_t bcfg = {
        .keys = &key,
        .nkeys = 1,
        .payload_batch_cols = pay_cols,
        .payload_table_cols = pay_tbls,
        .payload_types = pay_types,
        .n_payload_cols = 1
    };

    vx_sink_t sink = { .vtbl = &VX_SINK_HASH_JOIN_BUILD, .plan_cfg = &bcfg };
    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    void *bgstate = NULL, *blstate = NULL;
    sink.vtbl->init_global(&sink, &ctx, &bgstate);
    sink.vtbl->init_local(&sink, &ctx, bgstate, &blstate);

    /* Batch 1: 4000 rows */
    vx_vec_t b1_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4000, .data = b_ids },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4000, .data = b_vals }
    };
    vx_batch_t b1 = {
        .cols = b1_cols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 4000, .is_identity = true }
    };
    sink.vtbl->sink(&sink, &ctx, bgstate, blstate, &b1);

    /* Batch 2: 1000 rows */
    vx_vec_t b2_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 1000, .data = b_ids + 4000 },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 1000, .data = b_vals + 4000 }
    };
    vx_batch_t b2 = {
        .cols = b2_cols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 1000, .is_identity = true }
    };
    sink.vtbl->sink(&sink, &ctx, bgstate, blstate, &b2);

    sink.vtbl->combine(&sink, &ctx, bgstate, blstate);
    sink.vtbl->finalize(&sink, &ctx, bgstate);
    sink.vtbl->destroy_local(blstate);

    vx_hj_table_t *ht = vx_hj_build_take(bgstate);
    ASSERT_EQ(ht->nrows, 5000);

    /* Single probe row with id=42 */
    int64_t p_id = 42;
    vx_vec_t p_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 1, .data = &p_id }
    };
    vx_batch_t p_batch = {
        .cols = p_cols,
        .ncols = 1,
        .cap_cols = 2,
        .sel = { .count = 1, .is_identity = true }
    };

    vx_hj_out_col_t out_cols[] = {
        { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 },
        { .from_build = true,  .src_col_idx = 0, .table_col_idx = 1 }
    };
    vx_hj_probe_cfg_t pcfg = {
        .join_type = VX_JOIN_INNER,
        .ht = ht,
        .probe_keys = &key,
        .nkeys = 1,
        .out_cols = out_cols,
        .n_out_cols = 2
    };

    vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
    void *pgstate = NULL, *plstate = NULL;
    op.vtbl->init_global(&op, &ctx, &pgstate);
    op.vtbl->init_local(&op, &ctx, pgstate, &plstate);

    /* First execute invocation: VX_BATCH_CAP (4096) rows and VX_OP_HAVE_MORE_OUTPUT */
    vx_vec_t out1_cols_buf[4];
    vx_batch_t out1 = { .cols = out1_cols_buf, .ncols = 0, .cap_cols = 4 };
    vx_op_res_t res1 = op.vtbl->execute(&op, &ctx, pgstate, plstate, &p_batch, &out1);
    ASSERT_EQ(res1, VX_OP_HAVE_MORE_OUTPUT);
    ASSERT_EQ(out1.sel.count, VX_BATCH_CAP);

    /* Second execute invocation: remaining 904 rows and VX_OP_NEED_MORE_INPUT */
    vx_vec_t out2_cols_buf[4];
    vx_batch_t out2 = { .cols = out2_cols_buf, .ncols = 0, .cap_cols = 4 };
    vx_op_res_t res2 = op.vtbl->execute(&op, &ctx, pgstate, plstate, &p_batch, &out2);
    ASSERT_EQ(res2, VX_OP_NEED_MORE_INPUT);
    ASSERT_EQ(out2.sel.count, 5000 - VX_BATCH_CAP);

    vx_batch_reset(&out1);
    vx_batch_reset(&out2);
    op.vtbl->destroy_local(plstate);
    op.vtbl->destroy_global(pgstate);

    vx_owner_release(&ht->hdr);
    free(b_ids);
    free(b_vals);
    printf("  [PASS] test_resumable_1_to_n_expansion\n");
}

/* --- Test 4: IEEE-754 (-0.0 vs +0.0, NaN) and NULL Keys --- */
static void test_ieee754_and_null_keys(vx_arena_t *scratch) {
    printf("Testing IEEE-754 (-0.0 vs +0.0, NaN) and NULL join keys...\n");

    double b_keys[5] = { -0.0, +0.0, 1.0, (double)NAN, 2.0 };
    uint64_t b_val = 0x0F; /* rows 0..3 valid, row 4 is NULL */
    int64_t  b_pay[5] = { 100, 200, 300, 400, 500 };

    vx_vec_t b_cols[2] = {
        { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .has_nulls = 1, .phys_len = 5, .data = b_keys, .validity = &b_val },
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 5, .data = b_pay }
    };
    vx_batch_t b_batch = {
        .cols = b_cols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 5, .is_identity = true }
    };

    vx_join_key_t key = { .type = VX_TYPE_F64, .batch_col_idx = 0, .table_col_idx = 0 };
    uint32_t pay_cols[] = { 1 };
    uint32_t pay_tbls[] = { 1 };
    vx_type_t pay_types[] = { VX_TYPE_I64 };

    vx_hj_build_cfg_t bcfg = {
        .keys = &key,
        .nkeys = 1,
        .payload_batch_cols = pay_cols,
        .payload_table_cols = pay_tbls,
        .payload_types = pay_types,
        .n_payload_cols = 1
    };

    vx_hj_table_t *ht = build_single_batch(&bcfg, &b_batch, scratch);
    ASSERT_EQ(ht->nrows, 3);

    double p_keys[4] = { +0.0, -0.0, (double)NAN, 2.0 };
    uint64_t p_val = 0x07; /* rows 0..2 valid, row 3 is NULL */

    vx_vec_t p_cols[2] = {
        { .type = VX_TYPE_F64, .kind = VX_VEC_FLAT, .has_nulls = 1, .phys_len = 4, .data = p_keys, .validity = &p_val }
    };
    vx_batch_t p_batch = {
        .cols = p_cols,
        .ncols = 1,
        .cap_cols = 2,
        .sel = { .count = 4, .is_identity = true }
    };

    vx_hj_out_col_t out_cols[] = {
        { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 },
        { .from_build = true,  .src_col_idx = 0, .table_col_idx = 1 }
    };
    vx_hj_probe_cfg_t pcfg = {
        .join_type = VX_JOIN_INNER,
        .ht = ht,
        .probe_keys = &key,
        .nkeys = 1,
        .out_cols = out_cols,
        .n_out_cols = 2
    };

    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
    void *gstate = NULL, *lstate = NULL;
    op.vtbl->init_global(&op, &ctx, &gstate);
    op.vtbl->init_local(&op, &ctx, gstate, &lstate);

    vx_vec_t out_cols_buf[4];
    vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
    vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &p_batch, &out);
    ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
    ASSERT_EQ(out.sel.count, 4);

    vx_batch_reset(&out);
    op.vtbl->destroy_local(lstate);
    op.vtbl->destroy_global(gstate);
    vx_owner_release(&ht->hdr);

    printf("  [PASS] test_ieee754_and_null_keys\n");
}

/* --- Test 5: SEMI & ANTI Joins --- */
static void test_semi_and_anti_join(vx_arena_t *scratch) {
    printf("Testing SEMI (&) and ANTI (~) joins...\n");

    int64_t b_keys[4] = { 10, 20, 20, 30 };
    vx_vec_t b_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = b_keys }
    };
    vx_batch_t b_batch = {
        .cols = b_cols,
        .ncols = 1,
        .cap_cols = 2,
        .sel = { .count = 4, .is_identity = true }
    };

    vx_join_key_t key = { .type = VX_TYPE_I64, .batch_col_idx = 0, .table_col_idx = 0 };
    vx_hj_build_cfg_t bcfg = {
        .keys = &key,
        .nkeys = 1,
        .payload_batch_cols = NULL,
        .payload_table_cols = NULL,
        .payload_types = NULL,
        .n_payload_cols = 0
    };

    vx_hj_table_t *ht = build_single_batch(&bcfg, &b_batch, scratch);
    ASSERT_EQ(ht->nrows, 4);

    int64_t p_keys[4] = { 5, 10, 20, 40 };
    vx_vec_t p_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 4, .data = p_keys }
    };
    vx_batch_t p_batch = {
        .cols = p_cols,
        .ncols = 1,
        .cap_cols = 2,
        .sel = { .count = 4, .is_identity = true }
    };

    vx_hj_out_col_t out_col = { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 };

    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };

    /* 1. SEMI join: matches 10 and 20 (exactly once!) */
    {
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_SEMI,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = &out_col,
            .n_out_cols = 1
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &p_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 2);

        ASSERT_EQ(out.sel.idx[0], 1);
        ASSERT_EQ(out.sel.idx[1], 2);

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    /* 2. ANTI join: only 5 and 40 survive */
    {
        vx_hj_probe_cfg_t pcfg = {
            .join_type = VX_JOIN_ANTI,
            .ht = ht,
            .probe_keys = &key,
            .nkeys = 1,
            .out_cols = &out_col,
            .n_out_cols = 1
        };
        vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
        void *gstate = NULL, *lstate = NULL;
        op.vtbl->init_global(&op, &ctx, &gstate);
        op.vtbl->init_local(&op, &ctx, gstate, &lstate);

        vx_vec_t out_cols_buf[4];
        vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
        vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &p_batch, &out);
        ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
        ASSERT_EQ(out.sel.count, 2);

        ASSERT_EQ(out.sel.idx[0], 0); /* 5 */
        ASSERT_EQ(out.sel.idx[1], 3); /* 40 */

        vx_batch_reset(&out);
        op.vtbl->destroy_local(lstate);
        op.vtbl->destroy_global(gstate);
    }

    vx_owner_release(&ht->hdr);
    printf("  [PASS] test_semi_and_anti_join\n");
}

/* --- Test 6: Multi-Key with StringView (Inline and Out-of-Line) --- */
static void test_multi_key_stringview(vx_arena_t *scratch) {
    printf("Testing multi-key join with StringView...\n");

    vx_strpool_t bpool;
    vx_strpool_init(&bpool, 4);

    vx_strview_t b_sv[3];
    b_sv[0] = vx_strview_inline((const uint8_t *)"alpha", 5);
    b_sv[1] = vx_strview_inline((const uint8_t *)"beta", 4);
    const char *long_str = "this_is_a_very_long_string_for_testing";
    vx_strpool_append(&bpool, (const uint8_t *)long_str, (int32_t)strlen(long_str), &b_sv[2]);

    int64_t b_dept[3] = { 1, 2, 3 };

    vx_vec_t b_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 3, .data = b_dept },
        { .type = VX_TYPE_STR, .kind = VX_VEC_FLAT, .phys_len = 3, .data = b_sv, .strpool = &bpool }
    };
    vx_batch_t b_batch = {
        .cols = b_cols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 3, .is_identity = true }
    };

    vx_join_key_t keys[2] = {
        { .type = VX_TYPE_I64, .batch_col_idx = 0, .table_col_idx = 0 },
        { .type = VX_TYPE_STR, .batch_col_idx = 1, .table_col_idx = 1 }
    };
    vx_hj_build_cfg_t bcfg = {
        .keys = keys,
        .nkeys = 2,
        .payload_batch_cols = NULL,
        .payload_table_cols = NULL,
        .payload_types = NULL,
        .n_payload_cols = 0
    };

    vx_hj_table_t *ht = build_single_batch(&bcfg, &b_batch, scratch);
    ASSERT_EQ(ht->nrows, 3);

    vx_strpool_t ppool;
    vx_strpool_init(&ppool, 4);

    vx_strview_t p_sv[3];
    p_sv[0] = vx_strview_inline((const uint8_t *)"alpha", 5);
    p_sv[1] = vx_strview_inline((const uint8_t *)"gamma", 5);
    vx_strpool_append(&ppool, (const uint8_t *)long_str, (int32_t)strlen(long_str), &p_sv[2]);

    int64_t p_dept[3] = { 1, 2, 3 };

    vx_vec_t p_cols[2] = {
        { .type = VX_TYPE_I64, .kind = VX_VEC_FLAT, .phys_len = 3, .data = p_dept },
        { .type = VX_TYPE_STR, .kind = VX_VEC_FLAT, .phys_len = 3, .data = p_sv, .strpool = &ppool }
    };
    vx_batch_t p_batch = {
        .cols = p_cols,
        .ncols = 2,
        .cap_cols = 2,
        .sel = { .count = 3, .is_identity = true }
    };

    vx_hj_out_col_t out_cols[] = {
        { .from_build = false, .src_col_idx = 0, .table_col_idx = 0 },
        { .from_build = false, .src_col_idx = 1, .table_col_idx = 1 }
    };
    vx_hj_probe_cfg_t pcfg = {
        .join_type = VX_JOIN_INNER,
        .ht = ht,
        .probe_keys = keys,
        .nkeys = 2,
        .out_cols = out_cols,
        .n_out_cols = 2
    };

    vx_err_t err = {0};
    vx_exec_ctx_t ctx = {
        .scratch = scratch,
        .work_mem_bytes = 64 * 1024 * 1024,
        .err = &err
    };
    vx_op_t op = { .vtbl = &VX_OP_HASH_JOIN_PROBE, .plan_cfg = &pcfg };
    void *gstate = NULL, *lstate = NULL;
    op.vtbl->init_global(&op, &ctx, &gstate);
    op.vtbl->init_local(&op, &ctx, gstate, &lstate);

    vx_vec_t out_cols_buf[4];
    vx_batch_t out = { .cols = out_cols_buf, .ncols = 0, .cap_cols = 4 };
    vx_op_res_t res = op.vtbl->execute(&op, &ctx, gstate, lstate, &p_batch, &out);
    ASSERT_EQ(res, VX_OP_NEED_MORE_INPUT);
    ASSERT_EQ(out.sel.count, 2);

    const int64_t *out_dept = (const int64_t *)out.cols[0].data;
    ASSERT_EQ(out_dept[0], 1);
    ASSERT_EQ(out_dept[1], 3);

    vx_batch_reset(&out);
    op.vtbl->destroy_local(lstate);
    op.vtbl->destroy_global(gstate);

    vx_strpool_destroy(&ppool);
    vx_strpool_destroy(&bpool);
    vx_owner_release(&ht->hdr);

    printf("  [PASS] test_multi_key_stringview\n");
}

int main(void) {
    printf("=== Starting V2 Hash Join Test Suite ===\n");
    vx_arena_t *scratch = vx_arena_create(64 * 1024, 1024 * 1024, 0);
    assert(scratch != NULL);

    test_empty_build_table(scratch);
    test_inner_and_left_equi_join(scratch);
    test_resumable_1_to_n_expansion(scratch);
    test_ieee754_and_null_keys(scratch);
    test_semi_and_anti_join(scratch);
    test_multi_key_stringview(scratch);

    vx_owner_release(&scratch->hdr);
    printf("=== All V2 Hash Join Tests Passed Successfully! ===\n");
    return 0;
}
