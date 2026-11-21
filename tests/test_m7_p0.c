#include "vx_db.h"
#include "vx_ebr.h"
#include "vx_test.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_us(long us) {
    struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

static bool approx_eq(double a, double b) {
    return fabs(a - b) < 1e-6;
}

static bool str_eq_cstr(vx_str_t s, const char *cstr) {
    size_t n = strlen(cstr);
    return (size_t)s.len == n && (n == 0 || memcmp(vx_str_data(&s), cstr, n) == 0);
}

static bool exec_ok(vx_db_t *db, const char *q,
                    const vx_param_t *params, size_t nparams, bool sync) {
    vx_result_t *r = vx_db_query(db, q, params, nparams, sync);
    bool ok = r && r->ok;
    if (r && !r->ok) {
        fprintf(stderr, "    [QUERY ERR] %s\n    query: %s\n", r->err_msg, q);
    }
    vx_result_free(r);
    return ok;
}

/* ==================== P0-4: EBR SLOT RECYCLING (100+ EPHEMERAL THREADS) ==================== */

typedef struct {
    vx_db_t *db;
    bool     ok;
} ephemeral_arg_t;

static void *ephemeral_reader_fn(void *arg) {
    ephemeral_arg_t *a = (ephemeral_arg_t *)arg;
    vx_result_t *r = vx_db_query(a->db, "items { n: count() }", NULL, 0, false);
    a->ok = (r && r->ok && r->nrows == 1 && vx_result_u64(r, 0, 0) == 3);
    vx_result_free(r);
    return NULL;
}

static void test_p0_ebr_slot_recycling_128_threads(void) {
    /* Direct EBR unit check: register -> unregister -> re-register beyond max_readers */
    int dummy_root = 1;
    vx_ebr_t *ebr = vx_ebr_create(4, &dummy_root);
    VX_ASSERT(ebr != NULL);
    for (int i = 0; i < 64; i++) {
        vx_ebr_reader_t *r = vx_ebr_register_reader(ebr);
        VX_ASSERT(r != NULL);
        VX_ASSERT(vx_ebr_enter(r) == &dummy_root);
        vx_ebr_exit(r);
        vx_ebr_unregister_reader(r);
    }
    vx_ebr_destroy(ebr);

    /* DB integration check: max_readers=8, spawn 128 ephemeral threads in waves of 4 */
    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.max_readers = 8;
    cfg.worker_threads = 2;

    vx_db_t *db = vx_db_open(NULL, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table items { id: u64 key, v: i64 }", NULL, 0, true));
    VX_ASSERT(exec_ok(db, "items += [{ id: 1, v: 10 }, { id: 2, v: 20 }, { id: 3, v: 30 }]",
                      NULL, 0, true));

    for (int wave = 0; wave < 32; wave++) {
        pthread_t th[4];
        ephemeral_arg_t args[4];
        for (int i = 0; i < 4; i++) {
            args[i] = (ephemeral_arg_t){ .db = db, .ok = false };
            VX_ASSERT(pthread_create(&th[i], NULL, ephemeral_reader_fn, &args[i]) == 0);
        }
        for (int i = 0; i < 4; i++) {
            pthread_join(th[i], NULL);
            VX_ASSERT(args[i].ok);
        }
    }

    vx_db_close(db);
}

/* ==================== P0-3: CONCURRENT vx_pool_run MORSEL STRESS ==================== */

typedef struct {
    vx_db_t        *db;
    int             iters;
    const uint64_t *expect_cnt;
    const double   *expect_sum;
    bool            ok;
} pool_stress_arg_t;

static void *pool_stress_reader_fn(void *arg) {
    pool_stress_arg_t *a = (pool_stress_arg_t *)arg;
    const char *q = "metrics[val > 500.0] by bucket { n: count(), s: sum(val) } sort bucket";

    for (int it = 0; it < a->iters; it++) {
        vx_result_t *r = vx_db_query(a->db, q, NULL, 0, false);
        if (!r || !r->ok || r->nrows != 8 || r->ncols != 3) {
            vx_result_free(r);
            a->ok = false;
            return NULL;
        }
        for (uint32_t b = 0; b < 8; b++) {
            if (vx_result_i64(r, 0, b) != (int64_t)b ||
                vx_result_u64(r, 1, b) != a->expect_cnt[b] ||
                !approx_eq(vx_result_f64(r, 2, b), a->expect_sum[b])) {
                vx_result_free(r);
                a->ok = false;
                return NULL;
            }
        }
        vx_result_free(r);
    }
    a->ok = true;
    return NULL;
}

static void test_p0_concurrent_morsel_pool_stress(void) {
    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.worker_threads = 4;
    cfg.chunk_rows = 256;
    cfg.max_readers = 16;

    vx_db_t *db = vx_db_open(NULL, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table metrics { id: u64 key, bucket: i64, val: f64 }", NULL, 0, true));

    const uint64_t n_rows = 8192; /* 32 chunks across 4 pool workers */
    const char *cols[3] = { "id", "bucket", "val" };
    uint64_t expect_cnt[8] = {0};
    double   expect_sum[8] = {0.0};

    for (uint64_t i = 0; i < n_rows; i++) {
        int64_t b = (int64_t)(i & 7ULL);
        double v = (double)((i * 37ULL) % 1000ULL);
        vx_val_t vals[3] = {
            { .type = VX_TYPE_U64, .v.u64 = i + 1 },
            { .type = VX_TYPE_I64, .v.i64 = b },
            { .type = VX_TYPE_F64, .v.f64 = v }
        };
        VX_ASSERT(vx_db_put_row(db, "metrics", cols, vals, 3, false, false));
        if (v > 500.0) {
            expect_cnt[b]++;
            expect_sum[b] += v;
        }
    }
    vx_db_flush(db);

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const int iters = 15;
#else
    const int iters = 60;
#endif

    /* 8 concurrent reader threads hammering vx_pool_run simultaneously */
    pthread_t th[8];
    pool_stress_arg_t args[8];
    for (int i = 0; i < 8; i++) {
        args[i] = (pool_stress_arg_t){
            .db = db,
            .iters = iters,
            .expect_cnt = expect_cnt,
            .expect_sum = expect_sum,
            .ok = false
        };
        VX_ASSERT(pthread_create(&th[i], NULL, pool_stress_reader_fn, &args[i]) == 0);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        VX_ASSERT(args[i].ok);
    }

    vx_db_close(db);
}

/* ==================== P0-1: db.put() WAL PERSISTENCE + UPSERT DEDUP ==================== */

static void test_p0_put_row_wal_persistence_and_upsert(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/vx_m7_put_%ld.wal", (long)getpid());
    unlink(path);

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.fsync_on_commit = true;

    vx_db_t *db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db,
        "table users { id: u64 key, name: str, score: f64, active: bool }",
        NULL, 0, true));

    const char *cols[4] = { "id", "name", "score", "active" };
    vx_val_t r1[4] = {
        { .type = VX_TYPE_U64,  .v.u64 = 1 },
        { .type = VX_TYPE_STR,  .v.str = { "Aarav", 5 } },
        { .type = VX_TYPE_F64,  .v.f64 = 10.5 },
        { .type = VX_TYPE_BOOL, .v.b   = true }
    };
    vx_val_t r2[4] = {
        { .type = VX_TYPE_U64,  .v.u64 = 2 },
        { .type = VX_TYPE_STR,  .v.str = { "Meera", 5 } },
        { .type = VX_TYPE_F64,  .v.f64 = 20.0 },
        { .type = VX_TYPE_BOOL, .v.b   = false }
    };
    vx_val_t r3[4] = {
        { .type = VX_TYPE_U64,  .v.u64 = 3 },
        { .type = VX_TYPE_STR,  .is_null = true },
        { .type = VX_TYPE_F64,  .v.f64 = 30.0 },
        { .type = VX_TYPE_BOOL, .v.b   = true }
    };

    VX_ASSERT(vx_db_put_row(db, "users", cols, r1, 4, false, true));
    VX_ASSERT(vx_db_put_row(db, "users", cols, r2, 4, false, true));
    VX_ASSERT(vx_db_put_row(db, "users", cols, r3, 4, false, true));

    /* Upsert existing PK=2 with >12B string + updated score; MUST deduplicate */
    vx_val_t r2_up[4] = {
        { .type = VX_TYPE_U64,  .v.u64 = 2 },
        { .type = VX_TYPE_STR,  .v.str = { "Meera_Updated_LongName", 22 } },
        { .type = VX_TYPE_F64,  .v.f64 = 99.25 },
        { .type = VX_TYPE_BOOL, .v.b   = true }
    };
    VX_ASSERT(vx_db_put_row(db, "users", cols, r2_up, 4, true, true));

    /* Upsert new PK=4 -> inserts */
    vx_val_t r4[4] = {
        { .type = VX_TYPE_U64,  .v.u64 = 4 },
        { .type = VX_TYPE_STR,  .v.str = { "Zara", 4 } },
        { .type = VX_TYPE_F64,  .v.f64 = 40.0 },
        { .type = VX_TYPE_BOOL, .v.b   = true }
    };
    VX_ASSERT(vx_db_put_row(db, "users", cols, r4, 4, true, true));

    /* In-memory dedup check */
    vx_result_t *res = vx_db_query(db, "users id, name, score, active sort id", NULL, 0, false);
    VX_ASSERT(res && res->ok && res->nrows == 4);
    VX_ASSERT(str_eq_cstr(vx_result_str(res, 1, 1), "Meera_Updated_LongName"));
    VX_ASSERT(approx_eq(vx_result_f64(res, 2, 1), 99.25));
    VX_ASSERT(vx_result_bool(res, 3, 1) == true);
    VX_ASSERT(vx_result_is_null(res, 1, 2));
    vx_result_free(res);

    vx_db_close(db);

    /* Reopen from WAL: every db.put() + upsert MUST survive intact */
    db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    res = vx_db_query(db, "users id, name, score, active sort id", NULL, 0, false);
    VX_ASSERT(res && res->ok && res->nrows == 4);
    VX_ASSERT(vx_result_u64(res, 0, 0) == 1 && str_eq_cstr(vx_result_str(res, 1, 0), "Aarav"));
    VX_ASSERT(vx_result_u64(res, 0, 1) == 2 && str_eq_cstr(vx_result_str(res, 1, 1), "Meera_Updated_LongName"));
    VX_ASSERT(approx_eq(vx_result_f64(res, 2, 1), 99.25) && vx_result_bool(res, 3, 1) == true);
    VX_ASSERT(vx_result_u64(res, 0, 2) == 3 && vx_result_is_null(res, 1, 2));
    VX_ASSERT(vx_result_u64(res, 0, 3) == 4 && str_eq_cstr(vx_result_str(res, 1, 3), "Zara"));
    vx_result_free(res);

    vx_db_close(db);
    unlink(path);
}

/* ==================== P0-2: PARAMETERIZED WRITES WAL REPLAY ==================== */

static void test_p0_parameterized_writes_wal_replay(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/vx_m7_param_%ld.wal", (long)getpid());
    unlink(path);

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.fsync_on_commit = true;

    vx_db_t *db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db,
        "table orders { id: u64 key, note: str, amount: f64, paid: bool }",
        NULL, 0, true));

    vx_param_t p1[4] = {
        { "id",   { .type = VX_TYPE_U64,  .v.u64 = 10 } },
        { "note", { .type = VX_TYPE_STR,  .v.str = { "first_order_paid", 16 } } },
        { "amt",  { .type = VX_TYPE_F64,  .v.f64 = 120.0 } },
        { "paid", { .type = VX_TYPE_BOOL, .v.b   = true } }
    };
    VX_ASSERT(exec_ok(db,
        "orders += { id: $id, note: $note, amount: $amt, paid: $paid }",
        p1, 4, true));

    vx_param_t p2[4] = {
        { "id",   { .type = VX_TYPE_U64,  .v.u64 = 11 } },
        { "note", { .type = VX_TYPE_STR,  .v.str = { "small", 5 } } },
        { "amt",  { .type = VX_TYPE_F64,  .v.f64 = 15.0 } },
        { "paid", { .type = VX_TYPE_BOOL, .v.b   = false } }
    };
    VX_ASSERT(exec_ok(db,
        "orders |= { id: $id, note: $note, amount: $amt, paid: $paid }",
        p2, 4, true));

    /* Parameterized update */
    vx_param_t pu[2] = {
        { "id",    { .type = VX_TYPE_U64, .v.u64 = 10 } },
        { "delta", { .type = VX_TYPE_F64, .v.f64 = 30.5 } }
    };
    VX_ASSERT(exec_ok(db, "orders[$id] = { amount: amount + $delta }", pu, 2, true));

    /* Parameterized delete */
    vx_param_t pd[1] = {
        { "cutoff", { .type = VX_TYPE_F64, .v.f64 = 50.0 } }
    };
    VX_ASSERT(exec_ok(db, "del orders[amount < $cutoff]", pd, 1, true));

    vx_db_close(db);

    /* Reopen: WAL replay has NO external params and MUST recover exact resolved state */
    db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    vx_result_t *res = vx_db_query(db, "orders id, note, amount, paid", NULL, 0, false);
    VX_ASSERT(res && res->ok && res->nrows == 1);
    VX_ASSERT(vx_result_u64(res, 0, 0) == 10);
    VX_ASSERT(str_eq_cstr(vx_result_str(res, 1, 0), "first_order_paid"));
    VX_ASSERT(approx_eq(vx_result_f64(res, 2, 0), 150.5));
    VX_ASSERT(vx_result_bool(res, 3, 0) == true);
    vx_result_free(res);

    vx_db_close(db);
    unlink(path);
}

/* ==================== P0-5: BACKGROUND SPSC WRITE-MESH AUTO-FLUSH + LOGGER ==================== */

static void test_log_sink(const vx_log_rec_t *recs, size_t count, void *user_ctx) {
    (void)recs;
    atomic_uint_fast64_t *cnt = (atomic_uint_fast64_t *)user_ctx;
    atomic_fetch_add_explicit(cnt, count, memory_order_relaxed);
}

typedef struct {
    vx_db_t *db;
    uint32_t tid;
    uint64_t count;
    bool     ok;
} mt_put_arg_t;

static void *mt_put_worker_fn(void *arg) {
    mt_put_arg_t *a = (mt_put_arg_t *)arg;
    const char *cols[2] = { "id", "px" };
    for (uint64_t i = 1; i <= a->count; i++) {
        uint64_t id = ((uint64_t)a->tid << 32) | i;
        vx_val_t vals[2] = {
            { .type = VX_TYPE_U64, .v.u64 = id },
            { .type = VX_TYPE_F64, .v.f64 = (double)i }
        };
        /* Only the very last write per thread asks for sync=true (RYOW) */
        bool sync = (i == a->count);
        if (!vx_db_put_row(a->db, "ticks", cols, vals, 2, false, sync)) {
            a->ok = false;
            return NULL;
        }
    }
    a->ok = true;
    return NULL;
}

static void test_p0_wmesh_auto_flush_mt_writers_and_logger(void) {
    atomic_uint_fast64_t sink_events = 0;

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.flush_interval_us = 3000; /* 3ms background auto-flush */
    cfg.log_sink = test_log_sink;
    cfg.log_ctx = &sink_events;

    vx_db_t *db = vx_db_open(NULL, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table ticks { id: u64 key, px: f64 }", NULL, 0, true));

    /* 1. Submit 5 async puts (far below batch threshold) and NEVER call vx_db_flush() */
    const char *cols[2] = { "id", "px" };
    for (uint64_t i = 1; i <= 5; i++) {
        vx_val_t vals[2] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_F64, .v.f64 = (double)i * 10.0 }
        };
        VX_ASSERT(vx_db_put_row(db, "ticks", cols, vals, 2, false, false));
    }

    /* Background SPSC write-mesh latency trigger MUST commit them within 250ms */
    uint64_t seen = 0;
    double t0 = vx_now_sec();
    while ((vx_now_sec() - t0) < 0.25) {
        vx_result_t *r = vx_db_query(db, "ticks { n: count() }", NULL, 0, false);
        if (r && r->ok && r->nrows == 1) {
            seen = vx_result_u64(r, 0, 0);
        }
        vx_result_free(r);
        if (seen == 5) break;
        sleep_us(2000);
    }
    VX_ASSERT(seen == 5);

    /* 2. 4 concurrent producer threads pushing into per-thread SPSC write mesh */
    pthread_t th[4];
    mt_put_arg_t args[4];
    const uint64_t per_thread = 100;
    for (int i = 0; i < 4; i++) {
        args[i] = (mt_put_arg_t){ .db = db, .tid = (uint32_t)(i + 1), .count = per_thread, .ok = false };
        VX_ASSERT(pthread_create(&th[i], NULL, mt_put_worker_fn, &args[i]) == 0);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        VX_ASSERT(args[i].ok);
    }

    vx_result_t *r = vx_db_query(db, "ticks { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && vx_result_u64(r, 0, 0) == 5 + 4 * per_thread);
    vx_result_free(r);

    /* 3. Verify async SPSC binary logger emitted engine telemetry */
    vx_db_stats_t st = vx_db_stats(db);
    VX_ASSERT(st.logs_emitted > 0);
    VX_ASSERT(atomic_load(&sink_events) > 0);

    vx_db_close(db);
}

int main(void) {
    VX_RUN(test_p0_ebr_slot_recycling_128_threads);
    VX_RUN(test_p0_concurrent_morsel_pool_stress);
    VX_RUN(test_p0_put_row_wal_persistence_and_upsert);
    VX_RUN(test_p0_parameterized_writes_wal_replay);
    VX_RUN(test_p0_wmesh_auto_flush_mt_writers_and_logger);
    return vx_test_summary("M7 P0 Hardening: EBR + Pool + WAL + Upsert + WriteMesh");
}

