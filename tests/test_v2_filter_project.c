#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vx_types.h"
#include "vx_owner.h"
#include "vx_arena.h"
#include "vx_str.h"
#include "vx_batch.h"
#include "vx_op.h"
#include "vx_op_filter.h"
#include "vx_op_project.h"
#include "vx_simd.h"

/* Minimal mock arena for test */
typedef struct mock_arena {
    vx_owner_t hdr;
    uint8_t   *buf;
    size_t     cap;
    size_t     used;
} mock_arena_t;

static void mock_arena_destroy(vx_owner_t *o) {
    mock_arena_t *ma = (mock_arena_t *)o;
    free(ma->buf);
    free(ma);
}

static vx_arena_t *create_mock_arena(size_t cap) {
    mock_arena_t *ma = (mock_arena_t *)calloc(1, sizeof(*ma));
    vx_owner_init(&ma->hdr, VX_OWN_ARENA, mock_arena_destroy);
    ma->buf = (uint8_t *)malloc(cap);
    ma->cap = cap;
    ma->used = 0;
    return (vx_arena_t *)ma;
}

void *vx_arena_alloc(vx_arena_t *a, size_t size) {
    mock_arena_t *ma = (mock_arena_t *)a;
    size_t aligned = (size + 63u) & ~63u;
    if (ma->used + aligned > ma->cap) return NULL;
    void *p = ma->buf + ma->used;
    ma->used += aligned;
    return p;
}

void *vx_arena_calloc(vx_arena_t *a, size_t count, size_t elem_size) {
    size_t tot = count * elem_size;
    void *p = vx_arena_alloc(a, tot);
    if (p) memset(p, 0, tot);
    return p;
}

/* Scalar SIMD filter mock functions for testing */
static uint32_t mock_filter_i64(const int64_t *col, const uint64_t *validity,
                                const vx_sel_t *in_sel, vx_sel_t *out_sel,
                                vx_cmp_op_t op, int64_t rhs) {
    uint32_t out_n = 0;
    for (uint32_t i = 0; i < in_sel->count; ++i) {
        uint32_t r = in_sel->is_identity ? i : in_sel->idx[i];
        if (validity && ((validity[r >> 6] >> (r & 63u)) & 1ULL) == 0) continue;
        bool match = false;
        switch (op) {
        case VX_CMP_EQ: match = (col[r] == rhs); break;
        case VX_CMP_NE: match = (col[r] != rhs); break;
        case VX_CMP_LT: match = (col[r] < rhs); break;
        case VX_CMP_LE: match = (col[r] <= rhs); break;
        case VX_CMP_GT: match = (col[r] > rhs); break;
        case VX_CMP_GE: match = (col[r] >= rhs); break;
        }
        if (match) out_sel->idx[out_n++] = r;
    }
    out_sel->count = out_n;
    out_sel->is_identity = (in_sel->is_identity && out_n == in_sel->count);
    return out_n;
}

static const vx_simd_ops_t MOCK_SIMD = {
    .isa_name = "mock_scalar",
    .filter_i64 = mock_filter_i64
};

uint32_t vx_delmap_live_sel(const vx_delmap_t *dm, uint32_t nrows, uint32_t *out_sel) {
    (void)dm; (void)nrows; (void)out_sel;
    return 0;
}

/* Dummy builtin evaluator */
vx_status_t vx_builtin_eval(vx_builtin_id_t fn, const vx_vec_t *args, uint32_t nargs,
                            const vx_sel_t *sel, vx_arena_t *scratch, vx_vec_t *out) {
    (void)fn; (void)args; (void)nargs; (void)sel; (void)scratch; (void)out;
    return VX_ERR_INVALID;
}

void vx_batch_reset(vx_batch_t *b) {
    if (!b) return;
    if (b->row_src.src_owner) {
        vx_owner_release(b->row_src.src_owner);
        b->row_src.src_owner = NULL;
    }
    for (uint32_t c = 0; c < b->ncols; ++c) {
        if (b->cols[c].owner) {
            vx_owner_release(b->cols[c].owner);
            b->cols[c].owner = NULL;
        }
    }
    b->ncols = 0;
    b->sel.count = 0;
    b->sel.is_identity = true;
}

int main(void) {
    printf("[test_v2_filter_project] Testing Vector Compaction, Filter, and Project...\n");

    vx_arena_t *arena = create_mock_arena(64 * 1024);

    /* 1. Test vx_vec_compact with i64 data */
    int64_t raw_data[10] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    uint64_t val_bits = 0x3FF; /* all 10 valid */
    val_bits &= ~(1ULL << 3);  /* row 3 (val 40) is null */

    vx_vec_t in_vec = {
        .type = VX_TYPE_I64,
        .kind = VX_VEC_FLAT,
        .has_nulls = 1,
        .phys_len = 10,
        .data = raw_data,
        .validity = &val_bits,
        .owner = NULL
    };

    /* Select rows 1, 3, 5, 7 */
    vx_sel_t sel;
    sel.count = 4;
    sel.is_identity = false;
    sel.idx[0] = 1; /* 20 (valid) */
    sel.idx[1] = 3; /* 40 (null) */
    sel.idx[2] = 5; /* 60 (valid) */
    sel.idx[3] = 7; /* 80 (valid) */

    vx_vec_t out_vec;
    vx_status_t st = vx_vec_compact(&in_vec, &sel, arena, &out_vec);
    assert(st == VX_OK);
    assert(out_vec.phys_len == 4);
    assert(out_vec.has_nulls == 1);
    assert(out_vec.validity != NULL);

    int64_t *compacted = (int64_t *)out_vec.data;
    assert(compacted[0] == 20);
    assert(compacted[2] == 60);
    assert(compacted[3] == 80);

    /* Check validity: row 0 valid, row 1 null, row 2 valid, row 3 valid */
    assert((out_vec.validity[0] & (1ULL << 0)) != 0);
    assert((out_vec.validity[0] & (1ULL << 1)) == 0);
    assert((out_vec.validity[0] & (1ULL << 2)) != 0);
    assert((out_vec.validity[0] & (1ULL << 3)) != 0);

    vx_owner_release(out_vec.owner);

    /* 2. Test vx_batch_forward_shallow */
    vx_vec_t batch_cols[2] = { in_vec, in_vec };
    vx_batch_t in_batch = {
        .cols = batch_cols,
        .ncols = 2,
        .cap_cols = 4,
        .sel = sel,
        .row_src = { .src_ctx = NULL, .src_owner = NULL }
    };
    vx_vec_t out_cols[4];
    memset(out_cols, 0, sizeof(out_cols));
    vx_batch_t out_batch = {
        .cols = out_cols,
        .ncols = 0,
        .cap_cols = 4
    };

    vx_batch_forward_shallow(&in_batch, &out_batch);
    assert(out_batch.ncols == 2);
    assert(out_batch.sel.count == 4);
    assert(out_batch.sel.idx[0] == 1);

    /* 3. Test Filter operator */
    vx_filter_pred_t p0;
    memset(&p0, 0, sizeof(p0));
    p0.kind = VX_PRED_COL_LIT;
    p0.type = VX_TYPE_I64;
    p0.cmp = VX_CMP_GT;
    p0.batch_col_idx = 0;
    p0.table_col_idx = 0;
    p0.lit.i64 = 50;

    vx_filter_cfg_t fcfg;
    fcfg.npreds = 1;
    fcfg.preds = &p0;

    vx_op_t filter_op = {
        .vtbl = &VX_OP_FILTER,
        .out_types = NULL,
        .out_ncols = 2,
        .plan_cfg = &fcfg
    };

    vx_err_t err;
    vx_exec_ctx_t ctx = {
        .worker_id = 0,
        .nworkers = 1,
        .scratch = arena,
        .out_arena = arena,
        .simd = &MOCK_SIMD,
        .work_mem_bytes = 1024 * 1024,
        .cancelled = NULL,
        .err = &err
    };

    /* In batch has sel = [1, 3, 5, 7] corresponding to values [20, 40 (null), 60, 80].
     * Filter i64 > 50 should leave [5, 7] (values 60 and 80). */
    printf("in_batch.cols[0].kind = %d, ncols=%u\n", in_batch.cols[0].kind, in_batch.ncols);
    vx_op_res_t op_res = VX_OP_FILTER.execute(&filter_op, &ctx, NULL, NULL, &in_batch, &out_batch);
    printf("op_res: %d, err code: %d\n", (int)op_res, (int)err.code);
    assert(op_res == VX_OP_NEED_MORE_INPUT);
    assert(out_batch.sel.count == 2);
    assert(out_batch.sel.idx[0] == 5);
    assert(out_batch.sel.idx[1] == 7);

    /* 4. Test Project operator */
    vx_proj_item_t proj_items[2];
    memset(proj_items, 0, sizeof(proj_items));

    /* Item 0: pass-through column 0 */
    proj_items[0].kind = VX_PROJ_COL;
    proj_items[0].batch_col_idx = 0;
    proj_items[0].table_col_idx = 0;

    /* Item 1: literal constant scalar */
    int64_t lit_val = 999;
    proj_items[1].kind = VX_PROJ_LITERAL;
    proj_items[1].lit_vec.type = VX_TYPE_I64;
    proj_items[1].lit_vec.kind = VX_VEC_CONST;
    proj_items[1].lit_vec.data = &lit_val;
    proj_items[1].lit_vec.owner = NULL;

    vx_project_cfg_t pcfg = {
        .items = proj_items,
        .n_items = 2,
        .has_computed = false
    };

    vx_op_t proj_op = {
        .vtbl = &VX_OP_PROJECT,
        .out_types = NULL,
        .out_ncols = 2,
        .plan_cfg = &pcfg
    };

    vx_vec_t proj_cols[4];
    memset(proj_cols, 0, sizeof(proj_cols));
    vx_batch_t proj_out = {
        .cols = proj_cols,
        .ncols = 0,
        .cap_cols = 4
    };

    vx_op_res_t proj_res = VX_OP_PROJECT.execute(&proj_op, &ctx, NULL, NULL, &out_batch, &proj_out);
    assert(proj_res == VX_OP_NEED_MORE_INPUT);
    assert(proj_out.ncols == 2);
    assert(proj_out.sel.count == 2);
    assert(proj_out.cols[0].data == in_vec.data); /* zero-copy pass-through */
    assert(proj_out.cols[1].kind == VX_VEC_CONST);

    /* Clean up mock arena */
    vx_owner_release(&arena->hdr);

    printf("[test_v2_filter_project] ALL TESTS PASSED!\n");
    return 0;
}
