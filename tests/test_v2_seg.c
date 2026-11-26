#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/vx_types.h"
#include "src/vx_owner.h"
#include "src/vx_chunk.h"
#include "src/vx_seg_fmt.h"
#include "src/vx_seg.h"
#include "src/vx_str.h"
#include "src/vx_op.h"

static void dummy_owner_destroy(vx_owner_t *o) {
    (void)o;
}

/* Helper to free a mock test chunk */
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

/* 1. Test basic write, open, zero-deserialization views, string spooling, and PK lookup */
static void test_basic_types_write_read(void) {
    printf("[1/7] Testing basic write & zero-deserialization open with StringView...\n");

    const char *seg_path = "/tmp/test_v2_basic.vx";
    (void)unlink(seg_path);

    /* Schema: col 0 = U64 (PK), col 1 = I64, col 2 = F64, col 3 = BOOL, col 4 = STR */
    vx_type_t col_types[5] = {
        VX_TYPE_U64, VX_TYPE_I64, VX_TYPE_F64, VX_TYPE_BOOL, VX_TYPE_STR
    };
    const uint32_t ncols = 5;

    /* Build 2 chunks: chunk 0 has 3000 rows, chunk 1 has 2000 rows (total 5000 rows > 4096) */
    const uint32_t ch0_rows = 3000;
    const uint32_t ch1_rows = 2000;
    const uint32_t total_expected = ch0_rows + ch1_rows;

    vx_chunk_t *ch0 = (vx_chunk_t *)calloc(1, sizeof(*ch0));
    vx_chunk_t *ch1 = (vx_chunk_t *)calloc(1, sizeof(*ch1));
    vx_owner_init(&ch0->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    vx_owner_init(&ch1->hdr, VX_OWN_CHUNK, dummy_owner_destroy);

    ch0->nrows = ch0_rows; ch0->ncols = ncols; ch0->cols = (vx_col_chunk_t *)calloc(ncols, sizeof(vx_col_chunk_t));
    ch1->nrows = ch1_rows; ch1->ncols = ncols; ch1->cols = (vx_col_chunk_t *)calloc(ncols, sizeof(vx_col_chunk_t));

    /* Setup string pool for out-of-line strings in hot chunk */
    const char *long_sample = "Supercalifragilisticexpialidocious!"; /* 35 bytes > 12 */
    size_t long_len = strlen(long_sample);

    for (int ci = 0; ci < 2; ++ci) {
        vx_chunk_t *ch = (ci == 0) ? ch0 : ch1;
        uint32_t base_idx = (ci == 0) ? 0 : ch0_rows;
        uint32_t nr = ch->nrows;

        /* Col 0: U64 PK */
        ch->cols[0].type = VX_TYPE_U64;
        uint64_t *pks = (uint64_t *)malloc(nr * sizeof(uint64_t));
        for (uint32_t r = 0; r < nr; ++r) pks[r] = (uint64_t)(base_idx + r + 100);
        ch->cols[0].data = pks;

        /* Col 1: I64 with some NULLs */
        ch->cols[1].type = VX_TYPE_I64;
        ch->cols[1].has_nulls = 1;
        int64_t *i64s = (int64_t *)malloc(nr * sizeof(int64_t));
        uint64_t *val1 = (uint64_t *)calloc(VX_BITMAP_WORDS, sizeof(uint64_t));
        for (uint32_t r = 0; r < nr; ++r) {
            if ((r % 7) != 0) {
                val1[r >> 6] |= (1ULL << (r & 63u));
                i64s[r] = (int64_t)(r * 10);
            } else {
                i64s[r] = 0;
            }
        }
        ch->cols[1].data = i64s;
        ch->cols[1].validity = val1;

        /* Col 2: F64 */
        ch->cols[2].type = VX_TYPE_F64;
        double *f64s = (double *)malloc(nr * sizeof(double));
        for (uint32_t r = 0; r < nr; ++r) f64s[r] = (double)r * 1.5;
        ch->cols[2].data = f64s;

        /* Col 3: BOOL */
        ch->cols[3].type = VX_TYPE_BOOL;
        uint8_t *bools = (uint8_t *)malloc(nr * sizeof(uint8_t));
        for (uint32_t r = 0; r < nr; ++r) bools[r] = (uint8_t)(r & 1);
        ch->cols[3].data = bools;

        /* Col 4: STR (alternate inline and out-of-line) */
        ch->cols[4].type = VX_TYPE_STR;
        vx_strpool_t *pool = (vx_strpool_t *)calloc(1, sizeof(*pool));
        pool->nbufs = 1;
        pool->cap = 1;
        pool->ptrs = (const uint8_t **)calloc(1, sizeof(uint8_t *));
        pool->lens = (int64_t *)calloc(1, sizeof(int64_t));
        pool->ptrs[0] = (const uint8_t *)long_sample;
        pool->lens[0] = (int64_t)long_len;
        ch->cols[4].strpool = pool;

        vx_strview_t *svs = (vx_strview_t *)malloc(nr * sizeof(vx_strview_t));
        for (uint32_t r = 0; r < nr; ++r) {
            if ((r & 1) == 0) {
                /* Inline string */
                memset(&svs[r], 0, sizeof(svs[r]));
                svs[r].inl.len = 5;
                memcpy(svs[r].inl.data, "quick", 5);
            } else {
                /* Out-of-line string */
                memset(&svs[r], 0, sizeof(svs[r]));
                svs[r].out.len = (int32_t)long_len;
                memcpy(svs[r].out.prefix, long_sample, 4);
                svs[r].out.buf_idx = 0;
                svs[r].out.offset = 0;
            }
        }
        ch->cols[4].data = svs;
    }

    const vx_chunk_t *chunks[2] = { ch0, ch1 };
    vx_seg_write_cfg_t cfg = {
        .table_id = 42,
        .segment_id = 1001,
        .lsn_max = 5555,
        .ncols = ncols,
        .pk_col_idx = 0,
        .col_types = col_types,
        .chunks = chunks,
        .delmaps = NULL,
        .nchunks = 2,
        .spill_dir = "/tmp"
    };

    vx_seg_write_info_t info;
    memset(&info, 0, sizeof(info));
    vx_status_t st = vx_seg_write(seg_path, &cfg, &info);
    assert(st == VX_OK);
    assert(info.nrows == total_expected);
    assert(info.nchunks == 2); /* ceil(5000 / 4096) = 2 */
    assert(info.min_pk == 100);
    assert(info.max_pk == 100 + total_expected - 1);
    assert(info.flags & VX_SEG_FLAG_PK);

    /* Open the segment */
    vx_seg_t *seg = NULL;
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);
    assert(seg != NULL);
    assert(seg->nchunks == 2);
    assert(seg->file_hdr->nrows == total_expected);
    assert(seg->file_hdr->table_id == 42);
    assert(seg->file_hdr->segment_id == 1001);

    /* Chunk views verification */
    vx_chunk_t *cv0 = seg->chunk_views[0];
    vx_chunk_t *cv1 = seg->chunk_views[1];
    assert(cv0->nrows == VX_BATCH_CAP);
    assert(cv1->nrows == total_expected - VX_BATCH_CAP);
    assert(cv0->tier == VX_TIER_COLD_MMAP);
    assert(cv0->backing == &seg->hdr);

    /* Verify CRC */
    st = vx_seg_verify_full_crc(seg);
    assert(st == VX_OK);

    /* Verify data contents in chunk views */
    const uint64_t *cv0_pks = (const uint64_t *)cv0->cols[0].data;
    for (uint32_t r = 0; r < cv0->nrows; ++r) {
        assert(cv0_pks[r] == 100 + r);
    }
    const uint64_t *cv1_pks = (const uint64_t *)cv1->cols[0].data;
    for (uint32_t r = 0; r < cv1->nrows; ++r) {
        assert(cv1_pks[r] == 100 + VX_BATCH_CAP + r);
    }

    /* Verify strings in chunk views */
    const vx_strview_t *cv0_strs = (const vx_strview_t *)cv0->cols[4].data;
    for (uint32_t r = 0; r < 50; ++r) {
        if ((r & 1) == 0) {
            assert(vx_strview_is_inline(cv0_strs[r]));
            assert(cv0_strs[r].inl.len == 5);
            assert(memcmp(cv0_strs[r].inl.data, "quick", 5) == 0);
        } else {
            assert(!vx_strview_is_inline(cv0_strs[r]));
            assert(cv0_strs[r].out.len == (int32_t)long_len);
            const uint8_t *p = vx_strview_ptr(&cv0_strs[r], cv0->cols[4].strpool);
            assert(p != NULL);
            assert(memcmp(p, long_sample, long_len) == 0);
        }
    }

    /* Verify PK index lookup */
    for (uint32_t r = 0; r < total_expected; r += 23) {
        uint64_t probe_pk = 100 + r;
        uint32_t found_row = 0;
        bool found = vx_seg_pk_lookup(seg, NULL, probe_pk, &found_row);
        assert(found);
        assert(found_row == r);
    }

    /* Non-existent PK lookup */
    uint32_t dummy_row = 0;
    assert(!vx_seg_pk_lookup(seg, NULL, 50, &dummy_row));
    assert(!vx_seg_pk_lookup(seg, NULL, 9999999, &dummy_row));

    /* Clean up */
    vx_owner_release(&seg->hdr);
    free_test_chunk(ch0);
    free_test_chunk(ch1);
    (void)unlink(seg_path);

    printf("      -> Basic write, mmap open, StringView, and PK lookup PASSED.\n");
}

/* 2. Test deletion map tombstones stripping during segment write */
static void test_delmap_tombstones(void) {
    printf("[2/7] Testing delmap tombstone stripping...\n");

    const char *seg_path = "/tmp/test_v2_delmap.vx";
    (void)unlink(seg_path);

    vx_type_t col_types[2] = { VX_TYPE_U64, VX_TYPE_I64 };
    const uint32_t nrows = 100;

    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(*ch));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    ch->nrows = nrows;
    ch->ncols = 2;
    ch->cols = (vx_col_chunk_t *)calloc(2, sizeof(vx_col_chunk_t));

    uint64_t *pks = (uint64_t *)malloc(nrows * sizeof(uint64_t));
    int64_t  *vals = (int64_t *)malloc(nrows * sizeof(int64_t));
    for (uint32_t i = 0; i < nrows; ++i) {
        pks[i] = 1000 + i;
        vals[i] = (int64_t)i;
    }
    ch->cols[0].type = VX_TYPE_U64; ch->cols[0].data = pks;
    ch->cols[1].type = VX_TYPE_I64; ch->cols[1].data = vals;

    /* Create delmap deleting rows with even indices (0, 2, 4, ... 98) -> 50 deleted, 50 live */
    vx_delmap_t dm;
    memset(&dm, 0, sizeof(dm));
    vx_owner_init(&dm.hdr, VX_OWN_DELMAP, dummy_owner_destroy);
    dm.nrows = nrows;
    dm.live_count = 50;
    for (uint32_t r = 0; r < nrows; r += 2) {
        dm.bits[r >> 6] |= (1ULL << (r & 63u));
    }

    const vx_chunk_t *chunks[1] = { ch };
    const vx_delmap_t *delmaps[1] = { &dm };

    vx_seg_write_cfg_t cfg = {
        .table_id = 1,
        .segment_id = 2,
        .lsn_max = 100,
        .ncols = 2,
        .pk_col_idx = 0,
        .col_types = col_types,
        .chunks = chunks,
        .delmaps = delmaps,
        .nchunks = 1,
        .spill_dir = "/tmp"
    };

    vx_seg_write_info_t info;
    vx_status_t st = vx_seg_write(seg_path, &cfg, &info);
    assert(st == VX_OK);
    assert(info.nrows == 50);

    /* Open and verify only odd rows survive */
    vx_seg_t *seg = NULL;
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);
    assert(seg->file_hdr->nrows == 50);

    const uint64_t *surviving_pks = (const uint64_t *)seg->chunk_views[0]->cols[0].data;
    for (uint32_t r = 0; r < 50; ++r) {
        assert(surviving_pks[r] == 1000 + (2 * r + 1));
    }

    /* Verify PK lookup on deleted keys returns false */
    uint32_t row = 0;
    for (uint32_t r = 0; r < nrows; ++r) {
        uint64_t pk = 1000 + r;
        bool found = vx_seg_pk_lookup(seg, NULL, pk, &row);
        if ((r % 2) == 0) {
            assert(!found);
        } else {
            assert(found);
            assert(row == r / 2);
        }
    }

    vx_owner_release(&seg->hdr);
    free_test_chunk(ch);
    (void)unlink(seg_path);

    printf("      -> Delmap tombstone stripping PASSED.\n");
}

/* 3. Test NaN zone-map bounds widening to [-Inf, +Inf] */
static void test_nan_zone_map(void) {
    printf("[3/7] Testing NaN zone map bounds widening...\n");

    const char *seg_path = "/tmp/test_v2_nan.vx";
    (void)unlink(seg_path);

    vx_type_t col_types[1] = { VX_TYPE_F64 };
    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(*ch));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    ch->nrows = 10;
    ch->ncols = 1;
    ch->cols = (vx_col_chunk_t *)calloc(1, sizeof(vx_col_chunk_t));

    double *vals = (double *)malloc(10 * sizeof(double));
    for (int i = 0; i < 9; ++i) vals[i] = (double)i;
    vals[9] = (double)NAN;

    ch->cols[0].type = VX_TYPE_F64;
    ch->cols[0].data = vals;

    const vx_chunk_t *chunks[1] = { ch };
    vx_seg_write_cfg_t cfg = {
        .table_id = 1,
        .segment_id = 3,
        .lsn_max = 1,
        .ncols = 1,
        .pk_col_idx = UINT32_MAX,
        .col_types = col_types,
        .chunks = chunks,
        .delmaps = NULL,
        .nchunks = 1,
        .spill_dir = "/tmp"
    };

    vx_seg_write_info_t info;
    vx_status_t st = vx_seg_write(seg_path, &cfg, &info);
    assert(st == VX_OK);

    vx_seg_t *seg = NULL;
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);

    /* Verify chunk zone bounds widened to -Inf and +Inf */
    const vx_zone_t *z = &seg->chunk_views[0]->cols[0].zone;
    assert(z->min_val.f64 == -INFINITY);
    assert(z->max_val.f64 ==  INFINITY);

    const vx_zone_t *sz = &seg->col_dir[0].seg_zone;
    assert(sz->min_val.f64 == -INFINITY);
    assert(sz->max_val.f64 ==  INFINITY);

    vx_owner_release(&seg->hdr);
    free_test_chunk(ch);
    (void)unlink(seg_path);

    printf("      -> NaN zone map bounds widening PASSED.\n");
}

/* 4. Test full file CRC verification and corruption handling */
static void test_corruption_and_crc(void) {
    printf("[4/7] Testing full file CRC32 & corruption detection...\n");

    const char *seg_path = "/tmp/test_v2_corrupt.vx";
    (void)unlink(seg_path);

    vx_type_t col_types[1] = { VX_TYPE_U64 };
    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(*ch));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    ch->nrows = 50;
    ch->ncols = 1;
    ch->cols = (vx_col_chunk_t *)calloc(1, sizeof(vx_col_chunk_t));

    uint64_t *vals = (uint64_t *)malloc(50 * sizeof(uint64_t));
    for (int i = 0; i < 50; ++i) vals[i] = (uint64_t)i * 10;
    ch->cols[0].type = VX_TYPE_U64;
    ch->cols[0].data = vals;

    const vx_chunk_t *chunks[1] = { ch };
    vx_seg_write_cfg_t cfg = {
        .table_id = 99,
        .segment_id = 4,
        .lsn_max = 1,
        .ncols = 1,
        .pk_col_idx = 0,
        .col_types = col_types,
        .chunks = chunks,
        .delmaps = NULL,
        .nchunks = 1,
        .spill_dir = "/tmp"
    };

    vx_status_t st = vx_seg_write(seg_path, &cfg, NULL);
    assert(st == VX_OK);

    /* Open intact segment */
    vx_seg_t *seg = NULL;
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);
    assert(vx_seg_verify_full_crc(seg) == VX_OK);
    vx_owner_release(&seg->hdr);

    /* Corrupt a byte in the column data section (offset 64 is col 0 data) */
    int fd = open(seg_path, O_RDWR);
    assert(fd >= 0);
    uint8_t byte = 0xFF;
    ssize_t nw = pwrite(fd, &byte, 1, 64);
    assert(nw == 1);
    close(fd);

    /* Fast open still succeeds (meta_crc32 only checks header & metadata block) */
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);
    /* Full CRC scrub MUST fail */
    assert(vx_seg_verify_full_crc(seg) == VX_ERR_CORRUPT);
    vx_owner_release(&seg->hdr);

    /* Corrupt header magic */
    fd = open(seg_path, O_RDWR);
    assert(fd >= 0);
    byte = 'X';
    nw = pwrite(fd, &byte, 1, 0);
    assert(nw == 1);
    close(fd);

    /* Fast open MUST fail */
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_ERR_CORRUPT);

    /* Truncate file */
    assert(truncate(seg_path, 32) == 0);
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_ERR_CORRUPT);

    free_test_chunk(ch);
    (void)unlink(seg_path);

    printf("      -> Full file CRC32 & corruption detection PASSED.\n");
}

/* 5. Test empty segment handling */
static void test_empty_segment(void) {
    printf("[5/7] Testing empty segment (0 rows)...\n");

    const char *seg_path = "/tmp/test_v2_empty.vx";
    (void)unlink(seg_path);

    vx_type_t col_types[2] = { VX_TYPE_U64, VX_TYPE_STR };
    vx_seg_write_cfg_t cfg = {
        .table_id = 10,
        .segment_id = 5,
        .lsn_max = 0,
        .ncols = 2,
        .pk_col_idx = 0,
        .col_types = col_types,
        .chunks = NULL,
        .delmaps = NULL,
        .nchunks = 0,
        .spill_dir = "/tmp"
    };

    vx_seg_write_info_t info;
    vx_status_t st = vx_seg_write(seg_path, &cfg, &info);
    assert(st == VX_OK);
    assert(info.nrows == 0);
    assert(info.nchunks == 0);

    vx_seg_t *seg = NULL;
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);
    assert(seg->nchunks == 0);
    assert(seg->chunk_views == NULL);
    assert(seg->file_hdr->nrows == 0);
    assert(vx_seg_verify_full_crc(seg) == VX_OK);

    vx_owner_release(&seg->hdr);
    (void)unlink(seg_path);

    printf("      -> Empty segment handling PASSED.\n");
}

/* 6. Test unlink_on_destroy lifecycle */
static void test_unlink_on_destroy(void) {
    printf("[6/7] Testing unlink_on_destroy lifecycle...\n");

    const char *seg_path = "/tmp/test_v2_unlink.vx";
    (void)unlink(seg_path);

    vx_type_t col_types[1] = { VX_TYPE_U64 };
    vx_seg_write_cfg_t cfg = {
        .table_id = 1,
        .segment_id = 6,
        .lsn_max = 0,
        .ncols = 1,
        .pk_col_idx = UINT32_MAX,
        .col_types = col_types,
        .chunks = NULL,
        .delmaps = NULL,
        .nchunks = 0,
        .spill_dir = "/tmp"
    };

    vx_status_t st = vx_seg_write(seg_path, &cfg, NULL);
    assert(st == VX_OK);
    assert(access(seg_path, F_OK) == 0);

    vx_seg_t *seg = NULL;
    st = vx_seg_open(seg_path, &seg);
    assert(st == VX_OK);

    /* Mark segment for deferred unlink */
    vx_seg_mark_unlink(seg);
    assert(access(seg_path, F_OK) == 0);

    /* Release segment -> triggers destroy -> unlinks file */
    vx_owner_release(&seg->hdr);
    assert(access(seg_path, F_OK) != 0);

    printf("      -> unlink_on_destroy lifecycle PASSED.\n");
}

/* 7. Test duplicate PK error detection on write */
static void test_unique_pk_rejection(void) {
    printf("[7/7] Testing duplicate primary key rejection...\n");

    const char *seg_path = "/tmp/test_v2_dup_pk.vx";
    (void)unlink(seg_path);

    vx_type_t col_types[1] = { VX_TYPE_U64 };
    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(*ch));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    ch->nrows = 4;
    ch->ncols = 1;
    ch->cols = (vx_col_chunk_t *)calloc(1, sizeof(vx_col_chunk_t));

    uint64_t *vals = (uint64_t *)malloc(4 * sizeof(uint64_t));
    vals[0] = 10;
    vals[1] = 20;
    vals[2] = 10; /* Duplicate key */
    vals[3] = 30;
    ch->cols[0].type = VX_TYPE_U64;
    ch->cols[0].data = vals;

    const vx_chunk_t *chunks[1] = { ch };
    vx_seg_write_cfg_t cfg = {
        .table_id = 1,
        .segment_id = 7,
        .lsn_max = 1,
        .ncols = 1,
        .pk_col_idx = 0,
        .col_types = col_types,
        .chunks = chunks,
        .delmaps = NULL,
        .nchunks = 1,
        .spill_dir = "/tmp"
    };

    vx_status_t st = vx_seg_write(seg_path, &cfg, NULL);
    assert(st == VX_ERR_UNIQUE);
    assert(access(seg_path, F_OK) != 0); /* Temporary file cleaned up */

    free_test_chunk(ch);
    printf("      -> Duplicate primary key rejection PASSED.\n");
}

int main(void) {
    printf("=== V2 Immutable Segment Writer & mmap Reader Verification ===\n");
    test_basic_types_write_read();
    test_delmap_tombstones();
    test_nan_zone_map();
    test_corruption_and_crc();
    test_empty_segment();
    test_unlink_on_destroy();
    test_unique_pk_rejection();
    printf("All 7 segment writer & reader tests passed successfully!\n");
    return 0;
}
