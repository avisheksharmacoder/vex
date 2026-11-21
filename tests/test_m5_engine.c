#include "vx_db.h"
#include "vx_test.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool str_eq_cstr(vx_str_t s, const char *cstr) {
    size_t n = strlen(cstr);
    return (size_t)s.len == n && (n == 0 || memcmp(vx_str_data(&s), cstr, n) == 0);
}

static bool approx_eq(double a, double b) {
    return fabs(a - b) < 1e-6;
}

static bool exec_ok(vx_db_t *db, const char *q, bool sync) {
    vx_result_t *r = vx_db_query(db, q, NULL, 0, sync);
    bool ok = r && r->ok;
    if (r && !r->ok) {
        fprintf(stderr, "    [QUERY ERR] %s\n    query: %s\n", r->err_msg, q);
    }
    vx_result_free(r);
    return ok;
}

/* ==================== 1. DDL, CRUD, PK & FAST PUT ==================== */

static void test_ddl_crud_pk_and_fast_put(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);

    VX_ASSERT(exec_ok(db,
        "table users { id: u64 key, name: str, age: i64, spend: f64, active: bool }",
        true));

    /* Single & batch inserts */
    VX_ASSERT(exec_ok(db,
        "users += { id: 1, name: \"Aarav\", age: 29, spend: 420.5, active: true }",
        true));
    VX_ASSERT(exec_ok(db,
        "users += ["
        "  { id: 2, name: \"Meera\", age: 31, spend: 100.0, active: true },"
        "  { id: 3, name: \"Kabir\", age: 17, spend: 50.0, active: false },"
        "  { id: 4, name: \"Rohan_Long_Name_Over12\", age: 25, spend: 300.0, active: true }"
        "]", true));

    /* Bare table scan returns all rows and columns */
    vx_result_t *r = vx_db_query(db, "users", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 4 && r->ncols == 5);
    vx_result_free(r);

    /* PK lookup + column projection */
    r = vx_db_query(db, "users[4] name, spend", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && r->ncols == 2);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "Rohan_Long_Name_Over12"));
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 300.0));
    vx_result_free(r);

    /* Upsert (|=) updates existing PK and inserts new PK */
    VX_ASSERT(exec_ok(db,
        "users |= { id: 2, name: \"Meera\", age: 32, spend: 999.5, active: true }",
        true));
    VX_ASSERT(exec_ok(db,
        "users |= { id: 5, name: \"Zara\", age: 28, spend: 75.0, active: true }",
        true));

    r = vx_db_query(db, "users[2] age, spend", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(vx_result_i64(r, 0, 0) == 32);
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 999.5));
    vx_result_free(r);

    /* Update with expression referencing existing column */
    VX_ASSERT(exec_ok(db, "users[1] = { spend: spend + 50.0, age: 30 }", true));
    r = vx_db_query(db, "users[1] age, spend", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(vx_result_i64(r, 0, 0) == 30);
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 470.5));
    vx_result_free(r);

    /* Fast structured put_row (bypasses parser) */
    const char *cnames[5] = { "id", "name", "age", "spend", "active" };
    vx_val_t vals[5] = {
        { .type = VX_TYPE_U64,  .v.u64 = 6 },
        { .type = VX_TYPE_STR,  .v.str = { "Dev", 3 } },
        { .type = VX_TYPE_I64,  .v.i64 = 40 },
        { .type = VX_TYPE_F64,  .v.f64 = 120.0 },
        { .type = VX_TYPE_BOOL, .v.b   = true }
    };
    VX_ASSERT(vx_db_put_row(db, "users", cnames, vals, 5, false, true));

    /* Delete by predicate & by PK -= */
    VX_ASSERT(exec_ok(db, "del users[age < 18]", true));
    VX_ASSERT(exec_ok(db, "users[5] -=", true));

    r = vx_db_query(db, "users id sort id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 4);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 1);
    VX_ASSERT(vx_result_u64(r, 0, 1) == 2);
    VX_ASSERT(vx_result_u64(r, 0, 2) == 4);
    VX_ASSERT(vx_result_u64(r, 0, 3) == 6);
    vx_result_free(r);

    /* Drop table */
    VX_ASSERT(exec_ok(db, "del table users", true));
    r = vx_db_query(db, "users", NULL, 0, false);
    VX_ASSERT(r && !r->ok && r->err_msg[0] != '\0');
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== 2. FILTERS, PARAMS, COMPUTED PROJ, SORT & SLICE ==================== */

static void test_filters_params_sort_slice_and_explain(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);

    VX_ASSERT(exec_ok(db,
        "table items { id: u64 key, cat: str, price: f64, qty: i64, active: bool }",
        true));
    VX_ASSERT(exec_ok(db,
        "items += ["
        "  { id: 1, cat: \"cpu\", price: 200.0, qty: 2, active: true },"
        "  { id: 2, cat: \"gpu\", price: 500.0, qty: 1, active: true },"
        "  { id: 3, cat: \"ram\", price: 80.0,  qty: 4, active: true },"
        "  { id: 4, cat: \"cpu\", price: 300.0, qty: 3, active: true },"
        "  { id: 5, cat: \"ssd\", price: 150.0, qty: 2, active: false }"
        "]", true));

    vx_param_t params[2] = {
        { .name = "min_price", .val = { .type = VX_TYPE_F64, .v.f64 = 100.0 } },
        { .name = "skip_cat",  .val = { .type = VX_TYPE_STR, .v.str = { "ram", 3 } } }
    };

    /* Filter with $params, IN list, computed alias, descending sort, and slice [1:3] */
    vx_result_t *r = vx_db_query(db,
        "items[active, price >= $min_price, cat != $skip_cat, cat in [\"cpu\", \"gpu\"]] "
        "id, total: price * qty sort -total, id [0:2]",
        params, 2, false);
    VX_ASSERT(r && r->ok && r->nrows == 2 && r->ncols == 2);
    /* totals: id=4 -> 900, id=2 -> 500, id=1 -> 400 */
    VX_ASSERT(vx_result_u64(r, 0, 0) == 4 && approx_eq(vx_result_f64(r, 1, 0), 900.0));
    VX_ASSERT(vx_result_u64(r, 0, 1) == 2 && approx_eq(vx_result_f64(r, 1, 1), 500.0));
    vx_result_free(r);

    /* Explain (?) returns plan string without crashing */
    r = vx_db_query(db, "? items[price > 100.0] id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->explain_text != NULL && strlen(r->explain_text) > 0);
    vx_result_free(r);

    /* Unknown column returns clean error */
    r = vx_db_query(db, "items nonexistent_col", NULL, 0, false);
    VX_ASSERT(r && !r->ok && r->err_msg[0] != '\0');
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== 3. HASH AGGREGATION, BY & MULTI-STMT BINDINGS ==================== */

static void test_group_by_agg_distinct_and_bindings(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);

    VX_ASSERT(exec_ok(db,
        "table orders { id: u64 key, country: str, status: str, amount: f64 }", true));
    VX_ASSERT(exec_ok(db,
        "orders += ["
        "  { id: 1, country: \"IN\", status: \"paid\",   amount: 200.0 },"
        "  { id: 2, country: \"IN\", status: \"paid\",   amount: 400.0 },"
        "  { id: 3, country: \"US\", status: \"paid\",   amount: 350.0 },"
        "  { id: 4, country: \"US\", status: \"refund\", amount: 900.0 },"
        "  { id: 5, country: \"JP\", status: \"paid\",   amount: 800.0 }"
        "]", true));

    /* Global aggregate */
    vx_result_t *r = vx_db_query(db,
        "orders[status == \"paid\"] { n: count(), rev: sum(amount), lo: min(amount), hi: max(amount), mean: avg(amount) }",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && r->ncols == 5);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 4);
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 1750.0));
    VX_ASSERT(approx_eq(vx_result_f64(r, 2, 0), 200.0));
    VX_ASSERT(approx_eq(vx_result_f64(r, 3, 0), 800.0));
    VX_ASSERT(approx_eq(vx_result_f64(r, 4, 0), 437.5));
    vx_result_free(r);

    /* Grouped aggregate + post-group filter (replaces HAVING) + sort */
    r = vx_db_query(db,
        "orders[status == \"paid\"] by country { n: count(), rev: sum(amount) } [rev >= 600.0] sort -rev",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2 && r->ncols == 3);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "JP") && vx_result_u64(r, 1, 0) == 1 && approx_eq(vx_result_f64(r, 2, 0), 800.0));
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 1), "IN") && vx_result_u64(r, 1, 1) == 2 && approx_eq(vx_result_f64(r, 2, 1), 600.0));
    vx_result_free(r);

    /* Distinct (`by` without `{}`) */
    r = vx_db_query(db, "orders by country sort country", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 3 && r->ncols == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "IN"));
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 1), "JP"));
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 2), "US"));
    vx_result_free(r);

    /* Multi-statement variable binding */
    r = vx_db_query(db,
        "paid = orders[status == \"paid\"] by country { rev: sum(amount) }\n"
        "paid[rev > 400.0] country, rev sort -rev [:1]",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "JP"));
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== 4. JOINS (+, ++, &, AUTO-FK) ==================== */

static void test_joins_left_full_inner_and_auto_fk(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);

    VX_ASSERT(exec_ok(db,
        "table users { id: u64 key, name: str, country: str }\n"
        "table orders { id: u64 key, user_id: u64 -> users.id, amount: f64 }\n"
        "table tags { id: u64 key, label: str }",
        true));

    VX_ASSERT(exec_ok(db,
        "users += ["
        "  { id: 1, name: \"Aarav\", country: \"IN\" },"
        "  { id: 2, name: \"Meera\", country: \"IN\" },"
        "  { id: 3, name: \"Idle\",  country: \"US\" }"
        "]\n"
        "orders += ["
        "  { id: 10, user_id: 1, amount: 100.0 },"
        "  { id: 11, user_id: 1, amount: 250.0 },"
        "  { id: 12, user_id: 99, amount: 50.0 }"
        "]", true));

    /* Inner join (&): only orders 10, 11 match user 1 */
    vx_result_t *r = vx_db_query(db,
        "orders & users(user_id == id) orders.id, users.name, amount sort orders.id",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 10 && str_eq_cstr(vx_result_str(r, 1, 0), "Aarav"));
    VX_ASSERT(vx_result_u64(r, 0, 1) == 11 && approx_eq(vx_result_f64(r, 2, 1), 250.0));
    vx_result_free(r);

    /* Auto-FK inner join (orders & users uses user_id -> users.id) */
    r = vx_db_query(db,
        "orders & users by users.name { total: sum(amount) }",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "Aarav"));
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 350.0));
    vx_result_free(r);

    /* Left outer join (+): keeps orphan order 12 with NULL users.name */
    r = vx_db_query(db,
        "orders + users(user_id == id) orders.id, users.name sort orders.id",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 3);
    VX_ASSERT(vx_result_u64(r, 0, 2) == 12 && vx_result_is_null(r, 1, 2));
    vx_result_free(r);

    /* Full outer join (++): keeps orders 10, 11, orphan 12, AND unmatched users 2, 3 */
    r = vx_db_query(db,
        "orders ++ users(user_id == id) orders.id, users.id",
        NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 5);
    int matched = 0, left_only = 0, right_only = 0;
    for (uint32_t i = 0; i < r->nrows; i++) {
        bool ln = vx_result_is_null(r, 0, i);
        bool rn = vx_result_is_null(r, 1, i);
        if (!ln && !rn) matched++;
        else if (!ln && rn) left_only++;
        else if (ln && !rn) right_only++;
    }
    VX_ASSERT(matched == 2 && left_only == 1 && right_only == 2);
    vx_result_free(r);

    /* Safety guard: auto-join with NO FK between tables MUST fail (no Cartesian explosion) */
    r = vx_db_query(db, "orders + tags", NULL, 0, false);
    VX_ASSERT(r && !r->ok && r->err_msg[0] != '\0');
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== 5. WAL PERSISTENCE & RECOVERY ==================== */

static void test_wal_persistence_across_reopen(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/vx_m5_db_%ld.wal", (long)getpid());
    unlink(path);

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.fsync_on_commit = true;

    vx_db_t *db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table acct { id: u64 key, owner: str, bal: f64 }", true));
    VX_ASSERT(exec_ok(db,
        "acct += [{ id: 1, owner: \"Alice\", bal: 100.0 }, { id: 2, owner: \"Bob\", bal: 200.0 }]",
        true));
    VX_ASSERT(exec_ok(db, "acct[1] = { bal: bal + 55.5 }", true));
    VX_ASSERT(exec_ok(db, "del acct[2]", true));
    vx_db_close(db);

    /* Reopen from disk WAL and verify recovered state */
    db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    vx_result_t *r = vx_db_query(db, "acct id, owner, bal", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 1, 0), "Alice"));
    VX_ASSERT(approx_eq(vx_result_f64(r, 2, 0), 155.5));
    vx_result_free(r);

    /* Append post-recovery mutation and verify second reopen */
    VX_ASSERT(exec_ok(db, "acct += { id: 3, owner: \"Cara\", bal: 300.0 }", true));
    vx_db_close(db);

    db = vx_db_open(path, &cfg);
    VX_ASSERT(db != NULL);
    r = vx_db_query(db, "acct { n: count(), total: sum(bal) }", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 2);
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 455.5));
    vx_result_free(r);

    vx_db_close(db);
    unlink(path);
}

/* ==================== 6. MVCC CONCURRENT READERS + WRITER ==================== */

typedef struct {
    vx_db_t    *db;
    atomic_int *stop;
    uint64_t    reads;
    bool        ok;
} mvcc_reader_ctx_t;

static void *mvcc_reader_fn(void *arg) {
    mvcc_reader_ctx_t *ctx = (mvcc_reader_ctx_t *)arg;
    uint64_t prev_count = 0;

    while (!atomic_load_explicit(ctx->stop, memory_order_acquire)) {
        vx_result_t *r = vx_db_query(ctx->db,
            "events[val >= 0] { n: count(), s: sum(val) }", NULL, 0, false);
        if (!r || !r->ok || r->nrows != 1) {
            vx_result_free(r);
            ctx->ok = false;
            return NULL;
        }
        uint64_t n = vx_result_u64(r, 0, 0);
        int64_t s = vx_result_i64(r, 1, 0);
        vx_result_free(r);

        /* Because ids 1..N are inserted sequentially with val = id, sum must equal N*(N+1)/2 */
        int64_t expect_sum = (int64_t)(n * (n + 1) / 2);
        if (n < prev_count || s != expect_sum) {
            ctx->ok = false;
            return NULL;
        }
        prev_count = n;
        ctx->reads++;
    }
    ctx->ok = true;
    return NULL;
}

static void test_mvcc_concurrent_readers_and_writer(void) {
    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.chunk_rows = 64;
    cfg.worker_threads = 2;
    vx_db_t *db = vx_db_open(NULL, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table events { id: u64 key, val: i64 }", true));

    atomic_int stop = 0;
    pthread_t th[4];
    mvcc_reader_ctx_t args[4];
    for (int i = 0; i < 4; i++) {
        args[i] = (mvcc_reader_ctx_t){ .db = db, .stop = &stop, .reads = 0, .ok = false };
        VX_ASSERT(pthread_create(&th[i], NULL, mvcc_reader_fn, &args[i]) == 0);
    }

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t n_inserts = 300;
#else
    const uint64_t n_inserts = 2000;
#endif

    const char *cols[2] = { "id", "val" };
    for (uint64_t i = 1; i <= n_inserts; i++) {
        vx_val_t vals[2] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_I64, .v.i64 = (int64_t)i }
        };
        VX_ASSERT(vx_db_put_row(db, "events", cols, vals, 2, false, (i % 50) == 0));
    }
    vx_db_flush(db);

    atomic_store_explicit(&stop, 1, memory_order_release);
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        VX_ASSERT(args[i].ok && args[i].reads > 0);
    }

    vx_db_close(db);
}

/* ==================== 7. MORSEL-DRIVEN MULTICORE SCAN & SLA ==================== */

static void test_morsel_multicore_scan_and_sla(void) {
    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.worker_threads = 4;
    cfg.chunk_rows = 4096;

    vx_db_t *db = vx_db_open(NULL, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table metrics { id: u64 key, bucket: i64, val: f64 }", true));

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t n_rows = 16384;
    const int iters = 3;
#else
    const uint64_t n_rows = 262144; /* 64 chunks across 4 workers */
    const int iters = 20;
#endif

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

    const char *q = "metrics[val > 500.0] by bucket { n: count(), s: sum(val) } sort bucket";
    double t0 = vx_now_sec();
    for (int it = 0; it < iters; it++) {
        vx_result_t *r = vx_db_query(db, q, NULL, 0, false);
        VX_ASSERT(r && r->ok && r->nrows == 8 && r->ncols == 3);
        if (it == 0) {
            for (uint32_t b = 0; b < 8; b++) {
                VX_ASSERT(vx_result_i64(r, 0, b) == (int64_t)b);
                VX_ASSERT(vx_result_u64(r, 1, b) == expect_cnt[b]);
                VX_ASSERT(approx_eq(vx_result_f64(r, 2, b), expect_sum[b]));
            }
        }
        vx_result_free(r);
    }
    double dt = vx_now_sec() - t0;

    double mrows = ((double)n_rows * (double)iters / dt) / 1e6;
    printf("    [METRIC] Morsel 4-core filter+hash_agg: %.1f M rows/sec (%.4fs)\n", mrows, dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* Release-build SLA tripwire: >= 50M rows/sec end-to-end parsed + parallel filter + hash agg */
    VX_ASSERT(mrows >= 50.0);
#endif

    vx_db_close(db);
}

int main(void) {
    VX_RUN(test_ddl_crud_pk_and_fast_put);
    VX_RUN(test_filters_params_sort_slice_and_explain);
    VX_RUN(test_group_by_agg_distinct_and_bindings);
    VX_RUN(test_joins_left_full_inner_and_auto_fk);
    VX_RUN(test_wal_persistence_across_reopen);
    VX_RUN(test_mvcc_concurrent_readers_and_writer);
    VX_RUN(test_morsel_multicore_scan_and_sla);
    return vx_test_summary("M5 Engine: Full DB + WAL + MVCC + Multicore Executor");
}

