#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "src/vx_seg_fmt.h"
#include "src/vx_manifest_fmt.h"

/* Test 1: Wire struct sizes and alignments */
static void test_struct_sizes(void) {
    printf("[1/5] Testing wire struct sizes and alignments...\n");

    /* Segment wire format structs */
    assert(sizeof(vx_seg_hdr_t) == 64);
    assert(sizeof(vx_seg_col_desc_t) == 96);
    assert(sizeof(vx_seg_chunk_meta_t) == 40);
    assert(sizeof(vx_seg_strbuf_desc_t) == 16);
    assert(sizeof(vx_seg_pk_hdr_t) == 32);
    assert(sizeof(vx_seg_pk_slot_t) == 16);
    assert(sizeof(vx_seg_footer_t) == 64);

    /* Manifest & sidecar wire format structs */
    assert(sizeof(vx_del_hdr_t) == 64);
    assert(sizeof(vx_man_hdr_t) == 64);
    assert(sizeof(vx_man_table_t) == 24);
    assert(sizeof(vx_man_col_t) == 16);
    assert(sizeof(vx_man_seg_t) == 64);

    /* 64-byte alignment helper */
    assert(vx_align64(0) == 0);
    assert(vx_align64(1) == 64);
    assert(vx_align64(63) == 64);
    assert(vx_align64(64) == 64);
    assert(vx_align64(65) == 128);
    assert(vx_align64(128) == 128);
    assert(vx_align64(129) == 192);

    /* Key field offsets */
    assert(offsetof(vx_seg_hdr_t, min_pk) == 48);
    assert(offsetof(vx_seg_hdr_t, max_pk) == 56);
    assert(offsetof(vx_seg_footer_t, magic) == 56);
    assert(offsetof(vx_del_hdr_t, bits_off) == 44);
    assert(offsetof(vx_del_hdr_t, file_bytes) == 48);
    assert(offsetof(vx_man_hdr_t, checkpoint_lsn) == 24);

    printf("      -> Wire struct sizes & alignments verified successfully.\n");
}

/* Test 2: Split-Block Bloom Filter */
static void test_bloom_filter(void) {
    printf("[2/5] Testing Split-Block Bloom Filter...\n");

    const uint32_t nblocks = 64; /* 64 * 32B = 2048 bytes */
    const uint32_t nblocks_mask = nblocks - 1;
    const uint32_t seed = 0x12345678u;
    uint32_t *blocks = calloc(nblocks * 8, sizeof(uint32_t));
    assert(blocks != NULL);

    /* Insert 100 distinct keys */
    const uint32_t nkeys = 100;
    uint64_t keys[100];
    for (uint32_t i = 0; i < nkeys; ++i) {
        keys[i] = (uint64_t)i * 104729ULL + 7ULL;
        vx_bloom_insert(blocks, nblocks_mask, seed, keys[i]);
    }

    /* All inserted keys MUST test positive */
    for (uint32_t i = 0; i < nkeys; ++i) {
        bool hit = vx_bloom_may_contain(blocks, nblocks_mask, seed, keys[i]);
        assert(hit == true);
    }

    /* Test 2000 non-inserted keys to estimate false positive rate */
    uint32_t fp_count = 0;
    const uint32_t test_count = 2000;
    for (uint32_t i = 0; i < test_count; ++i) {
        uint64_t probe_key = 0x8000000000000000ULL + i * 37ULL;
        if (vx_bloom_may_contain(blocks, nblocks_mask, seed, probe_key)) {
            fp_count++;
        }
    }

    /* With 2048 bytes (16384 bits) for 100 keys (163 bits/key),
     * false-positive rate should be < 1% (less than 20 out of 2000). */
    double fp_rate = (double)fp_count / (double)test_count;
    printf("      -> Bloom FP count: %u / %u (rate: %.4f%%)\n", fp_count, test_count, fp_rate * 100.0);
    assert(fp_count <= 20);

    free(blocks);
}

/* Helper to insert into Robin Hood hash table */
static void rh_insert(vx_seg_pk_slot_t *slots, uint32_t cap, uint32_t seed,
                      uint64_t pk, uint32_t row_idx, uint32_t *max_psl) {
    uint32_t mask = cap - 1u;
    uint32_t pos = (uint32_t)vx_pk_hash64(pk, seed) & mask;
    uint32_t psl = 0;
    vx_seg_pk_slot_t cur = { .pk = pk, .row_idx = row_idx, .psl = 0 };

    while (1) {
        if (slots[pos].row_idx == VX_PK_SLOT_EMPTY) {
            cur.psl = psl;
            slots[pos] = cur;
            if (psl > *max_psl) *max_psl = psl;
            return;
        }
        if (psl > slots[pos].psl) {
            /* Robin Hood swap with resident item */
            vx_seg_pk_slot_t tmp = slots[pos];
            cur.psl = psl;
            slots[pos] = cur;
            cur = tmp;
            psl = cur.psl;
        }
        pos = (pos + 1u) & mask;
        psl++;
        cur.psl = psl;
        if (psl > *max_psl) *max_psl = psl;
    }
}

/* Test 3: Immutable Robin Hood PK Index Probes with Delmap */
static void test_pk_lookup(void) {
    printf("[3/5] Testing Robin Hood PK Lookup & Delmap Tombstones...\n");

    const uint32_t rh_cap = 64; /* power of 2 */
    vx_seg_pk_slot_t *slots = malloc(rh_cap * sizeof(vx_seg_pk_slot_t));
    assert(slots != NULL);
    for (uint32_t i = 0; i < rh_cap; ++i) {
        slots[i].row_idx = VX_PK_SLOT_EMPTY;
        slots[i].pk = 0;
        slots[i].psl = 0;
    }

    const uint32_t bloom_nblocks = 16;
    uint32_t *bloom = calloc(bloom_nblocks * 8, sizeof(uint32_t));
    assert(bloom != NULL);

    const uint32_t seed = 0xCAFEBABEu;
    uint32_t max_psl = 0;

    /* Insert keys, including 0 and UINT64_MAX boundary values */
    uint64_t test_pks[] = { 0, 42, 100, 250, 777, 1000, 99999, UINT64_MAX };
    uint32_t num_pks = sizeof(test_pks) / sizeof(test_pks[0]);

    for (uint32_t i = 0; i < num_pks; ++i) {
        rh_insert(slots, rh_cap, seed, test_pks[i], i, &max_psl);
        vx_bloom_insert(bloom, bloom_nblocks - 1u, seed, test_pks[i]);
    }

    vx_seg_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_SEG_MAGIC, 8);
    hdr.version = VX_SEG_VERSION;
    hdr.flags = VX_SEG_FLAG_PK;
    hdr.nrows = num_pks;
    hdr.min_pk = 0;
    hdr.max_pk = UINT64_MAX;

    vx_seg_pk_hdr_t pkh;
    pkh.bloom_off = 0;
    pkh.bloom_nblocks = bloom_nblocks;
    pkh.bloom_seed = seed;
    pkh.rh_off = 0;
    pkh.rh_cap = rh_cap;
    pkh.rh_max_psl = max_psl;

    vx_seg_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.file_hdr = &hdr;
    seg.pk_hdr = &pkh;
    seg.bloom_blocks = bloom;
    seg.pk_slots = slots;

    /* 1. All inserted PKs must resolve to correct row_idx with del_bits == NULL */
    for (uint32_t i = 0; i < num_pks; ++i) {
        uint32_t out_row = UINT32_MAX;
        bool found = vx_seg_pk_lookup(&seg, NULL, test_pks[i], &out_row);
        assert(found == true);
        assert(out_row == i);
    }

    /* 2. Keys not inserted must return false */
    uint32_t dummy_row;
    assert(!vx_seg_pk_lookup(&seg, NULL, 12345, &dummy_row));
    assert(!vx_seg_pk_lookup(&seg, NULL, 999, &dummy_row));
    assert(!vx_seg_pk_lookup(&seg, NULL, 500, &dummy_row));

    /* 3. Range bounds check: if min_pk / max_pk tightened */
    hdr.min_pk = 40;
    hdr.max_pk = 100000;
    /* test_pks[0] is 0 -> out of min_pk bound */
    assert(!vx_seg_pk_lookup(&seg, NULL, 0, &dummy_row));
    /* test_pks[7] is UINT64_MAX -> out of max_pk bound */
    assert(!vx_seg_pk_lookup(&seg, NULL, UINT64_MAX, &dummy_row));
    /* key 42 is in range */
    assert(vx_seg_pk_lookup(&seg, NULL, 42, &dummy_row) && dummy_row == 1);

    /* Reset bounds */
    hdr.min_pk = 0;
    hdr.max_pk = UINT64_MAX;

    /* 4. Delmap tombstoning: delete row 1 (pk 42) and row 4 (pk 777) */
    vx_delmap_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.bits[(1 & 4095u) >> 6] |= (1ULL << (1 & 63));
    dm.bits[(4 & 4095u) >> 6] |= (1ULL << (4 & 63));
    const vx_delmap_t *delmaps[1] = { &dm };

    /* pk 42 (row 1) should now return false */
    assert(!vx_seg_pk_lookup(&seg, delmaps, 42, &dummy_row));
    /* pk 777 (row 4) should now return false */
    assert(!vx_seg_pk_lookup(&seg, delmaps, 777, &dummy_row));
    /* pk 100 (row 2) remains live */
    assert(vx_seg_pk_lookup(&seg, delmaps, 100, &dummy_row) && dummy_row == 2);
    /* pk 0 (row 0) remains live */
    assert(vx_seg_pk_lookup(&seg, delmaps, 0, &dummy_row) && dummy_row == 0);

    /* 5. Flag disabled */
    hdr.flags &= ~VX_SEG_FLAG_PK;
    assert(!vx_seg_pk_lookup(&seg, delmaps, 100, &dummy_row));
    hdr.flags |= VX_SEG_FLAG_PK;

    /* 6. nrows == 0 */
    hdr.nrows = 0;
    assert(!vx_seg_pk_lookup(&seg, delmaps, 100, &dummy_row));

    free(slots);
    free(bloom);
    printf("      -> PK lookup & delmap masking verified successfully.\n");
}

/* Test 4: MANIFEST and .del sidecar header initialization */
static void test_manifest_del_headers(void) {
    printf("[4/5] Testing MANIFEST and .del header layouts...\n");

    /* .del header */
    vx_del_hdr_t del_hdr;
    memset(&del_hdr, 0, sizeof(del_hdr));
    memcpy(del_hdr.magic, VX_DEL_MAGIC, 8);
    del_hdr.version = VX_FMT_VERSION;
    del_hdr.table_id = 1;
    del_hdr.segment_id = 42;
    del_hdr.generation = 3;
    del_hdr.nrows = 8192;
    del_hdr.live_rows = 8000;
    del_hdr.nchunks = 2;
    del_hdr.bits_off = (uint32_t)vx_align64(64 + del_hdr.nchunks * sizeof(uint32_t));
    del_hdr.file_bytes = del_hdr.bits_off + del_hdr.nchunks * 512ULL;

    assert(memcmp(del_hdr.magic, "VXDEL02\0", 8) == 0);
    assert(del_hdr.version == 2);
    assert(del_hdr.bits_off == 128); /* 64 + 2*4 = 72 -> aligned to 64 = 128 */
    assert(del_hdr.file_bytes == 128 + 2 * 512);

    /* MANIFEST header */
    vx_man_hdr_t man_hdr;
    memset(&man_hdr, 0, sizeof(man_hdr));
    memcpy(man_hdr.magic, VX_MAN_MAGIC, 8);
    man_hdr.version = VX_FMT_VERSION;
    man_hdr.manifest_gen = 10;
    man_hdr.checkpoint_lsn = 1050;
    man_hdr.next_seg_id = 43;
    man_hdr.ntables = 2;
    man_hdr.nsegments = 5;

    assert(memcmp(man_hdr.magic, "VXMAN02\0", 8) == 0);
    assert(man_hdr.version == 2);
    assert(man_hdr.ntables == 2);
    assert(man_hdr.nsegments == 5);

    /* MANIFEST segment descriptor */
    vx_man_seg_t man_seg;
    memset(&man_seg, 0, sizeof(man_seg));
    man_seg.segment_id = 42;
    man_seg.del_gen = 3;
    man_seg.lsn_max = 1050;
    man_seg.file_bytes = 1048576;
    man_seg.min_pk = 1;
    man_seg.max_pk = 50000;
    man_seg.table_id = 1;
    man_seg.nrows = 8192;
    man_seg.live_rows = 8000;
    man_seg.level = 0;
    man_seg.flags = VX_SEG_FLAG_PK;

    assert(man_seg.segment_id == 42);
    assert(man_seg.del_gen == 3);
    assert(man_seg.level == 0);
    assert(man_seg.flags & VX_SEG_FLAG_PK);

    printf("      -> MANIFEST and .del sidecar headers verified successfully.\n");
}

/* Test 5: Segment Footer Layout */
static void test_seg_footer(void) {
    printf("[5/5] Testing vx_seg_footer_t layout...\n");

    vx_seg_footer_t footer;
    memset(&footer, 0, sizeof(footer));
    memcpy(footer.magic, VX_SEG_MAGIC, 8);
    footer.nchunks = 10;
    footer.file_bytes = 65536;

    assert(memcmp(footer.magic, "VXSEG02\0", 8) == 0);
    assert(footer.nchunks == 10);
    assert(footer.file_bytes == 65536);

    printf("      -> Segment footer layout verified successfully.\n");
}

int main(void) {
    printf("=== V2 Immutable Segment & Manifest Binary Format Unit Tests ===\n");
    test_struct_sizes();
    test_bloom_filter();
    test_pk_lookup();
    test_manifest_del_headers();
    test_seg_footer();
    printf("All 5 tests passed successfully!\n");
    return 0;
}

