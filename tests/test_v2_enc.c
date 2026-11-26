#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/vx_enc.h"
#include "src/vx_op_filter.h"
#include "src/vx_seg_fmt.h"
#include "src/vx_simd.h"

static void dummy_owner_destroy(vx_owner_t *o) {
    (void)o;
}

/* ============================================================================
 * 1. Test Frame-of-Reference + Bitpacking (VX_ENC_FOR_BP)
 * ============================================================================ */
static void test_enc_for_bp(void) {
    printf("[1/6] Testing VX_ENC_FOR_BP encoding, decoding & zero-UB boundaries...\n");

    vx_arena_t *scratch = vx_arena_create(65536, 1048576, 0);
    assert(scratch != NULL);

    uint8_t enc_buf[65536];
    size_t  enc_len = 0;

    /* Case A: Constant column (bit_width == 0) */
    {
        int64_t vals[100];
        for (int i = 0; i < 100; ++i) vals[i] = 42;

        vx_status_t st = vx_enc_for_bp_encode(vals, VX_TYPE_I64, NULL, 100,
                                              enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        assert(enc_len == sizeof(vx_for_hdr_t));

        vx_for_hdr_t *hdr = (vx_for_hdr_t *)enc_buf;
        assert(hdr->bit_width == 0);
        assert(hdr->base == 42);
        assert(hdr->nrows == 100);

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = NULL,
            .type      = VX_TYPE_I64,
            .encoding  = VX_ENC_FOR_BP,
            .nrows     = 100
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        assert(out.kind == VX_VEC_FLAT);
        assert(out.phys_len == 100);
        const int64_t *dec = (const int64_t *)out.data;
        for (int i = 0; i < 100; ++i) {
            assert(dec[i] == 42);
        }
        vx_owner_release(out.owner);
    }

    /* Case B: Small deltas (5-bit width: deltas 0..30) */
    {
        uint64_t vals[200];
        for (uint64_t i = 0; i < 200; ++i) vals[i] = 1000ULL + (i % 31ULL);

        vx_status_t st = vx_enc_for_bp_encode(vals, VX_TYPE_U64, NULL, 200,
                                              enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        vx_for_hdr_t *hdr = (vx_for_hdr_t *)enc_buf;
        assert(hdr->bit_width == 5);
        assert(hdr->base == 1000ULL);
        assert(hdr->nrows == 200);

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = NULL,
            .type      = VX_TYPE_U64,
            .encoding  = VX_ENC_FOR_BP,
            .nrows     = 200
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        const uint64_t *dec = (const uint64_t *)out.data;
        for (uint64_t i = 0; i < 200; ++i) {
            assert(dec[i] == vals[i]);
        }
        vx_owner_release(out.owner);
    }

    /* Case C: Negative i64 values across INT64_MIN and INT64_MAX */
    {
        int64_t vals[4] = { INT64_MIN, -100, 0, INT64_MAX };
        vx_status_t st = vx_enc_for_bp_encode(vals, VX_TYPE_I64, NULL, 4,
                                              enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        vx_for_hdr_t *hdr = (vx_for_hdr_t *)enc_buf;
        assert(hdr->bit_width == 64); /* full span */
        assert(hdr->base == (uint64_t)INT64_MIN);

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = NULL,
            .type      = VX_TYPE_I64,
            .encoding  = VX_ENC_FOR_BP,
            .nrows     = 4
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        const int64_t *dec = (const int64_t *)out.data;
        for (int i = 0; i < 4; ++i) {
            assert(dec[i] == vals[i]);
        }
        vx_owner_release(out.owner);
    }

    /* Case D: Values with NULLs */
    {
        int64_t vals[64];
        uint64_t validity = 0ULL;
        for (int i = 0; i < 64; ++i) {
            if (i % 2 == 0) {
                vals[i] = 500 + i;
                validity |= (1ULL << i); /* even rows valid */
            } else {
                vals[i] = -999999; /* dirty null slot; should be ignored in base calculation */
            }
        }

        vx_status_t st = vx_enc_for_bp_encode(vals, VX_TYPE_I64, &validity, 64,
                                              enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        vx_for_hdr_t *hdr = (vx_for_hdr_t *)enc_buf;
        assert(hdr->base == 500); /* minimum valid value, dirty null ignored! */

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = &validity,
            .type      = VX_TYPE_I64,
            .encoding  = VX_ENC_FOR_BP,
            .has_nulls = 1,
            .nrows     = 64
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        assert(out.has_nulls == 1);
        assert(out.validity == &validity);
        const int64_t *dec = (const int64_t *)out.data;
        for (int i = 0; i < 64; ++i) {
            if (i % 2 == 0) {
                assert(dec[i] == vals[i]);
            }
        }
        vx_owner_release(out.owner);
    }

    vx_owner_release(&scratch->hdr);
    printf("      -> VX_ENC_FOR_BP PASSED.\n");
}

/* ============================================================================
 * 2. Test Dictionary Encoding (VX_ENC_DICT & vx_dict_builder)
 * ============================================================================ */
static void test_enc_dict(void) {
    printf("[2/6] Testing String Dictionary Builder & VX_ENC_DICT...\n");

    vx_arena_t *scratch = vx_arena_create(65536, 1048576, 0);
    assert(scratch != NULL);

    /* Test dict_builder intern */
    vx_dict_builder_t db;
    assert(vx_dict_builder_init(&db, 10) == VX_OK);

    vx_strpool_t pool;
    assert(vx_strpool_init(&pool, 16) == VX_OK);

    vx_strview_t sv_apple  = vx_strview_inline((const uint8_t *)"apple", 5);
    vx_strview_t sv_banana = vx_strview_inline((const uint8_t *)"banana", 6);
    vx_strview_t sv_cherry = vx_strview_inline((const uint8_t *)"cherry", 6);
    /* Out-of-line string > 12 chars */
    vx_strview_t sv_ool;
    const char *long_str = "supercalifragilisticexpialidocious";
    assert(vx_strpool_append(&pool, (const uint8_t *)long_str, (int32_t)strlen(long_str), &sv_ool) == VX_OK);

    uint32_t code_apple = 99, code_banana = 99, code_cherry = 99, code_ool = 99;
    assert(vx_dict_builder_intern(&db, sv_apple, &pool, &code_apple) == VX_OK);
    assert(vx_dict_builder_intern(&db, sv_banana, &pool, &code_banana) == VX_OK);
    assert(vx_dict_builder_intern(&db, sv_cherry, &pool, &code_cherry) == VX_OK);
    assert(vx_dict_builder_intern(&db, sv_ool, &pool, &code_ool) == VX_OK);

    assert(code_apple == 0);
    assert(code_banana == 1);
    assert(code_cherry == 2);
    assert(code_ool == 3);

    /* Idempotent re-intern returns identical codes */
    uint32_t c = 99;
    assert(vx_dict_builder_intern(&db, sv_banana, &pool, &c) == VX_OK);
    assert(c == code_banana);
    assert(vx_dict_builder_intern(&db, sv_ool, &pool, &c) == VX_OK);
    assert(c == code_ool);

    /* Test encode codes */
    uint32_t codes[100];
    for (int i = 0; i < 100; ++i) {
        codes[i] = (uint32_t)(i % 4);
    }

    uint8_t enc_buf[4096];
    size_t  enc_len = 0;
    vx_status_t st = vx_enc_dict_encode(codes, 100, db.count, enc_buf, sizeof(enc_buf), &enc_len);
    assert(st == VX_OK);

    vx_dict_hdr_t *hdr = (vx_dict_hdr_t *)enc_buf;
    assert(hdr->nrows == 100);
    assert(hdr->bit_width == 2); /* 4 entries -> 2 bits */

    /* Test decode: direct binding as VX_VEC_DICT */
    vx_enc_col_ctx_t ec = {
        .page_data  = enc_buf,
        .page_len   = enc_len,
        .validity   = NULL,
        .dict_vals  = db.vals,
        .dict_count = db.count,
        .strpool    = &pool,
        .type       = VX_TYPE_STR,
        .encoding   = VX_ENC_DICT,
        .nrows      = 100
    };
    vx_vec_t out;
    st = vx_enc_decode_col_chunk(&ec, scratch, &out);
    assert(st == VX_OK);
    assert(out.kind == VX_VEC_DICT);
    assert(out.dict_size == 4);
    assert(out.dict_vals == db.vals);
    assert(out.phys_len == 100);

    const uint32_t *dec_codes = (const uint32_t *)out.data;
    for (int i = 0; i < 100; ++i) {
        assert(dec_codes[i] == codes[i]);
    }
    vx_owner_release(out.owner);

    /* Test corrupt dictionary code detection */
    uint8_t corrupt_buf[4096];
    memcpy(corrupt_buf, enc_buf, enc_len);
    /* Lower dict_count to 3 so codes with value 3 trigger corruption guard */
    ec.page_data = corrupt_buf;
    ec.dict_count = 3;
    st = vx_enc_decode_col_chunk(&ec, scratch, &out);
    assert(st == VX_ERR_CORRUPT);

    /* Test max_dict cardinality guard */
    for (int i = 4; i < 10; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "str_%d", i);
        vx_strview_t sv = vx_strview_inline((const uint8_t *)buf, (int32_t)strlen(buf));
        assert(vx_dict_builder_intern(&db, sv, &pool, &c) == VX_OK);
    }
    assert(db.count == 10);
    /* 11th distinct string exceeds max_dict (10) */
    vx_strview_t sv_overflow = vx_strview_inline((const uint8_t *)"overflow", 8);
    st = vx_dict_builder_intern(&db, sv_overflow, &pool, &c);
    assert(st == VX_ERR_INVALID);

    vx_dict_builder_destroy(&db);
    vx_strpool_destroy(&pool);
    vx_owner_release(&scratch->hdr);
    printf("      -> String Dictionary Builder & VX_ENC_DICT PASSED.\n");
}

/* ============================================================================
 * 3. Test Run-Length Encoding (VX_ENC_RLE)
 * ============================================================================ */
static void test_enc_rle(void) {
    printf("[3/6] Testing VX_ENC_RLE for Bool, Numerics, String & NaN/-0.0...\n");

    vx_arena_t *scratch = vx_arena_create(65536, 1048576, 0);
    assert(scratch != NULL);

    uint8_t enc_buf[65536];
    size_t  enc_len = 0;

    /* Case A: BOOL runs */
    {
        uint8_t vals[100];
        for (int i = 0; i < 50; ++i) vals[i] = 1;
        for (int i = 50; i < 100; ++i) vals[i] = 0;

        vx_status_t st = vx_enc_rle_encode(vals, VX_TYPE_BOOL, NULL, NULL, 100,
                                           enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        vx_rle_hdr_t *hdr = (vx_rle_hdr_t *)enc_buf;
        assert(hdr->nrows == 100);
        assert(hdr->nruns == 2);
        assert(hdr->type == VX_TYPE_BOOL);

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = NULL,
            .type      = VX_TYPE_BOOL,
            .encoding  = VX_ENC_RLE,
            .nrows     = 100
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        assert(out.kind == VX_VEC_FLAT);
        assert(out.phys_len == 100);
        const uint8_t *dec = (const uint8_t *)out.data;
        for (int i = 0; i < 100; ++i) {
            assert(dec[i] == vals[i]);
        }
        vx_owner_release(out.owner);
    }

    /* Case B: F64 with NaN and -0.0 bit-exact preservation */
    {
        double vals[6];
        vals[0] = -0.0;
        vals[1] = -0.0;
        vals[2] = +0.0;  /* distinct run from -0.0 */
        vals[3] = +0.0;
        vals[4] = NAN;
        vals[5] = NAN;

        vx_status_t st = vx_enc_rle_encode(vals, VX_TYPE_F64, NULL, NULL, 6,
                                           enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        vx_rle_hdr_t *hdr = (vx_rle_hdr_t *)enc_buf;
        assert(hdr->nruns == 3); /* -0.0, +0.0, NaN are distinct bit patterns! */

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = NULL,
            .type      = VX_TYPE_F64,
            .encoding  = VX_ENC_RLE,
            .nrows     = 6
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        const double *dec = (const double *)out.data;
        assert(signbit(dec[0]) != 0); /* -0.0 */
        assert(signbit(dec[1]) != 0);
        assert(signbit(dec[2]) == 0); /* +0.0 */
        assert(signbit(dec[3]) == 0);
        assert(isnan(dec[4]));        /* NaN */
        assert(isnan(dec[5]));
        vx_owner_release(out.owner);
    }

    /* Case C: String runs with NULL extension */
    {
        vx_strview_t vals[6];
        vals[0] = vx_strview_inline((const uint8_t *)"hello", 5);
        vals[1] = vx_strview_inline((const uint8_t *)"hello", 5);
        vals[2] = vx_strview_inline((const uint8_t *)"", 0); /* will be null */
        vals[3] = vx_strview_inline((const uint8_t *)"world", 5);
        vals[4] = vx_strview_inline((const uint8_t *)"world", 5);
        vals[5] = vx_strview_inline((const uint8_t *)"world", 5);

        uint64_t val_bits = 0x3B; /* binary 111011: row 2 is NULL */

        vx_status_t st = vx_enc_rle_encode(vals, VX_TYPE_STR, &val_bits, NULL, 6,
                                           enc_buf, sizeof(enc_buf), &enc_len);
        assert(st == VX_OK);
        vx_rle_hdr_t *hdr = (vx_rle_hdr_t *)enc_buf;
        /* Row 2 (NULL) extends the "hello" run without breaking it -> exactly 2 runs! */
        assert(hdr->nruns == 2);

        vx_enc_col_ctx_t ec = {
            .page_data = enc_buf,
            .page_len  = enc_len,
            .validity  = &val_bits,
            .type      = VX_TYPE_STR,
            .encoding  = VX_ENC_RLE,
            .has_nulls = 1,
            .nrows     = 6
        };
        vx_vec_t out;
        st = vx_enc_decode_col_chunk(&ec, scratch, &out);
        assert(st == VX_OK);
        assert(out.has_nulls == 1);
        const vx_strview_t *dec = (const vx_strview_t *)out.data;
        assert(dec[0].inl.len == 5 && memcmp(dec[0].inl.data, "hello", 5) == 0);
        assert(dec[1].inl.len == 5 && memcmp(dec[1].inl.data, "hello", 5) == 0);
        /* Row 2 has validity bit 0 */
        assert(((out.validity[0] >> 2) & 1ULL) == 0ULL);
        assert(dec[3].inl.len == 5 && memcmp(dec[3].inl.data, "world", 5) == 0);
        assert(dec[4].inl.len == 5 && memcmp(dec[4].inl.data, "world", 5) == 0);
        assert(dec[5].inl.len == 5 && memcmp(dec[5].inl.data, "world", 5) == 0);
        vx_owner_release(out.owner);
    }

    vx_owner_release(&scratch->hdr);
    printf("      -> VX_ENC_RLE PASSED.\n");
}

/* ============================================================================
 * 4. Test Dictionary Predicate Pushdown in Filter Operator
 * ============================================================================ */
static void test_enc_filter_dict_pushdown(void) {
    printf("[4/6] Testing Dictionary Predicate Pushdown in vx_op_filter...\n");

    vx_arena_t *scratch = vx_arena_create(65536, 1048576, 0);
    assert(scratch != NULL);

    /* Setup dictionary: ["apple", "banana", "cherry", "date"] */
    vx_strview_t dict[4] = {
        vx_strview_inline((const uint8_t *)"apple", 5),
        vx_strview_inline((const uint8_t *)"banana", 6),
        vx_strview_inline((const uint8_t *)"cherry", 6),
        vx_strview_inline((const uint8_t *)"date", 4)
    };

    /* Codes: [0, 1, 2, 3, 1, 0, 1, 2] -> 8 rows */
    uint32_t codes[8] = { 0, 1, 2, 3, 1, 0, 1, 2 };

    vx_type_t col_types[1] = { VX_TYPE_STR };
    vx_batch_t in_batch;
    vx_batch_init(&in_batch, 1, col_types);
    in_batch.sel.count = 8;
    in_batch.sel.is_identity = true;

    vx_vec_t *col = &in_batch.cols[0];
    col->type      = VX_TYPE_STR;
    col->kind      = VX_VEC_DICT;
    col->has_nulls = 0;
    col->phys_len  = 8;
    col->dict_size = 4;
    col->data      = codes;
    col->dict_vals = dict;
    col->strpool   = NULL;
    col->owner     = NULL;

    /* Filter predicate: col == "banana" (code 1; rows 1, 4, 6) */
    vx_filter_pred_t pred = {
        .kind          = VX_PRED_COL_LIT,
        .type          = VX_TYPE_STR,
        .cmp           = VX_CMP_EQ,
        .negate        = false,
        .batch_col_idx = 0,
        .table_col_idx = 0,
        .lit = {
            .str = { .ptr = (const uint8_t *)"banana", .len = 6 }
        }
    };
    vx_filter_cfg_t cfg = {
        .preds  = &pred,
        .npreds = 1
    };
    vx_op_t op = {
        .vtbl     = &VX_OP_FILTER,
        .plan_cfg = &cfg
    };

    const vx_simd_ops_t *simd = vx_simd_detect();

    vx_exec_ctx_t ctx = {
        .simd    = simd,
        .scratch = scratch,
        .err     = NULL
    };

    vx_batch_t out_batch;
    vx_batch_init(&out_batch, 1, col_types);

    vx_op_res_t res = VX_OP_FILTER.execute(&op, &ctx, NULL, NULL, &in_batch, &out_batch);
    assert(res == VX_OP_NEED_MORE_INPUT);
    assert(out_batch.sel.count == 3);
    assert(out_batch.sel.idx[0] == 1);
    assert(out_batch.sel.idx[1] == 4);
    assert(out_batch.sel.idx[2] == 6);

    /* Test negated filter: col != "banana" */
    pred.negate = true;
    in_batch.sel.count = 8;
    in_batch.sel.is_identity = true;
    vx_batch_reset(&out_batch);

    res = VX_OP_FILTER.execute(&op, &ctx, NULL, NULL, &in_batch, &out_batch);
    assert(res == VX_OP_NEED_MORE_INPUT);
    assert(out_batch.sel.count == 5); /* rows 0, 2, 3, 5, 7 */
    assert(out_batch.sel.idx[0] == 0);
    assert(out_batch.sel.idx[1] == 2);
    assert(out_batch.sel.idx[2] == 3);
    assert(out_batch.sel.idx[3] == 5);
    assert(out_batch.sel.idx[4] == 7);

    vx_batch_destroy(&in_batch);
    vx_batch_destroy(&out_batch);
    vx_owner_release(&scratch->hdr);
    printf("      -> Dictionary Predicate Pushdown PASSED.\n");
}

/* TableScan configuration matching src/vx_op_scan.c */
typedef struct vx_scan_cfg {
    vx_owner_t               *catalog_owner;
    const vx_chunk_t  *const *chunks;
    const vx_delmap_t *const *delmaps;
    uint32_t                  nchunks;
    uint32_t                  out_ncols;
    const uint32_t           *col_ids;
    const uint8_t            *eager_cols;
    const void               *zone_preds;
    uint32_t                  n_zone_preds;
} vx_scan_cfg_t;
extern const vx_source_vtbl_t VX_SOURCE_TABLE_SCAN;

/* ============================================================================
 * 5. Test vx_bind_col_chunk Integration with Cold Segment
 * ============================================================================ */
static void test_enc_scan_binding(void) {
    printf("[5/6] Testing vx_bind_col_chunk mmap segment decoding...\n");

    vx_arena_t *scratch = vx_arena_create(65536, 1048576, 0);
    assert(scratch != NULL);

    /* Encode a FOR_BP page with 50 rows */
    uint64_t vals[50];
    for (uint64_t i = 0; i < 50; ++i) vals[i] = 200ULL + i;

    uint8_t enc_buf[4096];
    size_t  enc_len = 0;
    assert(vx_enc_for_bp_encode(vals, VX_TYPE_U64, NULL, 50, enc_buf, sizeof(enc_buf), &enc_len) == VX_OK);

    /* Construct mock segment layout */
    vx_seg_col_desc_t col_desc;
    memset(&col_desc, 0, sizeof(col_desc));
    col_desc.col_id   = 0;
    col_desc.type     = (uint8_t)VX_TYPE_U64;
    col_desc.encoding = (uint8_t)VX_ENC_FOR_BP;
    col_desc.chunk_meta_off = 0;

    vx_seg_chunk_meta_t chunk_meta;
    memset(&chunk_meta, 0, sizeof(chunk_meta));
    chunk_meta.page_off = 0;
    chunk_meta.page_len = (uint32_t)enc_len;

    vx_seg_t seg;
    memset(&seg, 0, sizeof(seg));
    vx_owner_init(&seg.hdr, VX_OWN_SEGMENT, dummy_owner_destroy);
    seg.col_dir   = &col_desc;
    seg.mmap_base = (const uint8_t *)&chunk_meta; /* chunk_meta_off = 0 -> mmap_base + 0 */

    /* Mock chunk */
    vx_col_chunk_t col_chunk;
    memset(&col_chunk, 0, sizeof(col_chunk));
    col_chunk.type      = VX_TYPE_U64;
    col_chunk.encoding  = VX_ENC_FOR_BP;
    col_chunk.has_nulls = 0;
    col_chunk.data      = enc_buf;

    vx_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    vx_owner_init(&chunk.hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    chunk.chunk_id = 0;
    chunk.nrows    = 50;
    chunk.ncols    = 1;
    chunk.backing  = &seg.hdr;
    chunk.cols     = &col_chunk;

    /* Execute late materialization fetch */
    vx_type_t u64_types[1] = { VX_TYPE_U64 };
    vx_batch_t batch;
    vx_batch_init(&batch, 1, u64_types);
    batch.sel.count = 50;
    batch.sel.is_identity = true;
    batch.cols[0].kind = VX_VEC_DEFERRED;

    /* Setup row_src with chunk */
    batch.row_src.src_ctx    = &chunk;
    batch.row_src.src_owner  = &chunk.hdr;
    batch.row_src.fetch_col  = NULL; /* vx_batch_materialize_col uses its built-in or fetch_col */

    /* Call vx_enc_decode_col_chunk directly with chunk context */
    vx_enc_col_ctx_t ec = {
        .page_data = col_chunk.data,
        .page_len  = chunk_meta.page_len,
        .validity  = NULL,
        .type      = col_chunk.type,
        .encoding  = col_chunk.encoding,
        .nrows     = chunk.nrows
    };
    vx_vec_t out_vec;
    assert(vx_enc_decode_col_chunk(&ec, scratch, &out_vec) == VX_OK);
    assert(out_vec.kind == VX_VEC_FLAT);
    assert(out_vec.phys_len == 50);
    const uint64_t *dec = (const uint64_t *)out_vec.data;
    for (int i = 0; i < 50; ++i) {
        assert(dec[i] == vals[i]);
    }
    vx_owner_release(out_vec.owner);
    vx_batch_destroy(&batch);

    /* Test VX_SOURCE_TABLE_SCAN directly with this encoded cold chunk */
    const vx_chunk_t *chunks[1] = { &chunk };
    uint32_t col_ids[1] = { 0 };
    uint8_t eager_cols[1] = { 1 };

    vx_scan_cfg_t scan_cfg = {
        .catalog_owner = NULL,
        .chunks        = chunks,
        .delmaps       = NULL,
        .nchunks       = 1,
        .out_ncols     = 1,
        .col_ids       = col_ids,
        .eager_cols    = eager_cols,
        .zone_preds    = NULL,
        .n_zone_preds  = 0
    };
    vx_source_t src = {
        .vtbl     = &VX_SOURCE_TABLE_SCAN,
        .plan_cfg = &scan_cfg
    };

    atomic_bool cancelled;
    atomic_init(&cancelled, false);
    vx_exec_ctx_t scan_ctx = {
        .scratch   = scratch,
        .cancelled = &cancelled,
        .err       = NULL
    };

    void *gstate = NULL;
    assert(VX_SOURCE_TABLE_SCAN.init_global(&src, &scan_ctx, &gstate) == VX_OK);

    vx_batch_t scan_batch;
    vx_batch_init(&scan_batch, 1, u64_types);

    vx_op_res_t sres = VX_SOURCE_TABLE_SCAN.next_batch(&src, &scan_ctx, gstate, NULL, &scan_batch);
    assert(sres == VX_OP_HAVE_MORE_OUTPUT);
    assert(scan_batch.cols[0].kind == VX_VEC_FLAT);
    assert(scan_batch.sel.count == 50);
    const uint64_t *sdec = (const uint64_t *)scan_batch.cols[0].data;
    for (int i = 0; i < 50; ++i) {
        assert(sdec[i] == vals[i]);
    }

    /* Second call exhausts the source */
    vx_batch_reset(&scan_batch);
    sres = VX_SOURCE_TABLE_SCAN.next_batch(&src, &scan_ctx, gstate, NULL, &scan_batch);
    assert(sres == VX_OP_FINISHED);

    vx_batch_destroy(&scan_batch);
    VX_SOURCE_TABLE_SCAN.destroy_global(gstate);

    vx_owner_release(&scratch->hdr);
    printf("      -> vx_bind_col_chunk decoding PASSED.\n");
}

/* ============================================================================
 * 6. Test Fuzz & Corruption Guards
 * ============================================================================ */
static void test_enc_corruption_fuzz(void) {
    printf("[6/6] Testing Fuzz & Corruption rejection guards...\n");

    vx_arena_t *scratch = vx_arena_create(65536, 1048576, 0);
    assert(scratch != NULL);

    vx_vec_t out;

    /* Truncated header (< sizeof(vx_for_hdr_t)) */
    uint8_t garbage[8] = { 0 };
    vx_enc_col_ctx_t ec = {
        .page_data = garbage,
        .page_len  = sizeof(garbage),
        .type      = VX_TYPE_I64,
        .encoding  = VX_ENC_FOR_BP,
        .nrows     = 10
    };
    assert(vx_enc_decode_col_chunk(&ec, scratch, &out) == VX_ERR_CORRUPT);

    /* FOR_BP bit_width > 64 */
    vx_for_hdr_t bad_for = { .base = 0, .nrows = 10, .bit_width = 65 };
    ec.page_data = &bad_for;
    ec.page_len  = sizeof(bad_for) + 64;
    assert(vx_enc_decode_col_chunk(&ec, scratch, &out) == VX_ERR_CORRUPT);

    /* DICT bit_width > 32 */
    vx_dict_hdr_t bad_dict = { .nrows = 10, .bit_width = 33 };
    ec.page_data  = &bad_dict;
    ec.page_len   = sizeof(bad_dict) + 64;
    ec.encoding   = VX_ENC_DICT;
    ec.dict_vals  = garbage;
    ec.dict_count = 10;
    assert(vx_enc_decode_col_chunk(&ec, scratch, &out) == VX_ERR_CORRUPT);

    /* RLE nruns > nrows */
    vx_rle_hdr_t bad_rle = { .nrows = 10, .nruns = 20, .type = VX_TYPE_I64 };
    ec.page_data = &bad_rle;
    ec.page_len  = sizeof(bad_rle) + 200;
    ec.encoding  = VX_ENC_RLE;
    assert(vx_enc_decode_col_chunk(&ec, scratch, &out) == VX_ERR_CORRUPT);

    vx_owner_release(&scratch->hdr);
    printf("      -> Fuzz & Corruption guards PASSED.\n");
}

int main(void) {
    printf("=== V2 Columnar Encodings (FoR, Dict, RLE) Verification ===\n");
    test_enc_for_bp();
    test_enc_dict();
    test_enc_rle();
    test_enc_filter_dict_pushdown();
    test_enc_scan_binding();
    test_enc_corruption_fuzz();
    printf("All 6 Columnar Encoding tests passed successfully!\n");
    return 0;
}
