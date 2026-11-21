#include "vx_log.h"
#include "vx_wmesh.h"
#include "vx_test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static void sleep_us(long us) {
    struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

/* ========================== LOGGER TESTS ========================== */

typedef struct {
    atomic_uint_fast64_t count;
    atomic_uint_fast64_t checksum;
    atomic_int           stall;
    atomic_int           in_sink;
} log_sink_ctx_t;

static void mock_log_sink(const vx_log_rec_t *recs, size_t count, void *user_ctx) {
    log_sink_ctx_t *ctx = (log_sink_ctx_t *)user_ctx;
    atomic_store_explicit(&ctx->in_sink, 1, memory_order_release);
    while (atomic_load_explicit(&ctx->stall, memory_order_acquire)) {
        sleep_us(500);
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum ^= (recs[i].a0 * 0x9E3779B97F4A7C15ULL) ^ recs[i].tid;
    }
    atomic_fetch_add_explicit(&ctx->count, count, memory_order_relaxed);
    atomic_fetch_xor_explicit(&ctx->checksum, sum, memory_order_relaxed);
}

static void test_log_validation_and_levels(void) {
    log_sink_ctx_t sctx = {0};
    vx_log_cfg_t bad = {0};
    VX_ASSERT(vx_log_create(NULL) == NULL);
    VX_ASSERT(vx_log_create(&bad) == NULL);

    vx_log_cfg_t cfg = {
        .max_producers = 2,
        .ring_cap_pow2 = 16,
        .min_level = VX_LOG_WARN,
        .flush_interval_us = 1000,
        .sink = mock_log_sink,
        .user_ctx = &sctx
    };
    vx_logger_t *log = vx_log_create(&cfg);
    VX_ASSERT(log != NULL);

    vx_log_tx_t *tx1 = vx_log_register(log, 1);
    vx_log_tx_t *tx2 = vx_log_register(log, 2);
    VX_ASSERT(tx1 != NULL && tx2 != NULL);
    VX_ASSERT(vx_log_register(log, 3) == NULL); /* exceeds max_producers */

    /* Below min_level -> ignored cleanly, not dropped */
    VX_ASSERT(vx_log_emit(tx1, VX_LOG_DEBUG, 1, "dbg", 1, 0, 0, 0));
    VX_ASSERT(vx_log_emit(tx1, VX_LOG_INFO,  2, "inf", 2, 0, 0, 0));

    /* At/above min_level -> emitted */
    VX_ASSERT(vx_log_emit(tx1, VX_LOG_WARN,  3, "wrn", 30, 0, 0, 0));
    VX_ASSERT(vx_log_emit(tx2, VX_LOG_ERROR, 4, "err", 40, 0, 0, 0));

    vx_log_flush(log);
    vx_log_stats_t st = vx_log_stats(log);
    VX_ASSERT(st.emitted == 2);
    VX_ASSERT(st.dropped == 0);
    VX_ASSERT(atomic_load(&sctx.count) == 2);

    vx_log_destroy(log);
}

static void test_log_nonblocking_drop(void) {
    log_sink_ctx_t sctx = {0};
    atomic_init(&sctx.stall, 1);

    vx_log_cfg_t cfg = {
        .max_producers = 1,
        .ring_cap_pow2 = 4,
        .min_level = VX_LOG_INFO,
        .flush_interval_us = 1000,
        .sink = mock_log_sink,
        .user_ctx = &sctx
    };
    vx_logger_t *log = vx_log_create(&cfg);
    VX_ASSERT(log != NULL);
    vx_log_tx_t *tx = vx_log_register(log, 7);
    VX_ASSERT(tx != NULL);

    /* Trigger consumer into stalled sink */
    VX_ASSERT(vx_log_emit(tx, VX_LOG_INFO, 1, "prime", 1, 0, 0, 0));
    while (!atomic_load_explicit(&sctx.in_sink, memory_order_acquire)) {
        sleep_us(500);
    }

    /* Blast 64 logs into 4-slot ring: MUST NOT block, MUST drop overflow */
    double t0 = vx_now_sec();
    size_t drops = 0;
    for (uint64_t i = 0; i < 64; i++) {
        if (!vx_log_emit(tx, VX_LOG_INFO, 2, "burst", i, 0, 0, 0)) {
            drops++;
        }
    }
    double dt = vx_now_sec() - t0;
    VX_ASSERT(dt < 0.05); /* non-blocking guarantee */
    VX_ASSERT(drops >= 60);

    atomic_store_explicit(&sctx.stall, 0, memory_order_release);
    vx_log_flush(log);

    vx_log_stats_t st = vx_log_stats(log);
    VX_ASSERT(st.dropped == drops);
    VX_ASSERT(st.emitted + st.dropped == 65);
    VX_ASSERT(atomic_load(&sctx.count) == st.emitted);

    vx_log_destroy(log);
}

typedef struct {
    vx_logger_t *log;
    uint32_t     tid;
    uint64_t     count;
    uint64_t     expected_xor;
} log_mt_arg_t;

static void *log_mt_worker(void *arg) {
    log_mt_arg_t *a = (log_mt_arg_t *)arg;
    vx_log_tx_t *tx = vx_log_register(a->log, a->tid);
    uint64_t x = 0;
    for (uint64_t i = 0; i < a->count; i++) {
        if (vx_log_emit(tx, VX_LOG_INFO, 10, "mt", i, 0, 0, 0)) {
            x ^= (i * 0x9E3779B97F4A7C15ULL) ^ a->tid;
        }
    }
    a->expected_xor = x;
    return NULL;
}

static void test_log_mt_concurrent(void) {
    log_sink_ctx_t sctx = {0};
    vx_log_cfg_t cfg = {
        .max_producers = 8,
        .ring_cap_pow2 = 4096,
        .min_level = VX_LOG_INFO,
        .flush_interval_us = 1000,
        .sink = mock_log_sink,
        .user_ctx = &sctx
    };
    vx_logger_t *log = vx_log_create(&cfg);
    VX_ASSERT(log != NULL);

    pthread_t th[8];
    log_mt_arg_t args[8];
    const uint64_t per_thread = 1000;

    for (int i = 0; i < 8; i++) {
        args[i] = (log_mt_arg_t){ .log = log, .tid = (uint32_t)(i + 1), .count = per_thread };
        VX_ASSERT(pthread_create(&th[i], NULL, log_mt_worker, &args[i]) == 0);
    }
    uint64_t total_xor = 0;
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        total_xor ^= args[i].expected_xor;
    }

    vx_log_flush(log);
    vx_log_stats_t st = vx_log_stats(log);
    VX_ASSERT(st.emitted + st.dropped == 8 * per_thread);
    VX_ASSERT(atomic_load(&sctx.count) == st.emitted);
    VX_ASSERT(atomic_load(&sctx.checksum) == total_xor);

    vx_log_destroy(log);
}

/* ========================== WRITE MESH TESTS ========================== */

typedef struct {
    atomic_uint_fast64_t ops;
    atomic_uint_fast64_t batches;
    atomic_uint_fast64_t last_lsn;
    atomic_uint_fast64_t key_xor;
    atomic_uint_fast64_t last_key;
    atomic_int           lsn_monotonic_ok;
    atomic_int           stall;
    atomic_int           in_commit;
} wmesh_commit_ctx_t;

static void mock_commit_fn(const vx_write_op_t *ops, size_t count,
                           uint64_t batch_lsn, void *user_ctx) {
    wmesh_commit_ctx_t *ctx = (wmesh_commit_ctx_t *)user_ctx;
    atomic_store_explicit(&ctx->in_commit, 1, memory_order_release);
    while (atomic_load_explicit(&ctx->stall, memory_order_acquire)) {
        sleep_us(500);
    }

    uint64_t prev = atomic_exchange_explicit(&ctx->last_lsn, batch_lsn, memory_order_acq_rel);
    if (batch_lsn <= prev) {
        atomic_store_explicit(&ctx->lsn_monotonic_ok, 0, memory_order_release);
    }

    uint64_t kx = 0;
    for (size_t i = 0; i < count; i++) {
        kx ^= ops[i].key;
    }
    if (count > 0) {
        atomic_store_explicit(&ctx->last_key, ops[count - 1].key, memory_order_release);
    }
    atomic_fetch_xor_explicit(&ctx->key_xor, kx, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->ops, count, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->batches, 1, memory_order_relaxed);
}

static void test_wmesh_validation(void) {
    vx_wmesh_cfg_t bad = {0};
    VX_ASSERT(vx_wmesh_create(NULL) == NULL);
    VX_ASSERT(vx_wmesh_create(&bad) == NULL);

    wmesh_commit_ctx_t cctx = { .lsn_monotonic_ok = 1 };
    vx_wmesh_cfg_t cfg = {
        .max_producers = 1,
        .ring_cap_pow2 = 16,
        .batch_max_ops = 8,
        .flush_interval_us = 2000,
        .commit_fn = mock_commit_fn,
        .user_ctx = &cctx
    };
    vx_wmesh_t *m = vx_wmesh_create(&cfg);
    VX_ASSERT(m != NULL);
    VX_ASSERT(vx_wmesh_register(m) != NULL);
    VX_ASSERT(vx_wmesh_register(m) == NULL); /* exceeds max_producers */
    vx_wmesh_destroy(m);
}

static void test_wmesh_hybrid_triggers_and_ryow(void) {
    /* 1. Latency trigger: 1 op commits automatically via flush_interval_us */
    wmesh_commit_ctx_t cctx1 = { .lsn_monotonic_ok = 1 };
    vx_wmesh_cfg_t cfg1 = {
        .max_producers = 1,
        .ring_cap_pow2 = 64,
        .batch_max_ops = 32,
        .flush_interval_us = 3000, /* 3ms */
        .commit_fn = mock_commit_fn,
        .user_ctx = &cctx1
    };
    vx_wmesh_t *m1 = vx_wmesh_create(&cfg1);
    VX_ASSERT(m1 != NULL);
    vx_wmesh_tx_t *tx1 = vx_wmesh_register(m1);

    vx_write_op_t op = { .key = 0xCAFEBABE, .table_id = 1, .op_type = VX_OP_INSERT, .len = 4 };
    uint64_t tok = 0;
    VX_ASSERT(vx_wmesh_submit(tx1, &op, &tok));
    VX_ASSERT(tok == 1);

    /* RYOW wait returns immediately once latency trigger commits */
    double t0 = vx_now_sec();
    VX_ASSERT(vx_wmesh_wait(tx1, tok));
    double dt = vx_now_sec() - t0;
    VX_ASSERT(dt < 0.2);
    VX_ASSERT(atomic_load_explicit(&cctx1.last_key, memory_order_acquire) == 0xCAFEBABE);
    VX_ASSERT(atomic_load(&cctx1.ops) == 1);
    vx_wmesh_destroy(m1);

    /* 2. Count trigger: huge 2s timer, but hitting batch_max_ops flushes fast */
    wmesh_commit_ctx_t cctx2 = { .lsn_monotonic_ok = 1 };
    vx_wmesh_cfg_t cfg2 = {
        .max_producers = 1,
        .ring_cap_pow2 = 128,
        .batch_max_ops = 32,
        .flush_interval_us = 2000000, /* 2 seconds */
        .commit_fn = mock_commit_fn,
        .user_ctx = &cctx2
    };
    vx_wmesh_t *m2 = vx_wmesh_create(&cfg2);
    VX_ASSERT(m2 != NULL);
    vx_wmesh_tx_t *tx2 = vx_wmesh_register(m2);

    double t1 = vx_now_sec();
    uint64_t last_tok = 0;
    for (uint64_t i = 1; i <= 32; i++) {
        op.key = i;
        VX_ASSERT(vx_wmesh_submit(tx2, &op, &last_tok));
    }
    /* Poll briefly without calling flush/wait to verify count trigger fired autonomously */
    while (atomic_load_explicit(&cctx2.ops, memory_order_acquire) < 32 &&
           (vx_now_sec() - t1) < 0.5) {
        sleep_us(500);
    }
    VX_ASSERT(atomic_load(&cctx2.ops) == 32);
    VX_ASSERT((vx_now_sec() - t1) < 0.5); /* far below the 2s timer */
    vx_wmesh_destroy(m2);
}

static void test_wmesh_backpressure(void) {
    wmesh_commit_ctx_t cctx = { .lsn_monotonic_ok = 1, .stall = 1 };
    vx_wmesh_cfg_t cfg = {
        .max_producers = 1,
        .ring_cap_pow2 = 4,
        .batch_max_ops = 1,
        .flush_interval_us = 1000,
        .commit_fn = mock_commit_fn,
        .user_ctx = &cctx
    };
    vx_wmesh_t *m = vx_wmesh_create(&cfg);
    VX_ASSERT(m != NULL);
    vx_wmesh_tx_t *tx = vx_wmesh_register(m);

    vx_write_op_t op = { .key = 100, .table_id = 1, .op_type = VX_OP_INSERT };
    VX_ASSERT(vx_wmesh_try_submit(tx, &op, NULL));
    while (!atomic_load_explicit(&cctx.in_commit, memory_order_acquire)) {
        sleep_us(500);
    }

    /* Ring capacity is 4; filling 4 slots succeeds, 5th MUST return false */
    for (uint64_t i = 1; i <= 4; i++) {
        op.key = 100 + i;
        VX_ASSERT(vx_wmesh_try_submit(tx, &op, NULL));
    }
    op.key = 999;
    VX_ASSERT(!vx_wmesh_try_submit(tx, &op, NULL));

    atomic_store_explicit(&cctx.stall, 0, memory_order_release);
    vx_wmesh_flush(m);
    VX_ASSERT(atomic_load(&cctx.ops) == 5);
    vx_wmesh_destroy(m);
}

typedef struct {
    vx_wmesh_t *mesh;
    uint32_t    tid;
    uint64_t    count;
    uint64_t    key_xor;
    bool        ok;
} wmesh_mt_arg_t;

static void *wmesh_mt_worker(void *arg) {
    wmesh_mt_arg_t *a = (wmesh_mt_arg_t *)arg;
    vx_wmesh_tx_t *tx = vx_wmesh_register(a->mesh);
    if (!tx) {
        a->ok = false;
        return NULL;
    }
    uint64_t kx = 0;
    uint64_t last_tok = 0;
    for (uint64_t i = 1; i <= a->count; i++) {
        uint64_t k = ((uint64_t)a->tid << 32) | i;
        vx_write_op_t op = { .key = k, .table_id = 1, .op_type = VX_OP_INSERT };
        if (!vx_wmesh_submit(tx, &op, &last_tok)) {
            a->ok = false;
            return NULL;
        }
        kx ^= k;
    }
    a->ok = vx_wmesh_wait(tx, last_tok);
    a->key_xor = kx;
    return NULL;
}

static void test_wmesh_mt_stress_and_sla(void) {
    wmesh_commit_ctx_t cctx = { .lsn_monotonic_ok = 1 };
    vx_wmesh_cfg_t cfg = {
        .max_producers = 8,
        .ring_cap_pow2 = 4096,
        .batch_max_ops = 1024,
        .flush_interval_us = 2000,
        .commit_fn = mock_commit_fn,
        .user_ctx = &cctx
    };
    vx_wmesh_t *m = vx_wmesh_create(&cfg);
    VX_ASSERT(m != NULL);

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t per_thread = 10000;
#else
    const uint64_t per_thread = 250000; /* 2M total ops across 8 threads */
#endif

    pthread_t th[8];
    wmesh_mt_arg_t args[8];
    double t0 = vx_now_sec();
    for (int i = 0; i < 8; i++) {
        args[i] = (wmesh_mt_arg_t){ .mesh = m, .tid = (uint32_t)(i + 1), .count = per_thread };
        VX_ASSERT(pthread_create(&th[i], NULL, wmesh_mt_worker, &args[i]) == 0);
    }

    uint64_t expected_xor = 0;
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        VX_ASSERT(args[i].ok);
        expected_xor ^= args[i].key_xor;
    }
    double dt = vx_now_sec() - t0;

    vx_wmesh_stats_t st = vx_wmesh_stats(m);
    const uint64_t total = 8 * per_thread;
    VX_ASSERT(st.ops_committed == total);
    VX_ASSERT(atomic_load(&cctx.ops) == total);
    VX_ASSERT(atomic_load(&cctx.key_xor) == expected_xor);
    VX_ASSERT(atomic_load(&cctx.lsn_monotonic_ok) == 1);
    VX_ASSERT(st.batches_committed > 0 && st.last_lsn == st.batches_committed);

    double mops = ((double)total / dt) / 1e6;
    printf("    [METRIC] WriteMesh 8-thread micro-batch: %.2f M ops/sec (%llu batches, %.3fs)\n",
           mops, (unsigned long long)st.batches_committed, dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* Release-build performance tripwire: >= 2.0 M ops/sec across 8 writers */
    VX_ASSERT(mops >= 2.0);
#endif

    vx_wmesh_destroy(m);
}

int main(void) {
    VX_RUN(test_log_validation_and_levels);
    VX_RUN(test_log_nonblocking_drop);
    VX_RUN(test_log_mt_concurrent);
    VX_RUN(test_wmesh_validation);
    VX_RUN(test_wmesh_hybrid_triggers_and_ryow);
    VX_RUN(test_wmesh_backpressure);
    VX_RUN(test_wmesh_mt_stress_and_sla);
    return vx_test_summary("M2 Queues: Async Logger + SPSC Write Mesh");
}

