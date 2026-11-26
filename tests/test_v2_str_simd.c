#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vx_types.h"
#include "vx_owner.h"
#include "vx_arena.h"
#include "vx_str.h"
#include "vx_batch.h"
#include "vx_simd_str_common.h"

#if defined(__x86_64__) || defined(_M_X64)
extern uint32_t vx_simd_avx2_filter_str_eq(const vx_strview_t *col, const vx_strpool_t *pool,
                                            const uint64_t *validity, const vx_sel_t *in_sel,
                                            vx_sel_t *out_sel, const uint8_t *rhs, int32_t rhs_len,
                                            bool negate);
#endif

int main(void) {
    printf("[test_v2_str_simd] Testing AVX2 StringView filter kernels...\n");

#if defined(__x86_64__) || defined(_M_X64)
    /* 1. Test inline string filter (<= 12 bytes) */
    vx_strview_t col[8];
    memset(col, 0, sizeof(col));

    /* Populate strings: 
     * 0: "apple", 1: "banana", 2: "apple", 3: "cherry",
     * 4: "apple", 5: "date",   6: "elderberry", 7: "fig" */
    col[0] = vx_make_str_target((const uint8_t *)"apple", 5);
    col[1] = vx_make_str_target((const uint8_t *)"banana", 6);
    col[2] = vx_make_str_target((const uint8_t *)"apple", 5);
    col[3] = vx_make_str_target((const uint8_t *)"cherry", 6);
    col[4] = vx_make_str_target((const uint8_t *)"apple", 5);
    col[5] = vx_make_str_target((const uint8_t *)"date", 4);
    col[6] = vx_make_str_target((const uint8_t *)"elderberry", 10);
    col[7] = vx_make_str_target((const uint8_t *)"fig", 3);

    vx_sel_t in_sel;
    in_sel.count = 8;
    in_sel.is_identity = true;

    vx_sel_t out_sel;
    memset(&out_sel, 0, sizeof(out_sel));

    /* Filter for "apple" (==) */
    uint32_t matched = vx_simd_avx2_filter_str_eq(col, NULL, NULL, &in_sel, &out_sel,
                                                  (const uint8_t *)"apple", 5, false);
    assert(matched == 3);
    assert(out_sel.count == 3);
    assert(out_sel.idx[0] == 0);
    assert(out_sel.idx[1] == 2);
    assert(out_sel.idx[2] == 4);

    /* Filter with null validity: row 2 is NULL (validity bit 2 = 0) */
    uint64_t val_bits = 0xFFFFFFFFFFFFFFFBULL; /* bit 2 clear */
    matched = vx_simd_avx2_filter_str_eq(col, NULL, &val_bits, &in_sel, &out_sel,
                                         (const uint8_t *)"apple", 5, false);
    assert(matched == 2);
    assert(out_sel.count == 2);
    assert(out_sel.idx[0] == 0);
    assert(out_sel.idx[1] == 4);

    /* 2. Test out-of-line strings (> 12 bytes) with shared stringpool */
    const char *long_a = "supercalifragilisticexpialidocious";
    const char *long_b = "supercalifragilistic_different_tail";
    const char *long_c = "supercalifragilisticexpialidocious";

    uint8_t pool_buf[256];
    memcpy(pool_buf + 0, long_a, strlen(long_a));
    memcpy(pool_buf + 64, long_b, strlen(long_b));
    memcpy(pool_buf + 128, long_c, strlen(long_c));

    const uint8_t *ptrs[1] = { pool_buf };
    int64_t lens[1] = { 256 };
    vx_strpool_t pool = {
        .ptrs = ptrs,
        .lens = lens,
        .owners = NULL,
        .nbufs = 1,
        .cap = 1
    };

    memset(col, 0, sizeof(col));
    /* row 0: long_a, offset 0 */
    col[0].out.len = (int32_t)strlen(long_a);
    memcpy(col[0].out.prefix, long_a, 4);
    col[0].out.buf_idx = 0;
    col[0].out.offset = 0;

    /* row 1: long_b, offset 64 (same 4B prefix "supe", different tail) */
    col[1].out.len = (int32_t)strlen(long_b);
    memcpy(col[1].out.prefix, long_b, 4);
    col[1].out.buf_idx = 0;
    col[1].out.offset = 64;

    /* row 2: long_c, offset 128 (exact match to long_a) */
    col[2].out.len = (int32_t)strlen(long_c);
    memcpy(col[2].out.prefix, long_c, 4);
    col[2].out.buf_idx = 0;
    col[2].out.offset = 128;

    in_sel.count = 4;
    in_sel.is_identity = true;

    matched = vx_simd_avx2_filter_str_eq(col, &pool, NULL, &in_sel, &out_sel,
                                         (const uint8_t *)long_a, (int32_t)strlen(long_a), false);
    assert(matched == 2);
    assert(out_sel.idx[0] == 0);
    assert(out_sel.idx[1] == 2);

    printf("[test_v2_str_simd] ALL TESTS PASSED!\n");
#else
    printf("[test_v2_str_simd] Skipped on non-x86_64 architecture.\n");
#endif

    return 0;
}

