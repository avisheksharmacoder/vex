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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/vx_art_fmt.h"
#include "src/vx_idx.h"
#include "src/vx_chunk.h"

typedef struct {
    uint32_t hit_count;
    uint32_t cap;
    uint32_t *hits;
    bool     stop_after_k;
    uint32_t stop_k;
} test_art_hit_ctx_t;

static vx_status_t test_art_on_hit(void *ctx, uint32_t row_idx, bool *out_stop) {
    test_art_hit_ctx_t *tc = (test_art_hit_ctx_t *)ctx;
    if (tc->hit_count < tc->cap && tc->hits) {
        tc->hits[tc->hit_count] = row_idx;
    }
    tc->hit_count++;
    if (tc->stop_after_k && tc->hit_count >= tc->stop_k) {
        *out_stop = true;
    }
    return VX_OK;
}

/* ============================================================================
 * 1. Test Empty ART
 * ============================================================================ */
static void test_empty_art(void) {
    printf("[1/6] Testing empty ART build, open, lookup and range scan...\n");
    const char *path = "/tmp/test_art_empty.idx";
    (void)unlink(path);

    vx_status_t st = vx_art_build_file(path, 1, 1, 100, 1, false, NULL, 0);
    assert(st == VX_OK);

    vx_idx_file_t *idx = NULL;
    st = vx_art_open(path, &idx);
    assert(st == VX_OK);
    assert(idx != NULL);
    assert(idx->file_hdr->nrows_indexed == 0);
    assert(idx->file_hdr->root_off == VX_ART_EMPTY_REF);

    /* Point lookup on empty */
    uint8_t k[8] = { 0 };
    test_art_hit_ctx_t tc = {0};
    st = vx_art_lookup_eq(idx, NULL, k, 8, test_art_on_hit, &tc);
    assert(st == VX_OK);
    assert(tc.hit_count == 0);

    /* Range scan on empty */
    vx_art_bound_t lo = { .unbounded = true };
    vx_art_bound_t hi = { .unbounded = true };
    st = vx_art_scan_range(idx, NULL, lo, hi, test_art_on_hit, &tc);
    assert(st == VX_OK);
    assert(tc.hit_count == 0);

    vx_idx_close(idx);
    (void)unlink(path);
}

/* ============================================================================
 * 2. Test ART Node Transitions (Node4, Node16, Node48, Node256)
 * ============================================================================ */
static void test_node_transitions(void) {
    printf("[2/6] Testing ART node transitions (Node4, Node16, Node48, Node256)...\n");

    uint32_t branch_counts[] = { 3, 4, 10, 16, 32, 48, 100, 256 };
    for (size_t b = 0; b < sizeof(branch_counts) / sizeof(branch_counts[0]); ++b) {
        uint32_t n_children = branch_counts[b];
        char path[128];
        snprintf(path, sizeof(path), "/tmp/test_art_nodes_%u.idx", n_children);
        (void)unlink(path);

        vx_art_entry_t *entries = (vx_art_entry_t *)calloc(n_children, sizeof(*entries));
        assert(entries != NULL);

        uint8_t *keys_buf = (uint8_t *)malloc(n_children * 8);
        assert(keys_buf != NULL);

        for (uint32_t i = 0; i < n_children; ++i) {
            uint8_t *k = keys_buf + i * 8;
            /* Common prefix of 7 bytes, 8th byte is distinct */
            memset(k, 0xAA, 7);
            k[7] = (uint8_t)i;

            entries[i].key     = k;
            entries[i].key_len = 8;
            entries[i].row_idx = i * 10;
        }

        vx_status_t st = vx_art_build_file(path, 1, 1, 101, 1, true, entries, n_children);
        assert(st == VX_OK);

        vx_idx_file_t *idx = NULL;
        st = vx_art_open(path, &idx);
        assert(st == VX_OK);
        assert(idx->file_hdr->nrows_indexed == n_children);

        /* Verify all keys via point lookup */
        for (uint32_t i = 0; i < n_children; ++i) {
            uint8_t *k = keys_buf + i * 8;
            uint32_t hit_row = UINT32_MAX;
            test_art_hit_ctx_t tc = { .cap = 1, .hits = &hit_row };
            st = vx_art_lookup_eq(idx, NULL, k, 8, test_art_on_hit, &tc);
            assert(st == VX_OK);
            assert(tc.hit_count == 1);
            assert(hit_row == i * 10);
        }

        /* Verify range scan visits all keys in ascending order */
        uint32_t *all_hits = (uint32_t *)malloc(n_children * sizeof(uint32_t));
        test_art_hit_ctx_t rtc = { .cap = n_children, .hits = all_hits };
        vx_art_bound_t lo = { .unbounded = true };
        vx_art_bound_t hi = { .unbounded = true };
        st = vx_art_scan_range(idx, NULL, lo, hi, test_art_on_hit, &rtc);
        assert(st == VX_OK);
        assert(rtc.hit_count == n_children);
        for (uint32_t i = 0; i < n_children; ++i) {
            assert(all_hits[i] == i * 10);
        }

        free(all_hits);
        free(keys_buf);
        free(entries);
        vx_idx_close(idx);
        (void)unlink(path);
    }
}

/* ============================================================================
 * 3. Test Long Optimistic Prefixes (> 12 Bytes) & Min-Leaf Descent
 * ============================================================================ */
static void test_long_prefixes(void) {
    printf("[3/6] Testing long optimistic prefixes (> 12 bytes) and min-leaf descent...\n");

    const char *path = "/tmp/test_art_long_prefix.idx";
    (void)unlink(path);

    const uint32_t n_entries = 50;
    const uint32_t prefix_len = 32; /* > 12 bytes */
    const uint32_t total_key_len = prefix_len + 4;

    vx_art_entry_t *entries = (vx_art_entry_t *)calloc(n_entries, sizeof(*entries));
    uint8_t *buf = (uint8_t *)malloc(n_entries * total_key_len);
    assert(entries && buf);

    for (uint32_t i = 0; i < n_entries; ++i) {
        uint8_t *k = buf + i * total_key_len;
        /* Prefix of 32 bytes */
        for (uint32_t p = 0; p < prefix_len; ++p) {
            k[p] = (uint8_t)('A' + (p % 26));
        }
        /* Suffix: i big-endian */
        uint32_t be = __builtin_bswap32(i);
        memcpy(k + prefix_len, &be, 4);

        entries[i].key     = k;
        entries[i].key_len = total_key_len;
        entries[i].row_idx = i + 1000;
    }

    vx_status_t st = vx_art_build_file(path, 1, 1, 102, 1, true, entries, n_entries);
    assert(st == VX_OK);

    vx_idx_file_t *idx = NULL;
    st = vx_art_open(path, &idx);
    assert(st == VX_OK);

    /* Lookup all 50 entries */
    for (uint32_t i = 0; i < n_entries; ++i) {
        uint8_t *k = buf + i * total_key_len;
        uint32_t hit_row = 0;
        test_art_hit_ctx_t tc = { .cap = 1, .hits = &hit_row };
        st = vx_art_lookup_eq(idx, NULL, k, total_key_len, test_art_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 1);
        assert(hit_row == i + 1000);
    }

    /* Key with slight prefix difference must return 0 hits */
    uint8_t bad_key[total_key_len];
    memcpy(bad_key, buf, total_key_len);
    bad_key[15] ^= 0xFF; /* change byte beyond 12 */
    test_art_hit_ctx_t bad_tc = {0};
    st = vx_art_lookup_eq(idx, NULL, bad_key, total_key_len, test_art_on_hit, &bad_tc);
    assert(st == VX_OK);
    assert(bad_tc.hit_count == 0);

    /* Range scan between entry 10 and 30 inclusive */
    uint8_t *lo_k = buf + 10 * total_key_len;
    uint8_t *hi_k = buf + 30 * total_key_len;
    vx_art_bound_t lo = { .key = lo_k, .len = total_key_len, .inclusive = true, .unbounded = false };
    vx_art_bound_t hi = { .key = hi_k, .len = total_key_len, .inclusive = true, .unbounded = false };

    uint32_t hits[50];
    test_art_hit_ctx_t rtc = { .cap = 50, .hits = hits };
    st = vx_art_scan_range(idx, NULL, lo, hi, test_art_on_hit, &rtc);
    assert(st == VX_OK);
    assert(rtc.hit_count == 21); /* 10 through 30 */
    for (uint32_t i = 0; i < 21; ++i) {
        assert(hits[i] == (10 + i) + 1000);
    }

    free(buf);
    free(entries);
    vx_idx_close(idx);
    (void)unlink(path);
}

/* ============================================================================
 * 4. Test Unique vs Non-Unique Posting Lists & Duplicate Rejection
 * ============================================================================ */
static void test_unique_and_posting_lists(void) {
    printf("[4/6] Testing unique enforcement and non-unique posting lists...\n");

    const char *path_uniq = "/tmp/test_art_uniq.idx";
    const char *path_nonuniq = "/tmp/test_art_nonuniq.idx";
    (void)unlink(path_uniq);
    (void)unlink(path_nonuniq);

    /* Entries with duplicate keys: 3 keys, each appearing 5 times (15 entries total) */
    uint8_t k1[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t k2[4] = { 0x01, 0x02, 0x03, 0x05 };
    uint8_t k3[4] = { 0x01, 0x02, 0x03, 0x06 };

    vx_art_entry_t entries[15];
    for (uint32_t i = 0; i < 5; ++i) {
        entries[i].key     = k1;
        entries[i].key_len = 4;
        entries[i].row_idx = 100 + i;

        entries[5 + i].key     = k2;
        entries[5 + i].key_len = 4;
        entries[5 + i].row_idx = 200 + i;

        entries[10 + i].key     = k3;
        entries[10 + i].key_len = 4;
        entries[10 + i].row_idx = 300 + i;
    }

    /* 4.1 Unique index creation must FAIL with VX_ERR_UNIQUE */
    vx_status_t st = vx_art_build_file(path_uniq, 1, 1, 103, 1, true, entries, 15);
    assert(st == VX_ERR_UNIQUE);

    /* 4.2 Non-unique index creation must SUCCEED and build posting lists */
    st = vx_art_build_file(path_nonuniq, 1, 1, 103, 1, false, entries, 15);
    assert(st == VX_OK);

    vx_idx_file_t *idx = NULL;
    st = vx_art_open(path_nonuniq, &idx);
    assert(st == VX_OK);
    assert(idx->file_hdr->nrows_indexed == 15);

    /* Point lookup on k2 must return exactly 5 rows (200, 201, 202, 203, 204) */
    uint32_t hits[16];
    test_art_hit_ctx_t tc = { .cap = 16, .hits = hits };
    st = vx_art_lookup_eq(idx, NULL, k2, 4, test_art_on_hit, &tc);
    assert(st == VX_OK);
    assert(tc.hit_count == 5);
    for (uint32_t i = 0; i < 5; ++i) {
        assert(hits[i] == 200 + i);
    }

    /* Range scan over all entries returns all 15 in order */
    test_art_hit_ctx_t all_tc = { .cap = 16, .hits = hits };
    vx_art_bound_t lo = { .unbounded = true };
    vx_art_bound_t hi = { .unbounded = true };
    st = vx_art_scan_range(idx, NULL, lo, hi, test_art_on_hit, &all_tc);
    assert(st == VX_OK);
    assert(all_tc.hit_count == 15);
    for (uint32_t i = 0; i < 5; ++i) assert(hits[i] == 100 + i);
    for (uint32_t i = 0; i < 5; ++i) assert(hits[5 + i] == 200 + i);
    for (uint32_t i = 0; i < 5; ++i) assert(hits[10 + i] == 300 + i);

    vx_idx_close(idx);
    (void)unlink(path_nonuniq);
}

/* ============================================================================
 * 5. Test Range Scans with Early Termination and Delmap Filtering
 * ============================================================================ */
static void test_range_pruning_and_delmap(void) {
    printf("[5/6] Testing range pruning, early stop, and delmap tombstones...\n");

    const char *path = "/tmp/test_art_prune.idx";
    (void)unlink(path);

    /* 1,000 monotonically increasing u64 keys */
    const uint32_t N = 1000;
    vx_art_entry_t *entries = (vx_art_entry_t *)calloc(N, sizeof(*entries));
    uint8_t *enc_buf = (uint8_t *)malloc(N * 8);
    assert(entries && enc_buf);

    for (uint32_t i = 0; i < N; ++i) {
        uint8_t *k = enc_buf + i * 8;
        vx_key_encode_u64((uint64_t)(i * 10), k);
        entries[i].key     = k;
        entries[i].key_len = 8;
        entries[i].row_idx = i;
    }

    vx_status_t st = vx_art_build_file(path, 1, 1, 104, 1, true, entries, N);
    assert(st == VX_OK);

    vx_idx_file_t *idx = NULL;
    st = vx_art_open(path, &idx);
    assert(st == VX_OK);

    /* Early termination: stop after 5 hits */
    {
        uint32_t hits[10];
        test_art_hit_ctx_t tc = { .cap = 10, .hits = hits, .stop_after_k = true, .stop_k = 5 };
        vx_art_bound_t lo = { .unbounded = true };
        vx_art_bound_t hi = { .unbounded = true };
        st = vx_art_scan_range(idx, NULL, lo, hi, test_art_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 5);
        for (uint32_t i = 0; i < 5; ++i) assert(hits[i] == i);
    }

    /* Range scan with delmap filtering */
    {
        vx_delmap_t *dm = vx_delmap_create(4096);
        assert(dm != NULL);

        /* Delete even rows 100, 102, 104, ..., 198 */
        for (uint32_t r = 100; r < 200; r += 2) {
            dm->bits[r >> 6] |= (1ULL << (r & 63u));
            dm->live_count--;
        }

        const vx_delmap_t *dms[1] = { dm };

        /* Scan keys [1000, 1990] => rows 100 to 199 (100 rows total) */
        uint8_t lo_k[8], hi_k[8];
        vx_key_encode_u64(1000, lo_k);
        vx_key_encode_u64(1990, hi_k);
        vx_art_bound_t lo = { .key = lo_k, .len = 8, .inclusive = true, .unbounded = false };
        vx_art_bound_t hi = { .key = hi_k, .len = 8, .inclusive = true, .unbounded = false };

        uint32_t hits[200];
        test_art_hit_ctx_t tc = { .cap = 200, .hits = hits };
        st = vx_art_scan_range(idx, dms, lo, hi, test_art_on_hit, &tc);
        assert(st == VX_OK);
        assert(tc.hit_count == 50); /* 50 odd rows survive */
        for (uint32_t i = 0; i < 50; ++i) {
            uint32_t expected_row = 101 + i * 2;
            assert(hits[i] == expected_row);
        }

        vx_owner_release(&dm->hdr);
    }

    free(enc_buf);
    free(entries);
    vx_idx_close(idx);
    (void)unlink(path);
}

/* ============================================================================
 * 6. Test File Header & CRC Corruption Handling
 * ============================================================================ */
static void test_corruption_safety(void) {
    printf("[6/6] Testing corruption detection (bad magic, bad CRC, truncated)...\n");

    const char *path = "/tmp/test_art_corrupt.idx";
    (void)unlink(path);

    uint8_t k[8];
    vx_key_encode_u64(42, k);
    vx_art_entry_t e = { .key = k, .key_len = 8, .row_idx = 1 };
    vx_status_t st = vx_art_build_file(path, 1, 1, 105, 1, true, &e, 1);
    assert(st == VX_OK);

    /* Verify it opens cleanly first */
    vx_idx_file_t *idx = NULL;
    st = vx_art_open(path, &idx);
    assert(st == VX_OK);
    vx_idx_close(idx);

    /* 6.1 Bad CRC corruption: flip a bit in payload */
    int fd = open(path, O_RDWR);
    assert(fd >= 0);
    off_t sz = lseek(fd, 0, SEEK_END);
    assert(sz > 64);
    uint8_t b = 0;
    ssize_t r1 = pread(fd, &b, 1, 64);
    assert(r1 == 1);
    b ^= 0xFF;
    ssize_t w1 = pwrite(fd, &b, 1, 64);
    assert(w1 == 1);
    close(fd);

    st = vx_art_open(path, &idx);
    assert(st == VX_ERR_CORRUPT);

    /* 6.2 Bad magic */
    fd = open(path, O_RDWR);
    assert(fd >= 0);
    ssize_t w2 = pwrite(fd, "BADMAGIC", 8, 0);
    assert(w2 == 8);
    close(fd);

    st = vx_art_open(path, &idx);
    assert(st == VX_ERR_CORRUPT);

    /* 6.3 Truncated file */
    int tr = truncate(path, 32); /* < sizeof(vx_idx_hdr_t) */
    assert(tr == 0);
    st = vx_art_open(path, &idx);
    assert(st == VX_ERR_CORRUPT);

    (void)unlink(path);
}

int main(void) {
    printf("=== Starting Milestone M16/Docs-17 ART Test Suite ===\n");
    test_empty_art();
    test_node_transitions();
    test_long_prefixes();
    test_unique_and_posting_lists();
    test_range_pruning_and_delmap();
    test_corruption_safety();
    printf("=== ALL ART TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
