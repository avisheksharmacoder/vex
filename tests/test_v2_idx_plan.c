#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/vx_art_fmt.h"
#include "src/vx_idx.h"
#include "src/vx_plan.h"
#include "src/vx_seg.h"
#include "src/vx_manifest.h"
#include "src/vx_chunk.h"
#include "include/vx_ast.h"

static void dummy_owner_destroy(vx_owner_t *o) {
    (void)o;
}

/* ============================================================================
 * 1. Test Order-Preserving Key Codecs
 * ============================================================================ */
static void test_key_codecs(void) {
    printf("[1/5] Testing order-preserving key codecs (u64, i64, f64, str)...\n");

    /* 1.1 U64 Codec */
    {
        uint64_t u_vals[] = {
            0, 1, 2, 42, 100, 1000, 65535, 1000000ULL,
            0x7FFFFFFFFFFFFFFFULL, UINT64_MAX - 1, UINT64_MAX
        };
        size_t n = sizeof(u_vals) / sizeof(u_vals[0]);
        for (size_t i = 0; i < n - 1; ++i) {
            uint8_t enc_a[8], enc_b[8];
            vx_key_encode_u64(u_vals[i], enc_a);
            vx_key_encode_u64(u_vals[i + 1], enc_b);
            assert(memcmp(enc_a, enc_b, 8) < 0);
        }
    }

    /* 1.2 I64 Codec: Sign-bit flip + big-endian */
    {
        int64_t i_vals[] = {
            INT64_MIN, INT64_MIN + 1, -1000000, -42, -1,
            0, 1, 42, 1000000, INT64_MAX - 1, INT64_MAX
        };
        size_t n = sizeof(i_vals) / sizeof(i_vals[0]);
        for (size_t i = 0; i < n - 1; ++i) {
            uint8_t enc_a[8], enc_b[8];
            vx_key_encode_i64(i_vals[i], enc_a);
            vx_key_encode_i64(i_vals[i + 1], enc_b);
            assert(memcmp(enc_a, enc_b, 8) < 0);
        }
    }

    /* 1.3 F64 Codec: Monotonic IEEE-754 flip + big-endian, -0.0 -> +0.0 */
    {
        double f_vals[] = {
            -INFINITY, -10000.5, -42.0, -1.0, -0.001,
            0.0, 0.001, 1.0, 42.0, 10000.5, INFINITY
        };
        size_t n = sizeof(f_vals) / sizeof(f_vals[0]);
        for (size_t i = 0; i < n - 1; ++i) {
            uint8_t enc_a[8], enc_b[8];
            vx_key_encode_f64(f_vals[i], enc_a);
            vx_key_encode_f64(f_vals[i + 1], enc_b);
            assert(memcmp(enc_a, enc_b, 8) < 0);
        }

        /* Check that -0.0 and +0.0 encode to identical bytes */
        uint8_t enc_neg_zero[8], enc_pos_zero[8];
        vx_key_encode_f64(-0.0, enc_neg_zero);
        vx_key_encode_f64(+0.0, enc_pos_zero);
        assert(memcmp(enc_neg_zero, enc_pos_zero, 8) == 0);
    }

    /* 1.4 String Codec: 0x00 escaped to 0x00 0x01, terminated with 0x00 0x00 */
    {
        const char *s_vals[] = {
            "", "a", "aa", "ab", "apple", "apply", "banana", "zebra"
        };
        size_t n = sizeof(s_vals) / sizeof(s_vals[0]);
        for (size_t i = 0; i < n - 1; ++i) {
            uint8_t enc_a[64], enc_b[64];
            size_t la = vx_key_encode_str((const uint8_t *)s_vals[i], (int32_t)strlen(s_vals[i]), enc_a);
            size_t lb = vx_key_encode_str((const uint8_t *)s_vals[i + 1], (int32_t)strlen(s_vals[i + 1]), enc_b);
            assert(la >= 2 && lb >= 2);
            /* Check termination */
            assert(enc_a[la - 2] == 0x00 && enc_a[la - 1] == 0x00);
            assert(enc_b[lb - 2] == 0x00 && enc_b[lb - 1] == 0x00);
            /* Lexicographical comparison */
            int cmp = memcmp(enc_a, enc_b, la < lb ? la : lb);
            assert(cmp < 0 || (cmp == 0 && la < lb));
        }

        /* Check embedded null byte */
        uint8_t s_null1[] = { 'a', 0x00, 'b' };
        uint8_t s_null2[] = { 'a', 0x00, 'c' };
        uint8_t enc1[32], enc2[32];
        size_t l1 = vx_key_encode_str(s_null1, 3, enc1);
        size_t l2 = vx_key_encode_str(s_null2, 3, enc2);
        assert(l1 == 6); /* 'a', 0x00, 0x01, 'b', 0x00, 0x00 */
        assert(enc1[0] == 'a' && enc1[1] == 0x00 && enc1[2] == 0x01 && enc1[3] == 'b');
        assert(memcmp(enc1, enc2, l1 < l2 ? l1 : l2) < 0);
    }
}

/* ============================================================================
 * 2. Helper: Build Test Segment & Chunks
 * ============================================================================ */
static void free_test_chunk(vx_chunk_t *ch) {
    if (!ch) return;
    for (uint32_t c = 0; c < ch->ncols; ++c) {
        free((void *)ch->cols[c].data);
        free((void *)ch->cols[c].validity);
        if (ch->cols[c].strpool) {
            free((void *)ch->cols[c].strpool->ptrs);
            free(ch->cols[c].strpool->lens);
            free(ch->cols[c].strpool);
        }
    }
    free(ch->cols);
    free(ch);
}

static void create_test_segment(const char *seg_path, uint32_t total_rows) {
    (void)unlink(seg_path);

    /* Schema:
     * Col 0: U64 PK (0 .. total_rows - 1)
     * Col 1: I64 (values: -total_rows/2 .. +total_rows/2 - 1)
     * Col 2: U64 non-unique (row % 10)
     */
    vx_type_t col_types[3] = { VX_TYPE_U64, VX_TYPE_I64, VX_TYPE_U64 };
    const uint32_t ncols = 3;

    uint32_t ch0_rows = total_rows > 300 ? 300 : total_rows / 2;
    uint32_t ch1_rows = total_rows - ch0_rows;

    vx_chunk_t *ch0 = (vx_chunk_t *)calloc(1, sizeof(*ch0));
    vx_chunk_t *ch1 = (vx_chunk_t *)calloc(1, sizeof(*ch1));
    assert(ch0 && ch1);
    vx_owner_init(&ch0->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    vx_owner_init(&ch1->hdr, VX_OWN_CHUNK, dummy_owner_destroy);

    ch0->nrows = ch0_rows; ch0->ncols = ncols; ch0->cols = (vx_col_chunk_t *)calloc(ncols, sizeof(vx_col_chunk_t));
    ch1->nrows = ch1_rows; ch1->ncols = ncols; ch1->cols = (vx_col_chunk_t *)calloc(ncols, sizeof(vx_col_chunk_t));

    for (int ci = 0; ci < 2; ++ci) {
        vx_chunk_t *ch = (ci == 0) ? ch0 : ch1;
        uint32_t base_idx = (ci == 0) ? 0 : ch0_rows;
        uint32_t nr = ch->nrows;

        /* Col 0: PK */
        ch->cols[0].type = VX_TYPE_U64;
        uint64_t *pks = (uint64_t *)malloc(nr * sizeof(uint64_t));
        for (uint32_t r = 0; r < nr; ++r) pks[r] = base_idx + r;
        ch->cols[0].data = pks;

        /* Col 1: I64 */
        ch->cols[1].type = VX_TYPE_I64;
        int64_t *i64s = (int64_t *)malloc(nr * sizeof(int64_t));
        for (uint32_t r = 0; r < nr; ++r) {
            i64s[r] = (int64_t)(base_idx + r) - (int64_t)(total_rows / 2);
        }
        ch->cols[1].data = i64s;

        /* Col 2: U64 non-unique */
        ch->cols[2].type = VX_TYPE_U64;
        uint64_t *mods = (uint64_t *)malloc(nr * sizeof(uint64_t));
        for (uint32_t r = 0; r < nr; ++r) mods[r] = (base_idx + r) % 10;
        ch->cols[2].data = mods;
    }

    const vx_chunk_t *chunks[2] = { ch0, ch1 };
    vx_seg_write_cfg_t cfg = {
        .table_id    = 1,
        .segment_id  = 100,
        .lsn_max     = 1000,
        .ncols       = ncols,
        .pk_col_idx  = 0,
        .col_types   = col_types,
        .chunks      = chunks,
        .delmaps     = NULL,
        .nchunks     = 2,
        .spill_dir   = "/tmp"
    };

    vx_seg_write_info_t info;
    vx_status_t st = vx_seg_write(seg_path, &cfg, &info);
    assert(st == VX_OK);
    assert(info.file_bytes > 0);

    free_test_chunk(ch0);
    free_test_chunk(ch1);
}

/* Range scan collector callback */
typedef struct {
    uint32_t hit_count;
    uint32_t hits[1024];
} test_hit_ctx_t;

static vx_status_t test_on_hit(void *ctx, uint32_t row_idx, bool *out_stop) {
    (void)out_stop;
    test_hit_ctx_t *tc = (test_hit_ctx_t *)ctx;
    if (tc->hit_count < sizeof(tc->hits) / sizeof(tc->hits[0])) {
        tc->hits[tc->hit_count] = row_idx;
    }
    tc->hit_count++;
    return VX_OK;
}

/* ============================================================================
 * 2. Test ART Build, Point Lookups, Range Scans, and Delmap Filtering
 * ============================================================================ */
static void test_art_point_and_range_scans(void) {
    printf("[2/5] Testing ART secondary index build, point lookups & range scans...\n");

    const char *seg_path = "/tmp/test_v2_art_seg.vx";
    const char *idx_path = "/tmp/seg_100_i1.idx";
    (void)unlink(seg_path);
    (void)unlink(idx_path);

    const uint32_t total_rows = 500;
    create_test_segment(seg_path, total_rows);

    vx_seg_t *seg = NULL;
    vx_status_t st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);
    assert(seg != NULL);

    /* Build ART Index on Column 1 (score, I64) */
    uint32_t col_ids[1] = { 1 };
    vx_man_index_def_t idef = {
        .index_id = 1,
        .name     = (char *)"idx_score",
        .col_ids  = col_ids,
        .ncols    = 1,
        .flags    = VX_IDX_KIND_ART
    };
    vx_idx_file_t *idx = NULL;
    st = vx_idx_build_from_seg("/tmp", seg, &idef, &idx);
    assert(st == VX_OK);
    assert(idx != NULL);
    assert(idx->file_hdr->kind == VX_IDX_KIND_ART);
    assert(idx->file_hdr->nrows_indexed == total_rows);

    /* Point Equality Lookups */
    {
        /* Lookup key corresponding to row 100: key = 100 - 250 = -150 */
        int64_t target_key = -150;
        uint8_t enc_key[8];
        vx_key_encode_i64(target_key, enc_key);
        size_t klen = 8;

        test_hit_ctx_t tc = {0};
        st = vx_idx_lookup_eq(idx, NULL, enc_key, (uint32_t)klen, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 1);
        assert(tc.hits[0] == 100);

        /* Lookup non-existent key: 99999 */
        vx_key_encode_i64(99999, enc_key);
        tc.hit_count = 0;
        st = vx_idx_lookup_eq(idx, NULL, enc_key, (uint32_t)klen, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 0);
    }

    /* Range Scan 1: Unbounded [-inf, +inf] */
    {
        vx_art_bound_t low = { .key = NULL, .len = 0, .inclusive = true, .unbounded = true };
        vx_art_bound_t high = { .key = NULL, .len = 0, .inclusive = true, .unbounded = true };
        test_hit_ctx_t tc = {0};
        st = vx_art_scan_range(idx, NULL, low, high, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == total_rows);
        /* Verify hits are ordered monotonically by key (which matches row index here) */
        for (uint32_t i = 0; i < total_rows; ++i) {
            assert(tc.hits[i] == i);
        }
    }

    /* Range Scan 2: Bounded inclusive [-50, +50] (keys -50 .. +50 => rows 200 .. 300 => 101 rows) */
    {
        uint8_t low_key[8], high_key[8];
        vx_key_encode_i64(-50, low_key);
        vx_key_encode_i64(+50, high_key);

        vx_art_bound_t low  = { .key = low_key, .len = 8, .inclusive = true, .unbounded = false };
        vx_art_bound_t high = { .key = high_key, .len = 8, .inclusive = true, .unbounded = false };
        test_hit_ctx_t tc = {0};
        st = vx_art_scan_range(idx, NULL, low, high, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 101);
        assert(tc.hits[0] == 200);
        assert(tc.hits[100] == 300);
    }

    /* Range Scan 3: Exclusive bounds (-50, +50) => rows 201 .. 299 => 99 rows */
    {
        uint8_t low_key[8], high_key[8];
        vx_key_encode_i64(-50, low_key);
        vx_key_encode_i64(+50, high_key);

        vx_art_bound_t low  = { .key = low_key, .len = 8, .inclusive = false, .unbounded = false };
        vx_art_bound_t high = { .key = high_key, .len = 8, .inclusive = false, .unbounded = false };
        test_hit_ctx_t tc = {0};
        st = vx_art_scan_range(idx, NULL, low, high, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 99);
        assert(tc.hits[0] == 201);
        assert(tc.hits[98] == 299);
    }

    /* Delete Filtering via chunk_delmaps */
    {
        /* Create a delmap and delete row 100 */
        vx_delmap_t *dm = vx_delmap_create(4096);
        assert(dm != NULL);
        dm->bits[100 >> 6] |= (1ULL << (100 & 63u));
        dm->live_count--;

        const vx_delmap_t *delmaps[2] = { dm, NULL };

        /* Point lookup on row 100 should now return 0 hits */
        int64_t target_key = -150;
        uint8_t enc_key[8];
        vx_key_encode_i64(target_key, enc_key);
        size_t klen = 8;

        test_hit_ctx_t tc = {0};
        st = vx_idx_lookup_eq(idx, delmaps, enc_key, (uint32_t)klen, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 0); /* Filtered out by delmap! */

        /* Range scan over [-50, +50]: delete row 250 in delmap */
        dm->bits[250 >> 6] |= (1ULL << (250 & 63u));
        dm->live_count--;
        uint8_t low_key[8], high_key[8];
        vx_key_encode_i64(-50, low_key);
        vx_key_encode_i64(+50, high_key);
        vx_art_bound_t low  = { .key = low_key, .len = 8, .inclusive = true, .unbounded = false };
        vx_art_bound_t high = { .key = high_key, .len = 8, .inclusive = true, .unbounded = false };
        tc.hit_count = 0;
        st = vx_art_scan_range(idx, delmaps, low, high, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 100); /* 101 minus row 250 */
        for (uint32_t i = 0; i < tc.hit_count; ++i) {
            assert(tc.hits[i] != 250);
        }

        vx_owner_release(&dm->hdr);
    }

    vx_idx_close(idx);
    vx_owner_release(&seg->hdr);
    unlink(seg_path);
    unlink(idx_path);
}

/* ============================================================================
 * 3. Test Secondary Hash Index & Non-Unique ART Posting Lists
 * ============================================================================ */
static void test_hash_index_and_posting_lists(void) {
    printf("[3/5] Testing secondary Robin Hood hash index & non-unique posting lists...\n");

    const char *seg_path  = "/tmp/test_v2_hash_seg.vx";
    const char *hidx_path = "/tmp/seg_100_i0.idx";
    const char *nuidx_path = "/tmp/seg_100_i2.idx";
    (void)unlink(seg_path);
    (void)unlink(hidx_path);
    (void)unlink(nuidx_path);

    const uint32_t total_rows = 500;
    create_test_segment(seg_path, total_rows);

    vx_seg_t *seg = NULL;
    vx_status_t st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);

    /* 3.1 Build Hash Index on Column 0 (PK, unique) */
    uint32_t col_ids0[1] = { 0 };
    vx_man_index_def_t idef0 = {
        .index_id = 0,
        .name     = (char *)"idx_pk",
        .col_ids  = col_ids0,
        .ncols    = 1,
        .flags    = VX_IDX_KIND_HASH | 4u /* unique */
    };
    vx_idx_file_t *hidx = NULL;
    st = vx_idx_build_from_seg("/tmp", seg, &idef0, &hidx);
    assert(st == VX_OK);
    assert(hidx != NULL);
    assert(hidx->file_hdr->kind == VX_IDX_KIND_HASH);
    assert(hidx->file_hdr->is_unique == 1);

    /* Lookup all rows in Hash Index */
    for (uint64_t r = 0; r < total_rows; r += 25) {
        uint8_t enc[8];
        vx_key_encode_u64(r, enc);
        size_t klen = 8;
        test_hit_ctx_t tc = {0};
        st = vx_idx_lookup_eq(hidx, NULL, enc, (uint32_t)klen, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 1);
        assert(tc.hits[0] == (uint32_t)r);
    }

    /* 3.2 Build Non-Unique ART Index on Column 2 (category = row % 10) */
    uint32_t col_ids2[1] = { 2 };
    vx_man_index_def_t idef2 = {
        .index_id = 2,
        .name     = (char *)"idx_cat",
        .col_ids  = col_ids2,
        .ncols    = 1,
        .flags    = VX_IDX_KIND_ART
    };
    vx_idx_file_t *nuidx = NULL;
    st = vx_idx_build_from_seg("/tmp", seg, &idef2, &nuidx);
    assert(st == VX_OK);
    assert(nuidx != NULL);
    assert(nuidx->file_hdr->kind == VX_IDX_KIND_ART);

    /* Query category 7: should return exactly 50 rows (7, 17, 27, ..., 497) */
    {
        uint8_t enc[8];
        vx_key_encode_u64(7, enc);
        size_t klen = 8;
        test_hit_ctx_t tc = {0};
        st = vx_idx_lookup_eq(nuidx, NULL, enc, (uint32_t)klen, test_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 50);
        for (uint32_t i = 0; i < tc.hit_count; ++i) {
            assert((tc.hits[i] % 10) == 7);
        }
    }

    vx_idx_close(hidx);
    vx_idx_close(nuidx);
    vx_owner_release(&seg->hdr);
    unlink(seg_path);
    unlink(hidx_path);
    unlink(nuidx_path);
}

/* ============================================================================
 * 4. Test Cost Optimizer, Access-Path Selection & Explain Formatter
 * ============================================================================ */
static void test_plan_optimizer(void) {
    printf("[4/5] Testing logical-to-physical cost optimizer & explain formatter...\n");

    /* Mock Catalog Setup */
    vx_opt_catalog_t cat;
    memset(&cat, 0, sizeof(cat));
    cat.ntables = 1;
    vx_opt_table_t tbl;
    memset(&tbl, 0, sizeof(tbl));
    strncpy(tbl.name, "users", sizeof(tbl.name) - 1);
    tbl.table_id   = 1;
    tbl.ncols      = 8;
    tbl.total_rows = 12288;
    tbl.nchunks    = 64;
    tbl.pk_col     = 0;
    tbl.nindexes   = 1;
    tbl.indexes[0].is_unique = true;
    tbl.indexes[0].ncols = 1;
    tbl.indexes[0].col_idxs[0] = 1; /* index on col 1 */

    vx_opt_table_t *tbl_ptr = &tbl;
    cat.tables = &tbl_ptr;

    /* Build AST Query:
     * users -> Filter(country == "IN") -> Project(id, email, score) ->
     * Join(orders) -> Sort(score DESC) -> Slice(limit=50)
     */
    vx_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.kind = VX_STMT_QUERY;
    stmt.u.query.explain = true;
    stmt.u.query.source_table = (vx_span_t){ .ptr = "users", .len = 5 };

    /* Construct stages */
    vx_stage_t stages[5];
    memset(stages, 0, sizeof(stages));

    /* Stage 0: Filter */
    stages[0].kind = VX_STAGE_FILTER;
    vx_expr_t lit_rhs = { .kind = VX_EXPR_STR, .u.str = { .ptr = "IN", .len = 2 } };
    vx_span_t part0 = { .ptr = "country", .len = 7 };
    vx_expr_t id_lhs  = { .kind = VX_EXPR_IDENT, .u.ident = { .parts = &part0, .nparts = 1 } };
    vx_expr_t filter_expr = {
        .kind = VX_EXPR_BINARY,
        .u.binary = { .op = VX_BINOP_EQ, .lhs = &id_lhs, .rhs = &lit_rhs }
    };
    stages[0].u.filter.pred = &filter_expr;

    /* Stage 1: Join */
    stages[1].kind = VX_STAGE_JOIN;
    stages[1].u.join.kind = VX_JOIN_INNER;
    stages[1].u.join.right_table = (vx_span_t){ .ptr = "orders", .len = 6 };

    /* Stage 2: Project */
    stages[2].kind = VX_STAGE_PROJECT;
    vx_proj_item_t proj_items[3] = {
        { .alias = { .ptr = "id", .len = 2 } },
        { .alias = { .ptr = "email", .len = 5 } },
        { .alias = { .ptr = "score", .len = 5 } }
    };
    stages[2].u.project.items  = proj_items;
    stages[2].u.project.nitems = 3;

    /* Stage 3: Sort */
    stages[3].kind = VX_STAGE_SORT;
    vx_span_t sort_part = { .ptr = "score", .len = 5 };
    vx_expr_t sort_expr = { .kind = VX_EXPR_IDENT, .u.ident = { .parts = &sort_part, .nparts = 1 } };
    vx_sort_item_t sort_items[1] = {
        { .expr = &sort_expr, .desc = true }
    };
    stages[3].u.sort.items  = sort_items;
    stages[3].u.sort.nitems = 1;

    /* Stage 4: Slice */
    stages[4].kind = VX_STAGE_SLICE;
    stages[4].u.slice.has_start = false;
    stages[4].u.slice.has_stop  = true;
    stages[4].u.slice.start     = 0;
    stages[4].u.slice.stop      = 50;

    stmt.u.query.stages  = stages;
    stmt.u.query.nstages = 5;

    vx_plan_result_t plan;
    vx_status_t st = vx_plan_compile(&stmt, &cat, &plan);
    assert(st == VX_OK);
    assert(plan.root != NULL);
    assert(plan.n_pipelines > 0);

    /* Verify Top-K fusion occurred */
    assert(strcmp(plan.root->op_name, "TopK") == 0);
    assert(strstr(plan.root->detail, "limit=50") != NULL);
    assert(strstr(plan.root->detail, "score DESC") != NULL);

    /* Verify child of TopK is Project */
    assert(plan.root->left != NULL);
    assert(strcmp(plan.root->left->op_name, "Project") == 0);

    /* Format explain plan */
    char explain_buf[2048];
    st = vx_plan_format_explain(plan.root, explain_buf, sizeof(explain_buf));
    assert(st == VX_OK);
    assert(strlen(explain_buf) > 0);

    /* Verify explain formatting contains expected keywords and box-drawing runes */
    assert(strstr(explain_buf, "TopK (limit=50, sort=score DESC)") != NULL);
    assert(strstr(explain_buf, "└─ Project") != NULL);
    assert(strstr(explain_buf, "HashJoin") != NULL);
    assert(strstr(explain_buf, "TableScan") != NULL);

    printf("Generated Explain Plan Output:\n%s\n", explain_buf);

    vx_plan_destroy(&plan);

    /* Test PK access path selection */
    {
        vx_stage_t pk_stage;
        memset(&pk_stage, 0, sizeof(pk_stage));
        pk_stage.kind = VX_STAGE_PK;
        vx_expr_t pk_key = { .kind = VX_EXPR_INT, .u.i64 = 42 };
        pk_stage.u.pk.key = &pk_key;

        vx_stmt_t pk_stmt = stmt;
        pk_stmt.u.query.stages  = &pk_stage;
        pk_stmt.u.query.nstages = 1;

        vx_plan_result_t pk_plan;
        st = vx_plan_compile(&pk_stmt, &cat, &pk_plan);
        assert(st == VX_OK);
        assert(pk_plan.root != NULL);
        assert(pk_plan.root->access.path == VX_ACCESS_PK_POINT);
        assert(pk_plan.root->est_rows == 1);
        vx_plan_destroy(&pk_plan);
    }
}

/* ============================================================================
 * 5. Test MANIFEST Index Orphan Sweeping
 * ============================================================================ */
static void test_manifest_idx_orphan_sweeping(void) {
    printf("[5/5] Testing MANIFEST companion .idx orphan sweeping on recover...\n");

    const char *test_dir = "/tmp/test_v2_orphan_idx";
    int sys_rc = system("rm -rf /tmp/test_v2_orphan_idx");
    (void)sys_rc;
    int rc = mkdir(test_dir, 0755);
    assert(rc == 0);

    /* Create dummy files */
    char path_valid[256], path_orphan_seg[256], path_orphan_idx[256], path_tmp[256];
    snprintf(path_valid, sizeof(path_valid), "%s/seg_100_i1.idx", test_dir);
    snprintf(path_orphan_seg, sizeof(path_orphan_seg), "%s/seg_999_i1.idx", test_dir);
    snprintf(path_orphan_idx, sizeof(path_orphan_idx), "%s/seg_100_i99.idx", test_dir);
    snprintf(path_tmp, sizeof(path_tmp), "%s/seg_100_i1.idx.tmp", test_dir);

    int fd;
    fd = open(path_valid, O_CREAT | O_WRONLY, 0644); close(fd);
    fd = open(path_orphan_seg, O_CREAT | O_WRONLY, 0644); close(fd);
    fd = open(path_orphan_idx, O_CREAT | O_WRONLY, 0644); close(fd);
    fd = open(path_tmp, O_CREAT | O_WRONLY, 0644); close(fd);

    /* Construct Manifest having segment 100 with table having index 1 */
    vx_manifest_t man;
    memset(&man, 0, sizeof(man));
    man.nsegments = 1;
    vx_man_seg_t seg_meta;
    memset(&seg_meta, 0, sizeof(seg_meta));
    seg_meta.segment_id = 100;
    seg_meta.table_id   = 1;
    man.segments = &seg_meta;

    man.ntables = 1;
    vx_man_table_def_t tdef;
    memset(&tdef, 0, sizeof(tdef));
    tdef.table_id = 1;
    tdef.n_indexes = 1;
    vx_man_index_def_t idef;
    memset(&idef, 0, sizeof(idef));
    idef.index_id = 1;
    tdef.indexes = &idef;
    man.tables = &tdef;

    uint32_t removed = 0;
    vx_status_t st = vx_manifest_sweep_orphans(test_dir, &man, &removed);
    assert(st == VX_OK);
    assert(removed == 3); /* orphan_seg, orphan_idx, and idx.tmp */

    /* Verify path_valid still exists */
    assert(access(path_valid, F_OK) == 0);
    /* Verify orphans were removed */
    assert(access(path_orphan_seg, F_OK) != 0);
    assert(access(path_orphan_idx, F_OK) != 0);
    assert(access(path_tmp, F_OK) != 0);

    unlink(path_valid);
    rmdir(test_dir);
}

int main(void) {
    printf("=== Starting Milestone M16: Secondary Index & Plan Test Suite ===\n");
    test_key_codecs();
    test_art_point_and_range_scans();
    test_hash_index_and_posting_lists();
    test_plan_optimizer();
    test_manifest_idx_orphan_sweeping();
    printf("=== ALL M16 TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
