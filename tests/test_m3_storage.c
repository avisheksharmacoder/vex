#include "vx_chunk.h"
#include "vx_ebr.h"
#include "vx_str.h"
#include "vx_test.h"
#include "vx_wal.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ========================== 1. GERMAN STRINGS ========================== */

static void test_str_inline_and_heap_boundaries(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 4096));

    /* Lengths 0, 4, 12 stay inline and succeed even with NULL arena */
    vx_str_t s0, s4, s12;
    VX_ASSERT(vx_str_make(NULL, "", 0, &s0));
    VX_ASSERT(vx_str_is_inline(&s0) && s0.len == 0);

    VX_ASSERT(vx_str_from_cstr(NULL, "paid", &s4));
    VX_ASSERT(vx_str_is_inline(&s4) && s4.len == 4);
    VX_ASSERT(memcmp(vx_str_data(&s4), "paid", 4) == 0);

    VX_ASSERT(vx_str_from_cstr(NULL, "123456789012", &s12));
    VX_ASSERT(vx_str_is_inline(&s12) && s12.len == 12);
    VX_ASSERT(memcmp(vx_str_data(&s12), "123456789012", 12) == 0);
    VX_ASSERT(vx_arena_used(&arena) == 0);

    /* Length 13 requires arena; NULL arena must fail cleanly */
    vx_str_t s13;
    VX_ASSERT(!vx_str_from_cstr(NULL, "1234567890123", &s13));
    VX_ASSERT(vx_str_from_cstr(&arena, "1234567890123", &s13));
    VX_ASSERT(!vx_str_is_inline(&s13) && s13.len == 13);
    VX_ASSERT(memcmp(s13.prefix, "1234", 4) == 0);
    VX_ASSERT(memcmp(vx_str_data(&s13), "1234567890123", 13) == 0);
    VX_ASSERT(vx_arena_used(&arena) >= 13);

    vx_arena_destroy(&arena);
}

static void test_str_eq_cmp_hash_and_binary_safety(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 4096));

    vx_str_t a, b, c, long_a, long_b, long_c;
    VX_ASSERT(vx_str_from_cstr(&arena, "alpha", &a));
    VX_ASSERT(vx_str_from_cstr(&arena, "alpha", &b));
    VX_ASSERT(vx_str_from_cstr(&arena, "alphb", &c));

    VX_ASSERT(vx_str_eq(&a, &b));
    VX_ASSERT(!vx_str_eq(&a, &c));
    VX_ASSERT(vx_str_cmp(&a, &b) == 0);
    VX_ASSERT(vx_str_cmp(&a, &c) < 0);
    VX_ASSERT(vx_str_cmp(&c, &a) > 0);
    VX_ASSERT(vx_str_hash(&a) == vx_str_hash(&b));
    VX_ASSERT(vx_str_hash(&a) != vx_str_hash(&c));

    /* Same 4-byte prefix, different tail > 12 bytes */
    VX_ASSERT(vx_str_from_cstr(&arena, "bengaluru_north_01", &long_a));
    VX_ASSERT(vx_str_from_cstr(&arena, "bengaluru_north_01", &long_b));
    VX_ASSERT(vx_str_from_cstr(&arena, "bengaluru_north_02", &long_c));

    VX_ASSERT(vx_str_eq(&long_a, &long_b));
    VX_ASSERT(!vx_str_eq(&long_a, &long_c));
    VX_ASSERT(vx_str_cmp(&long_a, &long_c) < 0);
    VX_ASSERT(vx_str_cmp(&long_c, &long_a) > 0);
    VX_ASSERT(vx_str_hash(&long_a) == vx_str_hash(&long_b));

    /* Embedded NUL bytes must be preserved and compared */
    const char bin1[5] = {'a', '\0', 'b', 'c', 'd'};
    const char bin2[5] = {'a', '\0', 'b', 'c', 'e'};
    vx_str_t nb1, nb2;
    VX_ASSERT(vx_str_make(NULL, bin1, 5, &nb1));
    VX_ASSERT(vx_str_make(NULL, bin2, 5, &nb2));
    VX_ASSERT(!vx_str_eq(&nb1, &nb2));
    VX_ASSERT(vx_str_cmp(&nb1, &nb2) < 0);

    vx_arena_destroy(&arena);
}

/* ==================== 2. CHUNKS, ZONE MAPS & SIMD ==================== */

static void test_chunk_alignment_nulls_and_zonemaps(void) {
    const vx_type_t types[3] = { VX_TYPE_U64, VX_TYPE_F64, VX_TYPE_STR };
    vx_chunk_t *ch = vx_chunk_create(1, 3, types, 128, 4096);
    VX_ASSERT(ch != NULL);

    for (uint32_t c = 0; c < 3; c++) {
        VX_ASSERT(((uintptr_t)ch->cols[c].data % 64) == 0);
        VX_ASSERT(((uintptr_t)ch->cols[c].null_bits % 64) == 0);
    }

    /* Empty chunk cannot match any numeric predicate */
    VX_ASSERT(!vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_GT, 0.0));

    const char *statuses[4] = { "paid", "pending", "paid", "very_long_status_refunded" };
    for (uint32_t i = 0; i < 4; i++) {
        vx_val_t row[3] = {
            { .type = VX_TYPE_U64, .is_null = false, .v.u64 = i + 1 },
            { .type = VX_TYPE_F64, .is_null = (i == 2), .v.f64 = 100.0 + (double)i * 25.0 },
            { .type = VX_TYPE_STR, .is_null = false,
              .v.str = { statuses[i], (uint32_t)strlen(statuses[i]) } }
        };
        VX_ASSERT(vx_chunk_push_row(ch, row));
    }

    VX_ASSERT(ch->nrows == 4);
    VX_ASSERT(vx_col_is_null(&ch->cols[1], 2));
    VX_ASSERT(!vx_col_is_null(&ch->cols[1], 0));
    VX_ASSERT(ch->cols[1].zone.null_count == 1);
    VX_ASSERT(ch->cols[1].zone.has_bounds);
    /* Non-null f64 values are 100.0, 125.0, 175.0 */
    VX_ASSERT(ch->cols[1].zone.b.f64.min == 100.0);
    VX_ASSERT(ch->cols[1].zone.b.f64.max == 175.0);

    /* Zone map pruning */
    VX_ASSERT(!vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_GT, 175.0));
    VX_ASSERT( vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_GE, 175.0));
    VX_ASSERT(!vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_LT, 100.0));
    VX_ASSERT( vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_LE, 100.0));
    VX_ASSERT(!vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_EQ, 200.0));
    VX_ASSERT( vx_zone_may_match_f64(&ch->cols[1].zone, VX_CMP_EQ, 125.0));

    /* Filter f64 > 110.0 -> rows 1 (125.0) and 3 (175.0); row 2 is NULL and excluded */
    uint32_t sel[4] = {0};
    uint32_t n = vx_chunk_filter_f64(ch, 1, VX_CMP_GT, 110.0, sel);
    VX_ASSERT(n == 2 && sel[0] == 1 && sel[1] == 3);

    /* Zone-map fast skip returns 0 immediately */
    VX_ASSERT(vx_chunk_filter_f64(ch, 1, VX_CMP_GT, 500.0, sel) == 0);

    /* String filter for "paid" -> rows 0 and 2 */
    vx_str_t needle;
    VX_ASSERT(vx_str_from_cstr(NULL, "paid", &needle));
    uint32_t sn = vx_filter_str_eq((const vx_str_t *)ch->cols[2].data,
                                   ch->cols[2].null_bits, ch->nrows, &needle, sel);
    VX_ASSERT(sn == 2 && sel[0] == 0 && sel[1] == 2);

    vx_chunk_destroy(ch);
}

static void test_simd_filter_correctness_and_sla(void) {
    const uint32_t n_rows = 65536;
    const vx_type_t types[2] = { VX_TYPE_I64, VX_TYPE_F64 };
    vx_chunk_t *ch = vx_chunk_create(2, 2, types, n_rows, 64);
    VX_ASSERT(ch != NULL);

    uint32_t expected_gt = 0;
    for (uint32_t i = 0; i < n_rows; i++) {
        double v = (double)((i * 37U) % 1000U);
        vx_val_t row[2] = {
            { .type = VX_TYPE_I64, .is_null = false, .v.i64 = (int64_t)i - 32768 },
            { .type = VX_TYPE_F64, .is_null = false, .v.f64 = v }
        };
        VX_ASSERT(vx_chunk_push_row(ch, row));
        if (v > 500.0) expected_gt++;
    }

    uint32_t *sel = (uint32_t *)malloc(n_rows * sizeof(uint32_t));
    VX_ASSERT(sel != NULL);

    /* Verify i64 filter bounds */
    uint32_t neg_n = vx_filter_i64((const int64_t *)ch->cols[0].data, NULL,
                                   n_rows, VX_CMP_LT, 0, sel);
    VX_ASSERT(neg_n == 32768);
    VX_ASSERT(sel[0] == 0 && sel[32767] == 32767);

    /* Benchmark branchless f64 filter on unmasked column */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const int iters = 16;
#else
    const int iters = 256; /* 16.7M rows total */
#endif

    const double *col = (const double *)ch->cols[1].data;
    uint32_t matched = 0;
    double t0 = vx_now_sec();
    for (int it = 0; it < iters; it++) {
        matched = vx_filter_f64(col, NULL, n_rows, VX_CMP_GT, 500.0, sel);
    }
    double dt = vx_now_sec() - t0;

    VX_ASSERT(matched == expected_gt);
    double mrows = ((double)n_rows * (double)iters / dt) / 1e6;
    printf("    [METRIC] Branchless f64 SIMD filter: %.1f M rows/sec (%.4fs)\n", mrows, dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* Release-build SLA tripwire: >= 500M rows/sec single-core */
    VX_ASSERT(mrows >= 500.0);
#endif

    free(sel);
    vx_chunk_destroy(ch);
}

/* ==================== 3. MVCC EPOCH RECLAMATION ==================== */

typedef struct {
    uint64_t version;
    uint64_t magic;
    uint64_t payload[6];
} mock_snap_t;

static atomic_uint_fast64_t g_ebr_freed = 0;

static void mock_snap_free(void *ptr, void *user_ctx) {
    mock_snap_t *s = (mock_snap_t *)ptr;
    (void)user_ctx;
    /* Poison before free so any reader UAF triggers immediately */
    s->magic = 0xDEADDEADDEADDEADULL;
    free(s);
    atomic_fetch_add_explicit(&g_ebr_freed, 1, memory_order_relaxed);
}

static mock_snap_t *make_snap(uint64_t ver) {
    mock_snap_t *s = (mock_snap_t *)malloc(sizeof(mock_snap_t));
    s->version = ver;
    s->magic = 0x5658534E41505631ULL ^ ver;
    for (int i = 0; i < 6; i++) s->payload[i] = ver + (uint64_t)i;
    return s;
}

static void test_ebr_pin_prevents_premature_free(void) {
    atomic_store(&g_ebr_freed, 0);
    mock_snap_t *v0 = make_snap(0);
    vx_ebr_t *ebr = vx_ebr_create(4, v0);
    VX_ASSERT(ebr != NULL);

    vx_ebr_reader_t *r0 = vx_ebr_register_reader(ebr);
    VX_ASSERT(r0 != NULL);

    /* Reader pins epoch 0 */
    mock_snap_t *seen = (mock_snap_t *)vx_ebr_enter(r0);
    VX_ASSERT(seen == v0 && seen->version == 0);

    /* Writer publishes v1 and retires v0 */
    mock_snap_t *v1 = make_snap(1);
    void *old = vx_ebr_publish(ebr, v1);
    VX_ASSERT(old == v0);
    VX_ASSERT(vx_ebr_retire(ebr, old, mock_snap_free, NULL));

    /* Reclaim MUST NOT free v0 while r0 is pinned at epoch 0 */
    VX_ASSERT(vx_ebr_reclaim(ebr) == 0);
    VX_ASSERT(atomic_load(&g_ebr_freed) == 0);
    VX_ASSERT(seen->magic == 0x5658534E41505631ULL);

    /* Reader exits -> v0 becomes reclaimable immediately */
    vx_ebr_exit(r0);
    VX_ASSERT(vx_ebr_reclaim(ebr) == 1);
    VX_ASSERT(atomic_load(&g_ebr_freed) == 1);

    /* Free final live root after destroy */
    void *live = vx_ebr_current_root(ebr);
    vx_ebr_destroy(ebr);
    free(live);
}

typedef struct {
    vx_ebr_t   *ebr;
    atomic_int *stop;
    uint64_t    reads;
    bool        ok;
} ebr_reader_arg_t;

static void *ebr_reader_worker(void *arg) {
    ebr_reader_arg_t *a = (ebr_reader_arg_t *)arg;
    vx_ebr_reader_t *r = vx_ebr_register_reader(a->ebr);
    if (!r) {
        a->ok = false;
        return NULL;
    }

    uint64_t last_ver = 0;
    while (!atomic_load_explicit(a->stop, memory_order_acquire)) {
        mock_snap_t *s = (mock_snap_t *)vx_ebr_enter(r);
        uint64_t ver = s->version;
        uint64_t expect_magic = 0x5658534E41505631ULL ^ ver;
        if (s->magic != expect_magic || ver < last_ver || s->payload[5] != ver + 5) {
            vx_ebr_exit(r);
            a->ok = false;
            return NULL;
        }
        last_ver = ver;
        a->reads++;
        vx_ebr_exit(r);
    }
    a->ok = true;
    return NULL;
}

static void test_ebr_mt_mvcc_stress(void) {
    atomic_store(&g_ebr_freed, 0);
    mock_snap_t *init = make_snap(0);
    vx_ebr_t *ebr = vx_ebr_create(4, init);
    VX_ASSERT(ebr != NULL);

    atomic_int stop = 0;
    pthread_t th[4];
    ebr_reader_arg_t args[4];
    for (int i = 0; i < 4; i++) {
        args[i] = (ebr_reader_arg_t){ .ebr = ebr, .stop = &stop, .reads = 0, .ok = false };
        VX_ASSERT(pthread_create(&th[i], NULL, ebr_reader_worker, &args[i]) == 0);
    }

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t updates = 4000;
#else
    const uint64_t updates = 25000;
#endif

    for (uint64_t v = 1; v <= updates; v++) {
        void *old = vx_ebr_publish(ebr, make_snap(v));
        VX_ASSERT(vx_ebr_retire(ebr, old, mock_snap_free, NULL));
        if ((v & 31U) == 0) {
            vx_ebr_reclaim(ebr);
        }
    }

    atomic_store_explicit(&stop, 1, memory_order_release);
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        VX_ASSERT(args[i].ok);
    }

    vx_ebr_reclaim(ebr);
    vx_ebr_stats_t st = vx_ebr_stats(ebr);
    VX_ASSERT(st.epoch == updates);
    VX_ASSERT(st.pending_retired == 0);
    VX_ASSERT(atomic_load(&g_ebr_freed) == updates);

    void *live = vx_ebr_current_root(ebr);
    vx_ebr_destroy(ebr);
    free(live);
}

/* ==================== 4. DETERMINISTIC WAL & RECOVERY ==================== */

typedef struct {
    uint64_t count;
    uint64_t last_lsn;
    uint64_t payload_xor;
    bool     ok;
} wal_replay_ctx_t;

static bool test_replay_cb(const vx_wal_rec_t *rec, void *user_ctx) {
    wal_replay_ctx_t *ctx = (wal_replay_ctx_t *)user_ctx;
    if (rec->lsn != ctx->last_lsn + 1) {
        ctx->ok = false;
        return false;
    }
    ctx->last_lsn = rec->lsn;
    ctx->count++;
    for (uint32_t i = 0; i < rec->payload_len; i++) {
        ctx->payload_xor ^= ((uint64_t)rec->payload[i] << ((i & 7U) * 8U));
    }
    return true;
}

static void test_wal_append_replay_and_crash_recovery(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/vx_test_wal_%ld.wal", (long)getpid());
    unlink(path);

    /* Verify deterministic IEEE CRC32 standard test vector ("123456789" -> 0xCBF43926) */
    VX_ASSERT(vx_crc32("123456789", 9) == 0xCBF43926U);

    vx_wal_recovery_t rec_open = {0};
    vx_wal_t *wal = vx_wal_open(path, &rec_open);
    VX_ASSERT(wal != NULL);
    VX_ASSERT(rec_open.records_replayed == 0 && !rec_open.had_corrupt_tail);

    /* Append batch of 10 records */
    uint8_t payloads[10][16];
    vx_wal_rec_t batch[10];
    uint64_t expected_xor_9 = 0;
    uint64_t expected_xor_10 = 0;

    for (uint32_t i = 0; i < 10; i++) {
        memset(payloads[i], (int)(0xA0 + i), sizeof(payloads[i]));
        batch[i] = (vx_wal_rec_t){
            .lsn = i + 1,
            .table_id = 7,
            .op_type = 1,
            .flags = 0,
            .payload_len = 16,
            .payload = payloads[i]
        };
        uint64_t rx = 0;
        for (uint32_t b = 0; b < 16; b++) {
            rx ^= ((uint64_t)payloads[i][b] << ((b & 7U) * 8U));
        }
        expected_xor_10 ^= rx;
        if (i < 9) expected_xor_9 ^= rx;
    }

    VX_ASSERT(vx_wal_append_batch(wal, batch, 10, true));
    VX_ASSERT(vx_wal_last_lsn(wal) == 10);

    /* Non-monotonic LSN must be rejected */
    vx_wal_rec_t bad_lsn = batch[0];
    bad_lsn.lsn = 10;
    VX_ASSERT(!vx_wal_append_batch(wal, &bad_lsn, 1, false));
    vx_wal_close(wal);

    /* Clean replay of all 10 records */
    wal_replay_ctx_t rctx = { .ok = true };
    vx_wal_recovery_t rinfo = {0};
    VX_ASSERT(vx_wal_replay(path, test_replay_cb, &rctx, &rinfo));
    VX_ASSERT(rctx.ok && rctx.count == 10 && rctx.last_lsn == 10);
    VX_ASSERT(rctx.payload_xor == expected_xor_10);
    VX_ASSERT(!rinfo.had_corrupt_tail && rinfo.truncated_bytes == 0);

    /* Simulate bit-flip corruption in the 10th record's payload (last byte of file) */
    int fd = open(path, O_RDWR);
    VX_ASSERT(fd >= 0);
    off_t fsize = lseek(fd, 0, SEEK_END);
    VX_ASSERT(fsize > 32);
    uint8_t last_byte = 0;
    VX_ASSERT(pread(fd, &last_byte, 1, fsize - 1) == 1);
    last_byte ^= 0xFF;
    VX_ASSERT(pwrite(fd, &last_byte, 1, fsize - 1) == 1);
    close(fd);

    wal_replay_ctx_t corrupt_ctx = { .ok = true };
    vx_wal_recovery_t corrupt_info = {0};
    VX_ASSERT(vx_wal_replay(path, test_replay_cb, &corrupt_ctx, &corrupt_info));
    VX_ASSERT(corrupt_ctx.ok && corrupt_ctx.count == 9 && corrupt_ctx.last_lsn == 9);
    VX_ASSERT(corrupt_ctx.payload_xor == expected_xor_9);
    VX_ASSERT(corrupt_info.had_corrupt_tail && corrupt_info.truncated_bytes > 0);

    /* Simulate torn write (truncate 7 bytes off the tail) */
    VX_ASSERT(truncate(path, fsize - 7) == 0);

    /* Reopen heals torn tail automatically, then allows appending new LSN 10 */
    vx_wal_recovery_t heal_info = {0};
    wal = vx_wal_open(path, &heal_info);
    VX_ASSERT(wal != NULL);
    VX_ASSERT(heal_info.had_corrupt_tail);
    VX_ASSERT(heal_info.last_valid_lsn == 9);
    VX_ASSERT(vx_wal_last_lsn(wal) == 9);

    VX_ASSERT(vx_wal_append_batch(wal, &batch[9], 1, true));
    VX_ASSERT(vx_wal_last_lsn(wal) == 10);
    vx_wal_close(wal);

    /* Final replay confirms all 10 valid records recovered cleanly */
    wal_replay_ctx_t final_ctx = { .ok = true };
    vx_wal_recovery_t final_info = {0};
    VX_ASSERT(vx_wal_replay(path, test_replay_cb, &final_ctx, &final_info));
    VX_ASSERT(final_ctx.ok && final_ctx.count == 10 && final_ctx.last_lsn == 10);
    VX_ASSERT(final_ctx.payload_xor == expected_xor_10);
    VX_ASSERT(!final_info.had_corrupt_tail);

    unlink(path);
}

int main(void) {
    VX_RUN(test_str_inline_and_heap_boundaries);
    VX_RUN(test_str_eq_cmp_hash_and_binary_safety);
    VX_RUN(test_chunk_alignment_nulls_and_zonemaps);
    VX_RUN(test_simd_filter_correctness_and_sla);
    VX_RUN(test_ebr_pin_prevents_premature_free);
    VX_RUN(test_ebr_mt_mvcc_stress);
    VX_RUN(test_wal_append_replay_and_crash_recovery);
    return vx_test_summary("M3 Storage Core: Strings + Chunks/SIMD + EBR + WAL");
}

