#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

#include "src/vx_types.h"
#include "src/vx_owner.h"
#include "src/vx_arena.h"
#include "src/vx_str.h"
#include "src/vx_chunk.h"
#include "src/vx_batch.h"
#include "src/vx_op.h"
#include "src/vx_op_filter.h"
#include "src/vx_op_project.h"
#include "src/vx_result.h"
#include "src/vx_simd_num_common.h"

/* --- Mock Table Source for Pipeline Verification --- */
typedef struct mock_src_state {
    uint32_t batch_idx;
    uint32_t total_batches;
} mock_src_state_t;

static vx_status_t mock_src_init_global(const vx_source_t *s, vx_exec_ctx_t *ctx, void **gstate) {
    (void)s; (void)ctx;
    mock_src_state_t *st = (mock_src_state_t *)calloc(1, sizeof(*st));
    st->total_batches = 2; /* 2 batches to test multi-batch ResultCollector & lazy stitch */
    *gstate = st;
    return VX_OK;
}

static vx_status_t mock_src_init_local(const vx_source_t *s, vx_exec_ctx_t *ctx, void *gstate, void **lstate) {
    (void)s; (void)ctx;
    *lstate = gstate;
    return VX_OK;
}

static vx_op_res_t mock_src_next_batch(const vx_source_t *s, vx_exec_ctx_t *ctx,
                                       void *gstate, void *lstate, vx_batch_t *out) {
    (void)s; (void)lstate;
    mock_src_state_t *st = (mock_src_state_t *)gstate;
    if (st->batch_idx >= st->total_batches) {
        return VX_OP_FINISHED;
    }

    const uint32_t N = 100;
    int64_t *d0 = (int64_t *)vx_arena_alloc(ctx->scratch, N * sizeof(int64_t));
    double  *d1 = (double  *)vx_arena_alloc(ctx->scratch, N * sizeof(double));

    for (uint32_t i = 0; i < N; ++i) {
        d0[i] = (int64_t)(st->batch_idx * 100 + i);
        d1[i] = (double)(d0[i]) * 1.5;
    }

    out->cols[0].type = VX_TYPE_I64;
    out->cols[0].kind = VX_VEC_FLAT;
    out->cols[0].phys_len = N;
    out->cols[0].data = d0;
    out->cols[0].owner = vx_owner_retain(&ctx->scratch->hdr);

    out->cols[1].type = VX_TYPE_F64;
    out->cols[1].kind = VX_VEC_FLAT;
    out->cols[1].phys_len = N;
    out->cols[1].data = d1;
    out->cols[1].owner = vx_owner_retain(&ctx->scratch->hdr);

    out->ncols = 2;
    out->sel.count = N;
    out->sel.is_identity = true;

    st->batch_idx++;
    return VX_OP_NEED_MORE_INPUT;
}

static void mock_src_destroy_local(void *lstate) { (void)lstate; }
static void mock_src_destroy_global(void *gstate) { free(gstate); }

static const vx_source_vtbl_t MOCK_SRC_VTBL = {
    .name = "MockSource",
    .init_global = mock_src_init_global,
    .init_local = mock_src_init_local,
    .next_batch = mock_src_next_batch,
    .destroy_local = mock_src_destroy_local,
    .destroy_global = mock_src_destroy_global
};

/* Mock SIMD ops for filter execution */
static const vx_simd_ops_t MOCK_SIMD_OPS = {
    .isa_name = "mock_scalar",
    .filter_i64 = vx_simd_scalar_filter_i64_ref,
    .filter_u64 = vx_simd_scalar_filter_u64_ref,
    .filter_f64 = vx_simd_scalar_filter_f64_ref,
    .sum_f64 = vx_simd_scalar_sum_f64_ref,
    .minmax_f64 = vx_simd_scalar_minmax_f64_ref
};


int main(void) {
    printf("=== Starting V2 Pipeline & ResultCollector Test Suite ===\n");

    /* 1. Build Pipeline: Source -> Filter (col0 >= 50) -> Project [col0, col1] -> ResultCollector */
    vx_type_t src_types[] = { VX_TYPE_I64, VX_TYPE_F64 };

    vx_source_t source = {
        .vtbl = &MOCK_SRC_VTBL,
        .out_types = src_types,
        .out_ncols = 2,
        .plan_cfg = NULL
    };

    /* Filter plan: col0 >= 50 */
    vx_filter_pred_t pred = {
        .kind = VX_PRED_COL_LIT,
        .cmp = VX_CMP_GE,
        .type = VX_TYPE_I64,
        .negate = false,
        .batch_col_idx = 0,
        .table_col_idx = 0,
        .lit = { .i64 = 50 },
        .builtin_id = 0,
        .nargs = 0,
        .args = { {0} }
    };
    vx_filter_cfg_t filter_plan = {
        .preds = &pred,
        .npreds = 1
    };

    vx_op_t filter_op = {
        .vtbl = &VX_OP_FILTER,
        .out_types = src_types,
        .out_ncols = 2,
        .plan_cfg = &filter_plan
    };

    /* Project plan: pass through [col0, col1] */
    vx_proj_item_t exprs[] = {
        { .kind = VX_PROJ_COL, .batch_col_idx = 0, .table_col_idx = 0, .lit_vec = {0}, .builtin_id = 0, .nargs = 0, .args = { {0} } },
        { .kind = VX_PROJ_COL, .batch_col_idx = 1, .table_col_idx = 1, .lit_vec = {0}, .builtin_id = 0, .nargs = 0, .args = { {0} } }
    };
    vx_project_cfg_t proj_plan = {
        .items = exprs,
        .n_items = 2,
        .has_computed = false
    };

    vx_op_t proj_op = {
        .vtbl = &VX_OP_PROJECT,
        .out_types = src_types,
        .out_ncols = 2,
        .plan_cfg = &proj_plan
    };

    vx_op_t ops[] = { filter_op, proj_op };

    /* Sink: ResultCollector */
    const char *col_names[] = { "id", "score" };
    vx_result_cfg_t res_cfg = {
        .ncols = 2,
        .col_names = col_names,
        .col_types = src_types
    };

    vx_sink_t sink = {
        .vtbl = &VX_SINK_RESULT,
        .plan_cfg = &res_cfg
    };

    vx_pipeline_t pipe = {
        .source = source,
        .ops = ops,
        .n_ops = 2,
        .sink = sink
    };

    /* 2. Setup Execution Context */
    vx_arena_t *root_arena = vx_arena_create(64u * 1024u, 4u * 1024u * 1024u, 0);
    assert(root_arena != NULL);

    _Atomic bool cancelled = false;
    vx_err_t err;
    memset(&err, 0, sizeof(err));

    vx_exec_ctx_t root_ctx = {
        .worker_id = 0,
        .nworkers = 1,
        .scratch = root_arena,
        .out_arena = root_arena,
        .simd = &MOCK_SIMD_OPS,
        .work_mem_bytes = 64 * 1024 * 1024,
        .cancelled = &cancelled,
        .err = &err
    };

    /* 3. Execute Pipeline */
    void *sink_gstate = NULL;
    vx_status_t st = vx_pipeline_execute(&pipe, NULL, &root_ctx, &sink_gstate);
    assert(st == VX_OK);
    assert(sink_gstate != NULL);

    /* 4. Extract Result */
    vx_result_t *res = vx_result_sink_take(sink_gstate);
    assert(res != NULL);
    assert(res->ncols == 2);
    assert(strcmp(res->col_names[0], "id") == 0);
    assert(strcmp(res->col_names[1], "score") == 0);

    /* Batch 0 had 0..99 -> rows >= 50: 50 rows survive
     * Batch 1 had 100..199 -> all 100 rows survive
     * Total rows = 150 */
    printf("Result: total_rows=%llu, nbatches=%u\n", (unsigned long long)res->total_rows, res->nbatches);
    assert(res->total_rows == 150);
    assert(res->nbatches == 2);

    /* 5. Test Lazy Contiguous Stitching (ColumnView) */
    vx_vec_t col0_stitched;
    st = vx_result_get_contiguous_col(res, 0, &col0_stitched);
    assert(st == VX_OK);
    assert(col0_stitched.phys_len == 150);
    assert(col0_stitched.kind == VX_VEC_FLAT);

    const int64_t *ids = (const int64_t *)col0_stitched.data;
    printf("ids[0] = %lld, ids[49] = %lld, ids[50] = %lld\n",
           (long long)ids[0], (long long)ids[49], (long long)ids[50]);
    assert(ids[0] == 50);
    assert(ids[49] == 99);
    assert(ids[50] == 100);
    assert(ids[149] == 199);

    /* Test second get_contiguous_col call uses flat_ready cache */
    vx_vec_t col0_cached;
    st = vx_result_get_contiguous_col(res, 0, &col0_cached);
    assert(st == VX_OK);
    assert(col0_cached.data == col0_stitched.data);

    /* Release vector views */
    vx_owner_release(col0_stitched.owner);
    vx_owner_release(col0_cached.owner);

    /* Release Result */
    vx_owner_release(&res->hdr);
    vx_owner_release(&root_arena->hdr);

    printf("=== All V2 Pipeline & ResultCollector Tests Passed! ===\n");
    return 0;
}
