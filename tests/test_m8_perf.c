#include "vx_db.h"
#include "vx_delmap.h"
#include "vx_rhmap.h"
#include "vx_test.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static bool approx_eq(double a, double b) {
    return fabs(a - b) < 1e-5;
}

static bool str_eq_cstr(vx_str_t s, const char *cstr) {
    size_t n = strlen(cstr);
    return (size_t)s.len == n && (n == 0 || memcmp(vx_str_data(&s), cstr, n) == 0);
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

/* ==================== 1. ROBIN HOOD HASH MAP + SLA ==================== */

static void test_rhmap_crud_edge_keys_churn_and_sla(void) {
    vx_rhmap_t m;
    VX_ASSERT(vx_rhmap_init(&m, 16));

    /* Edge keys: 0 and UINT64_MAX must work identically to ordinary keys */
    uint64_t v = 0;
    VX_ASSERT(!vx_rhmap_get(&m, 0, &v));
    VX_ASSERT(vx_rhmap_put(&m, 0, 111));
    VX_ASSERT(vx_rhmap_put(&m, UINT64_MAX, 222));
    VX_ASSERT(vx_rhmap_get(&m, 0, &v) && v == 111);
    VX_ASSERT(vx_rhmap_get(&m, UINT64_MAX, &v) && v == 222);
    VX_ASSERT(vx_rhmap_put(&m, 0, 333));
    VX_ASSERT(vx_rhmap_get(&m, 0, &v) && v == 333);
    VX_ASSERT(vx_rhmap_len(&m) == 2);

    /* Backward-shift delete check */
    VX_ASSERT(vx_rhmap_del(&m, 0, &v) && v == 333);
    VX_ASSERT(!vx_rhmap_get(&m, 0, &v));
    VX_ASSERT(vx_rhmap_get(&m, UINT64_MAX, &v) && v == 222);
    VX_ASSERT(!vx_rhmap_del(&m, 0, &v));
    VX_ASSERT(vx_rhmap_del(&m, UINT64_MAX, &v) && v == 222);
    VX_ASSERT(vx_rhmap_len(&m) == 0);

    /* Heavy churn: insert 20k, delete evens, reinsert new keys -> no tombstone decay */
    const uint64_t churn_n = 20000;
    for (uint64_t i = 1; i <= churn_n; i++) {
        VX_ASSERT(vx_rhmap_put(&m, i, vx_pk_pack((uint32_t)(i >> 12), (uint32_t)(i & 4095U))));
    }
    for (uint64_t i = 2; i <= churn_n; i += 2) {
        VX_ASSERT(vx_rhmap_del(&m, i, NULL));
    }
    VX_ASSERT(vx_rhmap_len(&m) == churn_n / 2);

    /* COW clone independence */
    vx_rhmap_t *clone = vx_rhmap_clone(&m);
    VX_ASSERT(clone != NULL && vx_rhmap_len(clone) == churn_n / 2);
    VX_ASSERT(vx_rhmap_put(clone, 999999, 42));
    VX_ASSERT(!vx_rhmap_get(&m, 999999, &v));
    VX_ASSERT(vx_rhmap_get(clone, 999999, &v) && v == 42);
    vx_rhmap_destroy(clone);
    free(clone);

    /* Remaining odd keys must still resolve with tight Robin Hood PSL */
    for (uint64_t i = 1; i <= churn_n; i += 2) {
        VX_ASSERT(vx_rhmap_get(&m, i, &v));
        VX_ASSERT(vx_pk_chunk(v) == (uint32_t)(i >> 12));
        VX_ASSERT(vx_pk_row(v) == (uint32_t)(i & 4095U));
    }
    VX_ASSERT(vx_rhmap_max_psl(&m) < 24U);

    /* Lookup throughput SLA */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t bench_keys = 10000;
    const int iters = 5;
#else
    const uint64_t bench_keys = 100000;
    const int iters = 20;
#endif

    vx_rhmap_t bm;
    VX_ASSERT(vx_rhmap_init(&bm, 16));
    for (uint64_t i = 1; i <= bench_keys; i++) {
        VX_ASSERT(vx_rhmap_put(&bm, i * 2654435761ULL, i));
    }

    uint64_t checksum = 0;
    double t0 = vx_now_sec();
    for (int it = 0; it < iters; it++) {
        for (uint64_t i = 1; i <= bench_keys; i++) {
            if (vx_rhmap_get(&bm, i * 2654435761ULL, &v)) {
                checksum += v;
            }
        }
    }
    double dt = vx_now_sec() - t0;
    VX_ASSERT(checksum == (uint64_t)iters * (bench_keys * (bench_keys + 1) / 2));

    double mops = ((double)bench_keys * (double)iters / dt) / 1e6;
    printf("    [METRIC] Robin Hood lookup: %.2f M ops/sec (max_psl=%u, %.4fs)\n",
           mops, vx_rhmap_max_psl(&bm), dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    VX_ASSERT(mops >= 10.0);
#endif

    vx_rhmap_destroy(&bm);
    vx_rhmap_destroy(&m);
}

/* ==================== 2. PER-CHUNK DELMAP (TOMBSTONES) ==================== */

static void test_delmap_cow_and_sel_compaction(void) {
    vx_delmap_t *dm = vx_delmap_create(256);
    VX_ASSERT(dm != NULL && vx_delmap_count(dm) == 0);
    VX_ASSERT(((uintptr_t)dm->bits % 64) == 0);

    VX_ASSERT(vx_delmap_set(dm, 0));
    VX_ASSERT(!vx_delmap_set(dm, 0)); /* idempotent */
    VX_ASSERT(vx_delmap_set(dm, 63));
    VX_ASSERT(vx_delmap_set(dm, 64));
    VX_ASSERT(vx_delmap_set(dm, 255));
    VX_ASSERT(!vx_delmap_set(dm, 256)); /* out of bounds */
    VX_ASSERT(vx_delmap_count(dm) == 4);

    /* COW clone independence */
    vx_delmap_t *dm2 = vx_delmap_clone(dm);
    VX_ASSERT(dm2 != NULL && vx_delmap_count(dm2) == 4);
    VX_ASSERT(vx_delmap_set(dm2, 100));
    VX_ASSERT(!vx_delmap_is_set(dm, 100));
    VX_ASSERT(vx_delmap_is_set(dm2, 100));
    VX_ASSERT(vx_delmap_count(dm) == 4 && vx_delmap_count(dm2) == 5);

    /* Selection vector compaction */
    uint32_t sel[8] = { 0, 1, 63, 64, 99, 100, 200, 255 };
    uint32_t n = vx_delmap_filter_sel(dm2, sel, 8);
    VX_ASSERT(n == 3 && sel[0] == 1 && sel[1] == 99 && sel[2] == 200);

    vx_delmap_dec(dm2);
    vx_delmap_dec(dm);
}

/* ==================== 3. O(1) PK LOOKUP, TOMBSTONE UPDATE/DELETE/UPSERT ==================== */

static void test_o1_pk_lookup_update_delete_upsert_sla(void) {
    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.chunk_rows = 1024;
    cfg.worker_threads = 4;

    vx_db_t *db = vx_db_open(NULL, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table kv { id: u64 key, bucket: i64, val: f64 }", true));

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t n_rows = 2000;
    const uint64_t n_ops  = 200;
#else
    const uint64_t n_rows = 50000;
    const uint64_t n_ops  = 1000;
#endif

    const char *cols[3] = { "id", "bucket", "val" };
    double expected_sum = 0.0;
    for (uint64_t i = 1; i <= n_rows; i++) {
        vx_val_t vals[3] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_I64, .v.i64 = (int64_t)(i & 7ULL) },
            { .type = VX_TYPE_F64, .v.f64 = (double)i }
        };
        VX_ASSERT(vx_db_put_row(db, "kv", cols, vals, 3, false, false));
        expected_sum += (double)i;
    }
    vx_db_flush(db);

    /* 1. O(1) PK lookups */
    double t0 = vx_now_sec();
    for (uint64_t k = 1; k <= n_ops; k++) {
        uint64_t target = (k * 37ULL) % n_rows + 1;
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = target } };
        vx_result_t *r = vx_db_query(db, "kv[$k] val", &p, 1, false);
        VX_ASSERT(r && r->ok && r->nrows == 1);
        VX_ASSERT(approx_eq(vx_result_f64(r, 0, 0), (double)target));
        vx_result_free(r);
    }
    double dt_pk = vx_now_sec() - t0;

    /* 2. O(1) Tombstone PK updates (keys 1..n_ops/2: val += 100.0) */
    const uint64_t n_upd = n_ops / 2;
    double t1 = vx_now_sec();
    for (uint64_t k = 1; k <= n_upd; k++) {
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = k } };
        vx_result_t *r = vx_db_query(db, "kv[$k] = { val: val + 100.0 }", &p, 1, true);
        VX_ASSERT(r && r->ok && r->rows_affected == 1);
        vx_result_free(r);
        expected_sum += 100.0;
    }

    /* 3. O(1) Tombstone PK deletes (keys n_upd+1 .. n_ops) */
    for (uint64_t k = n_upd + 1; k <= n_ops; k++) {
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = k } };
        vx_result_t *r = vx_db_query(db, "del kv[$k]", &p, 1, true);
        VX_ASSERT(r && r->ok && r->rows_affected == 1);
        vx_result_free(r);
        expected_sum -= (double)k;
    }

    /* 4. O(1) Upsert via db.put(upsert=true) on updated key 1 and deleted key n_upd+1 */
    vx_val_t u1[3] = {
        { .type = VX_TYPE_U64, .v.u64 = 1 },
        { .type = VX_TYPE_I64, .v.i64 = 1 },
        { .type = VX_TYPE_F64, .v.f64 = 500.0 }
    };
    VX_ASSERT(vx_db_put_row(db, "kv", cols, u1, 3, true, true));
    expected_sum += (500.0 - 101.0);

    vx_val_t u_revive[3] = {
        { .type = VX_TYPE_U64, .v.u64 = n_upd + 1 },
        { .type = VX_TYPE_I64, .v.i64 = (int64_t)((n_upd + 1) & 7ULL) },
        { .type = VX_TYPE_F64, .v.f64 = 777.0 }
    };
    VX_ASSERT(vx_db_put_row(db, "kv", cols, u_revive, 3, true, true));
    expected_sum += 777.0;
    double dt_mut = vx_now_sec() - t1;

    uint64_t expected_live = n_rows - (n_ops - n_upd) + 1;

    /* Verify BOTH general aggregate AND morsel-driven fast path skip tombstones */
    vx_result_t *r_gen = vx_db_query(db, "kv { n: count(), s: sum(val) }", NULL, 0, false);
    VX_ASSERT(r_gen && r_gen->ok && r_gen->nrows == 1);
    VX_ASSERT(vx_result_u64(r_gen, 0, 0) == expected_live);
    VX_ASSERT(approx_eq(vx_result_f64(r_gen, 1, 0), expected_sum));
    vx_result_free(r_gen);

    vx_result_t *r_morsel = vx_db_query(db, "kv[val > 0.0] { n: count(), s: sum(val) }",
                                        NULL, 0, false);
    VX_ASSERT(r_morsel && r_morsel->ok && r_morsel->nrows == 1);
    VX_ASSERT(vx_result_u64(r_morsel, 0, 0) == expected_live);
    VX_ASSERT(approx_eq(vx_result_f64(r_morsel, 1, 0), expected_sum));
    vx_result_free(r_morsel);

    /* Deleted key must return 0 rows on PK lookup */
    vx_param_t pd = { "k", { .type = VX_TYPE_U64, .v.u64 = n_ops } };
    vx_result_t *r_del = vx_db_query(db, "kv[$k] val", &pd, 1, false);
    VX_ASSERT(r_del && r_del->ok && r_del->nrows == 0);
    vx_result_free(r_del);

    printf("    [METRIC] O(1) PK on %llu rows: %llu lookups in %.4fs, %llu upd+del in %.4fs\n",
           (unsigned long long)n_rows, (unsigned long long)n_ops, dt_pk,
           (unsigned long long)n_ops, dt_mut);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* Impossible under O(N) full-table rebuilds (which take >5s); O(1) must be < 0.25s */
    VX_ASSERT(dt_pk < 0.15);
    VX_ASSERT(dt_mut < 0.25);
#endif

    vx_db_close(db);
}

/* ==================== 4. HASH JOIN (+, ++, &) CORRECTNESS & SLA ==================== */

static void test_hash_join_dups_nulls_and_sla(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);

    /* 1. Edge-case correctness: duplicate keys (M:N) + NULL keys (NULL != NULL) */
    VX_ASSERT(exec_ok(db,
        "table lhs { id: u64 key, k: i64 }\n"
        "table rhs { id: u64 key, k: i64 }", true));
    VX_ASSERT(exec_ok(db,
        "lhs += [{ id: 1, k: 10 }, { id: 2, k: 10 }, { id: 3, k: 20 }, { id: 4, k: null }]\n"
        "rhs += [{ id: 100, k: 10 }, { id: 101, k: 10 }, { id: 102, k: 30 }, { id: 103, k: null }]",
        true));

    /* Inner (&): 2x2 matches on k=10 -> 4 rows; NULL must NOT match NULL */
    vx_result_t *r = vx_db_query(db, "lhs & rhs(k == k)", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 4);
    vx_result_free(r);

    /* Left (+): 4 matched + k=20 + lhs NULL -> 6 rows */
    r = vx_db_query(db, "lhs + rhs(k == k)", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 6);
    vx_result_free(r);

    /* Full (++): 4 matched + 2 left-only + 2 right-only (k=30, rhs NULL) -> 8 rows */
    r = vx_db_query(db, "lhs ++ rhs(k == k)", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 8);
    vx_result_free(r);

    /* 2. Scale & SLA test */
    VX_ASSERT(exec_ok(db,
        "table users { id: u64 key, tier: i64 }\n"
        "table orders { id: u64 key, user_id: u64 -> users.id, amt: f64 }", true));

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t n_users = 1000;
    const uint64_t n_orders = 3000;
#else
    const uint64_t n_users = 15000;
    const uint64_t n_orders = 30000;
#endif

    const char *ucols[2] = { "id", "tier" };
    for (uint64_t i = 1; i <= n_users; i++) {
        vx_val_t v[2] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_I64, .v.i64 = (int64_t)(i % 5) }
        };
        VX_ASSERT(vx_db_put_row(db, "users", ucols, v, 2, false, false));
    }

    const char *ocols[3] = { "id", "user_id", "amt" };
    double expected_matched_sum = 0.0;
    uint64_t expected_matched_cnt = 0;
    for (uint64_t i = 1; i <= n_orders; i++) {
        /* 10% orphan orders (user_id > n_users) to exercise outer joins */
        uint64_t uid = (i % 10 == 0) ? (n_users + i) : ((i % n_users) + 1);
        vx_val_t v[3] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_U64, .v.u64 = uid },
            { .type = VX_TYPE_F64, .v.f64 = 10.0 }
        };
        VX_ASSERT(vx_db_put_row(db, "orders", ocols, v, 3, false, false));
        if (uid <= n_users) {
            expected_matched_cnt++;
            expected_matched_sum += 10.0;
        }
    }
    vx_db_flush(db);

    double t0 = vx_now_sec();
    vx_result_t *ri = vx_db_query(db, "orders & users { n: count(), s: sum(amt) }", NULL, 0, false);
    double dt_inner = vx_now_sec() - t0;
    VX_ASSERT(ri && ri->ok && ri->nrows == 1);
    VX_ASSERT(vx_result_u64(ri, 0, 0) == expected_matched_cnt);
    VX_ASSERT(approx_eq(vx_result_f64(ri, 1, 0), expected_matched_sum));
    vx_result_free(ri);

    double t1 = vx_now_sec();
    vx_result_t *rl = vx_db_query(db, "orders + users(user_id == id) { n: count() }", NULL, 0, false);
    double dt_left = vx_now_sec() - t1;
    VX_ASSERT(rl && rl->ok && rl->nrows == 1);
    VX_ASSERT(vx_result_u64(rl, 0, 0) == n_orders);
    vx_result_free(rl);

    printf("    [METRIC] Hash Join (%llu x %llu): inner=%.4fs, left=%.4fs\n",
           (unsigned long long)n_orders, (unsigned long long)n_users, dt_inner, dt_left);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* O(N*M) nested loop does 450M compares (>3s); Hash Join must be < 0.25s */
    VX_ASSERT(dt_inner < 0.25);
    VX_ASSERT(dt_left < 0.25);
#endif

    vx_db_close(db);
}

/* ==================== 5. HASH GROUP-BY HIGH-CARDINALITY & SLA ==================== */

static void test_hash_group_by_high_cardinality_and_sla(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db,
        "table sales { id: u64 key, tenant: str, region: str, amt: f64 }", true));

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t n_groups = 400;
    const uint64_t per_grp = 4;
#else
    const uint64_t n_groups = 5000;
    const uint64_t per_grp = 10; /* 50,000 rows -> 5,000 distinct long-string groups */
#endif

    const char *cols[4] = { "id", "tenant", "region", "amt" };
    char tbuf[32];
    uint64_t id = 0;
    for (uint64_t rep = 0; rep < per_grp; rep++) {
        for (uint64_t g = 0; g < n_groups; g++) {
            int n = snprintf(tbuf, sizeof(tbuf), "tenant_org_%05llu", (unsigned long long)g);
            vx_val_t v[4] = {
                { .type = VX_TYPE_U64, .v.u64 = ++id },
                { .type = VX_TYPE_STR, .v.str = { tbuf, (uint32_t)n } },
                { .type = VX_TYPE_STR, .v.str = { (g & 1) ? "IN" : "US", 2 } },
                { .type = VX_TYPE_F64, .v.f64 = (double)(rep + 1) }
            };
            VX_ASSERT(vx_db_put_row(db, "sales", cols, v, 4, false, false));
        }
    }
    vx_db_flush(db);

    double t0 = vx_now_sec();
    vx_result_t *r = vx_db_query(db,
        "sales by tenant, region { n: count(), s: sum(amt), lo: min(amt), hi: max(amt) } "
        "sort tenant",
        NULL, 0, false);
    double dt = vx_now_sec() - t0;

    VX_ASSERT(r && r->ok && r->nrows == n_groups && r->ncols == 6);
    double expect_sum = (double)(per_grp * (per_grp + 1) / 2);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "tenant_org_00000"));
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 1, 0), "US"));
    VX_ASSERT(vx_result_u64(r, 2, 0) == per_grp);
    VX_ASSERT(approx_eq(vx_result_f64(r, 3, 0), expect_sum));
    VX_ASSERT(approx_eq(vx_result_f64(r, 4, 0), 1.0));
    VX_ASSERT(approx_eq(vx_result_f64(r, 5, 0), (double)per_grp));
    vx_result_free(r);

    printf("    [METRIC] Hash Group-By (%llu rows, %llu string groups): %.4fs\n",
           (unsigned long long)(n_groups * per_grp), (unsigned long long)n_groups, dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* O(N*G) linear scan does ~125M compares (>2s); Hash Group-By must be < 0.20s */
    VX_ASSERT(dt < 0.20);
#endif

    vx_db_close(db);
}

/* ==================== 6. LOCK-FREE CONCURRENT SORT ==================== */

typedef struct {
    vx_db_t *db;
    int      iters;
    uint32_t expected_rows;
    bool     ok;
} sort_mt_arg_t;

static void *concurrent_sort_worker(void *arg) {
    sort_mt_arg_t *a = (sort_mt_arg_t *)arg;
    for (int it = 0; it < a->iters; it++) {
        vx_result_t *r = vx_db_query(a->db, "scores sort -val, id", NULL, 0, false);
        if (!r || !r->ok || r->nrows != a->expected_rows) {
            vx_result_free(r);
            a->ok = false;
            return NULL;
        }
        for (uint32_t i = 1; i < r->nrows; i++) {
            double prev_v = vx_result_f64(r, 1, i - 1);
            double cur_v  = vx_result_f64(r, 1, i);
            uint64_t prev_id = vx_result_u64(r, 0, i - 1);
            uint64_t cur_id  = vx_result_u64(r, 0, i);
            if (cur_v > prev_v || (cur_v == prev_v && cur_id < prev_id)) {
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

static void test_lockfree_concurrent_sort(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table scores { id: u64 key, val: f64 }", true));

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint32_t n_rows = 1024;
    const int iters = 6;
#else
    const uint32_t n_rows = 8192;
    const int iters = 12;
#endif

    const char *cols[2] = { "id", "val" };
    for (uint32_t i = 1; i <= n_rows; i++) {
        vx_val_t v[2] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_F64, .v.f64 = (double)((i * 7919U) % 500U) }
        };
        VX_ASSERT(vx_db_put_row(db, "scores", cols, v, 2, false, false));
    }
    vx_db_flush(db);

    pthread_t th[8];
    sort_mt_arg_t args[8];
    double t0 = vx_now_sec();
    for (int i = 0; i < 8; i++) {
        args[i] = (sort_mt_arg_t){ .db = db, .iters = iters, .expected_rows = n_rows, .ok = false };
        VX_ASSERT(pthread_create(&th[i], NULL, concurrent_sort_worker, &args[i]) == 0);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        VX_ASSERT(args[i].ok);
    }
    double dt = vx_now_sec() - t0;
    printf("    [METRIC] Lock-free 8-thread concurrent sort (%u rows x %d): %.4fs\n",
           n_rows, 8 * iters, dt);

    vx_db_close(db);
}

int main(void) {
    VX_RUN(test_rhmap_crud_edge_keys_churn_and_sla);
    VX_RUN(test_delmap_cow_and_sel_compaction);
    VX_RUN(test_o1_pk_lookup_update_delete_upsert_sla);
    VX_RUN(test_hash_join_dups_nulls_and_sla);
    VX_RUN(test_hash_group_by_high_cardinality_and_sla);
    VX_RUN(test_lockfree_concurrent_sort);
    return vx_test_summary("M8 Speed: RobinHood PK + Tombstones + HashJoin/Group + LockFreeSort");
}
