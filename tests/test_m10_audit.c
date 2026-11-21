#include "vx_db.h"
#include "vx_test.h"
#include "vx_wmesh.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool exec_fails(vx_db_t *db, const char *q,
                       const vx_param_t *params, size_t nparams, bool sync) {
    vx_result_t *r = vx_db_query(db, q, params, nparams, sync);
    bool failed = (r != NULL && !r->ok && r->err_msg[0] != '\0');
    vx_result_free(r);
    return failed;
}

/* ==================== BUG 1: BOUNDED SLICE UPDATE & DELETE ==================== */

static void test_m10_bounded_slice_update_and_delete(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table seq { id: u64 key, v: i64 }", NULL, 0, true));
    VX_ASSERT(exec_ok(db,
        "seq += ["
        "  { id: 1, v: 10 }, { id: 2, v: 20 }, { id: 3, v: 30 },"
        "  { id: 4, v: 40 }, { id: 5, v: 50 }"
        "]", NULL, 0, true));

    /* Bounded slice update: rows [1:3] -> ids 2 and 3 ONLY */
    vx_result_t *u = vx_db_query(db, "seq[1:3] = { v: v + 100 }", NULL, 0, true);
    VX_ASSERT(u && u->ok && u->rows_affected == 2);
    vx_result_free(u);

    vx_result_t *r = vx_db_query(db, "seq id, v", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 5);
    VX_ASSERT(vx_result_i64(r, 1, 0) == 10);
    VX_ASSERT(vx_result_i64(r, 1, 1) == 120);
    VX_ASSERT(vx_result_i64(r, 1, 2) == 130);
    VX_ASSERT(vx_result_i64(r, 1, 3) == 40);
    VX_ASSERT(vx_result_i64(r, 1, 4) == 50);
    vx_result_free(r);

    /* Bounded slice delete: del seq[1:3] deletes ONLY ids 2 and 3 */
    vx_result_t *d = vx_db_query(db, "del seq[1:3]", NULL, 0, true);
    VX_ASSERT(d && d->ok && d->rows_affected == 2);
    vx_result_free(d);

    r = vx_db_query(db, "seq id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 3);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 1);
    VX_ASSERT(vx_result_u64(r, 0, 1) == 4);
    VX_ASSERT(vx_result_u64(r, 0, 2) == 5);
    vx_result_free(r);

    /* Bounded prefix slice delete: del seq[:1] deletes ONLY id 1 */
    d = vx_db_query(db, "del seq[:1]", NULL, 0, true);
    VX_ASSERT(d && d->ok && d->rows_affected == 1);
    vx_result_free(d);

    r = vx_db_query(db, "seq id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 4 && vx_result_u64(r, 0, 1) == 5);
    vx_result_free(r);

    /* Full slice [:] still updates/truncates all rows */
    u = vx_db_query(db, "seq[:] = { v: 7 }", NULL, 0, true);
    VX_ASSERT(u && u->ok && u->rows_affected == 2);
    vx_result_free(u);

    d = vx_db_query(db, "del seq[:]", NULL, 0, true);
    VX_ASSERT(d && d->ok && d->rows_affected == 2);
    vx_result_free(d);

    r = vx_db_query(db, "seq", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 0);
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== BUG 2: == null AND != null FILTERING ==================== */

static void test_m10_null_equality_filtering(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table u { id: u64 key, name: str, age: i64 }", NULL, 0, true));
    VX_ASSERT(exec_ok(db,
        "u += ["
        "  { id: 1, name: \"Aarav\", age: 29 },"
        "  { id: 2, name: null,      age: 31 },"
        "  { id: 3, name: \"Meera\", age: null },"
        "  { id: 4, name: null,      age: null }"
        "]", NULL, 0, true));

    vx_result_t *r = vx_db_query(db, "u[name == null] id sort id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 2 && vx_result_u64(r, 0, 1) == 4);
    vx_result_free(r);

    /* Reversed operand order: null == name */
    r = vx_db_query(db, "u[null == name] id sort id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 2 && vx_result_u64(r, 0, 1) == 4);
    vx_result_free(r);

    /* != null */
    r = vx_db_query(db, "u[name != null] id sort id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 1 && vx_result_u64(r, 0, 1) == 3);
    vx_result_free(r);

    /* Combined null + non-null predicates; ordering/value ops still exclude NULLs */
    r = vx_db_query(db, "u[name == null, age != null] id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && vx_result_u64(r, 0, 0) == 2);
    vx_result_free(r);

    r = vx_db_query(db, "u[age > 20] id sort id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(vx_result_u64(r, 0, 0) == 1 && vx_result_u64(r, 0, 1) == 2);
    vx_result_free(r);

    /* Parameter bound to NULL works identically */
    vx_param_t pn = { "p", { .type = VX_TYPE_STR, .is_null = true } };
    r = vx_db_query(db, "u[name == $p] id sort id", &pn, 1, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== BUG 3: sum(u64) PRECISION & UNION CORRUPTION ==================== */

static void test_m10_sum_u64_exact_arithmetic(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table io { id: u64 key, grp: str, bytes: u64 }", NULL, 0, true));

    /* Values > 2^53 test BOTH union-type corruption AND exact 64-bit integer precision */
    const uint64_t big_a = (1ULL << 54) + 3ULL;
    const uint64_t big_b = 2ULL;
    const char *cols[3] = { "id", "grp", "bytes" };

    vx_val_t r1[3] = {
        { .type = VX_TYPE_U64, .v.u64 = 1 },
        { .type = VX_TYPE_STR, .v.str = { "a", 1 } },
        { .type = VX_TYPE_U64, .v.u64 = big_a }
    };
    vx_val_t r2[3] = {
        { .type = VX_TYPE_U64, .v.u64 = 2 },
        { .type = VX_TYPE_STR, .v.str = { "a", 1 } },
        { .type = VX_TYPE_U64, .v.u64 = big_b }
    };
    vx_val_t r3[3] = {
        { .type = VX_TYPE_U64, .v.u64 = 3 },
        { .type = VX_TYPE_STR, .v.str = { "b", 1 } },
        { .type = VX_TYPE_U64, .v.u64 = 30ULL }
    };
    VX_ASSERT(vx_db_put_row(db, "io", cols, r1, 3, false, true));
    VX_ASSERT(vx_db_put_row(db, "io", cols, r2, 3, false, true));
    VX_ASSERT(vx_db_put_row(db, "io", cols, r3, 3, false, true));

    /* Global sum(u64) */
    vx_result_t *res = vx_db_query(db, "io { total: sum(bytes) }", NULL, 0, false);
    VX_ASSERT(res && res->ok && res->nrows == 1);
    VX_ASSERT(res->cols[0].type == VX_TYPE_U64);
    VX_ASSERT(vx_result_u64(res, 0, 0) == big_a + big_b + 30ULL);
    vx_result_free(res);

    /* Grouped sum(u64) */
    res = vx_db_query(db, "io by grp { total: sum(bytes) } sort grp", NULL, 0, false);
    VX_ASSERT(res && res->ok && res->nrows == 2);
    VX_ASSERT(res->cols[1].type == VX_TYPE_U64);
    VX_ASSERT(vx_result_u64(res, 1, 0) == big_a + big_b);
    VX_ASSERT(vx_result_u64(res, 1, 1) == 30ULL);
    vx_result_free(res);

    vx_db_close(db);
}

/* ==================== BUG 4: DUPLICATE PK REJECTION ==================== */

static void test_m10_duplicate_pk_rejection(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table acct { id: u64 key, name: str }", NULL, 0, true));
    VX_ASSERT(exec_ok(db, "acct += { id: 1, name: \"Alice\" }", NULL, 0, true));

    /* Single duplicate insert via += MUST fail */
    VX_ASSERT(exec_fails(db, "acct += { id: 1, name: \"Impostor\" }", NULL, 0, true));

    /* Batch insert with internal duplicate MUST fail atomically (zero partial rows) */
    VX_ASSERT(exec_fails(db,
        "acct += [{ id: 2, name: \"Bob\" }, { id: 2, name: \"Bob2\" }]",
        NULL, 0, true));

    /* Fast-path put_row with upsert=false MUST not create a duplicate row */
    const char *cols[2] = { "id", "name" };
    vx_val_t dup[2] = {
        { .type = VX_TYPE_U64, .v.u64 = 1 },
        { .type = VX_TYPE_STR, .v.str = { "DupPut", 6 } }
    };
    (void)vx_db_put_row(db, "acct", cols, dup, 2, false, true);
    vx_db_flush(db);

    vx_result_t *r = vx_db_query(db, "acct id, name", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 1, 0), "Alice"));
    vx_result_free(r);

    /* Upsert (|= and put_row upsert=true) still replaces cleanly */
    VX_ASSERT(exec_ok(db, "acct |= { id: 1, name: \"AliceV2\" }", NULL, 0, true));
    r = vx_db_query(db, "acct[1] name", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && str_eq_cstr(vx_result_str(r, 0, 0), "AliceV2"));
    vx_result_free(r);

    /* Deleting PK=1 allows inserting PK=1 again */
    VX_ASSERT(exec_ok(db, "del acct[1]", NULL, 0, true));
    VX_ASSERT(exec_ok(db, "acct += { id: 1, name: \"Reborn\" }", NULL, 0, true));
    r = vx_db_query(db, "acct { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 1);
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== BUG 5: db.put() EAGER SCHEMA VALIDATION ==================== */

static void test_m10_put_row_schema_validation(void) {
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table users { id: u64 key, age: i64, name: str }", NULL, 0, true));

    const char *good_cols[3] = { "id", "age", "name" };
    vx_val_t good_vals[3] = {
        { .type = VX_TYPE_U64, .v.u64 = 1 },
        { .type = VX_TYPE_I64, .v.i64 = 25 },
        { .type = VX_TYPE_STR, .v.str = { "Ok", 2 } }
    };

    /* 1. Unknown table MUST fail immediately even with sync=false */
    VX_ASSERT(!vx_db_put_row(db, "typo_table", good_cols, good_vals, 3, false, false));

    /* 2. Unknown column MUST fail immediately even with sync=false */
    const char *bad_cols[2] = { "id", "nonexistent_col" };
    vx_val_t bad_vals[2] = {
        { .type = VX_TYPE_U64, .v.u64 = 2 },
        { .type = VX_TYPE_I64, .v.i64 = 10 }
    };
    VX_ASSERT(!vx_db_put_row(db, "users", bad_cols, bad_vals, 2, false, false));

    /* 3. Incompatible type (string into i64 column) MUST fail immediately */
    const char *type_cols[2] = { "id", "age" };
    vx_val_t wrong_type[2] = {
        { .type = VX_TYPE_U64, .v.u64 = 3 },
        { .type = VX_TYPE_STR, .v.str = { "not_an_int", 10 } }
    };
    VX_ASSERT(!vx_db_put_row(db, "users", type_cols, wrong_type, 2, false, false));

    /* Valid row succeeds */
    VX_ASSERT(vx_db_put_row(db, "users", good_cols, good_vals, 3, false, true));
    vx_result_t *r = vx_db_query(db, "users { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 1);
    vx_result_free(r);

    vx_db_close(db);
}

/* ==================== BUG 6: STRING ESCAPE SEQUENCES ==================== */

static void test_m10_string_unescaping(void) {
    char wal[160], snap[176];
    snprintf(wal, sizeof(wal), "/tmp/vx_m10_esc_%ld.wal", (long)getpid());
    snprintf(snap, sizeof(snap), "%s.snap", wal);
    unlink(wal);
    unlink(snap);

    vx_db_t *db = vx_db_open(wal, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table notes { id: u64 key, body: str }", NULL, 0, true));

    VX_ASSERT(exec_ok(db,
        "notes += { id: 1, body: \"line1\\nline2\\t\\\"quoted\\\"\\\\end\" }",
        NULL, 0, true));
    VX_ASSERT(exec_ok(db,
        "notes += { id: 2, body: 'it\\'s\\r\\nok' }",
        NULL, 0, true));

    vx_result_t *r = vx_db_query(db, "notes id, body sort id", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 2);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 1, 0), "line1\nline2\t\"quoted\"\\end"));
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 1, 1), "it's\r\nok"));
    vx_result_free(r);

    /* Filtering with escaped literal matches unescaped stored bytes */
    r = vx_db_query(db, "notes[body == \"line1\\nline2\\t\\\"quoted\\\"\\\\end\"] id",
                    NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && vx_result_u64(r, 0, 0) == 1);
    vx_result_free(r);

    /* Survives checkpoint + reopen */
    VX_ASSERT(vx_db_checkpoint(db));
    vx_db_close(db);

    db = vx_db_open(wal, NULL);
    VX_ASSERT(db != NULL);
    r = vx_db_query(db, "notes[1] body", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 &&
              str_eq_cstr(vx_result_str(r, 0, 0), "line1\nline2\t\"quoted\"\\end"));
    vx_result_free(r);

    vx_db_close(db);
    unlink(wal);
    unlink(snap);
}

/* ==================== BUG 7: UNIQUE & INDEX ENFORCEMENT ==================== */

static void test_m10_unique_and_index_enforcement(void) {
    char wal[160], snap[176];
    snprintf(wal, sizeof(wal), "/tmp/vx_m10_uniq_%ld.wal", (long)getpid());
    snprintf(snap, sizeof(snap), "%s.snap", wal);
    unlink(wal);
    unlink(snap);

    vx_db_t *db = vx_db_open(wal, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table users { id: u64 key, email: str, country: str }", NULL, 0, true));

    /* Creating index on unknown table or unknown column MUST fail */
    VX_ASSERT(exec_fails(db, "index ghost(email)", NULL, 0, true));
    VX_ASSERT(exec_fails(db, "unique users(no_such_col)", NULL, 0, true));

    /* Declare unique(email) and secondary index(country) */
    VX_ASSERT(exec_ok(db, "unique users(email)", NULL, 0, true));
    VX_ASSERT(exec_ok(db, "index users(country)", NULL, 0, true));

    VX_ASSERT(exec_ok(db,
        "users += ["
        "  { id: 1, email: \"a@x.io\", country: \"IN\" },"
        "  { id: 2, email: null,       country: \"IN\" },"
        "  { id: 3, email: null,       country: \"US\" }"
        "]", NULL, 0, true)); /* multiple NULLs allowed */

    /* Duplicate non-NULL email MUST be rejected on insert & update */
    VX_ASSERT(exec_fails(db, "users += { id: 4, email: \"a@x.io\", country: \"UK\" }", NULL, 0, true));
    VX_ASSERT(exec_fails(db, "users[2] = { email: \"a@x.io\" }", NULL, 0, true));

    /* Updating row 1's email frees "a@x.io" for row 2 */
    VX_ASSERT(exec_ok(db, "users[1] = { email: \"a2@x.io\" }", NULL, 0, true));
    VX_ASSERT(exec_ok(db, "users[2] = { email: \"a@x.io\" }", NULL, 0, true));

    /* Checkpoint + reopen MUST preserve unique constraint */
    VX_ASSERT(vx_db_checkpoint(db));
    vx_db_close(db);

    db = vx_db_open(wal, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_fails(db, "users += { id: 5, email: \"a@x.io\", country: \"JP\" }", NULL, 0, true));
    VX_ASSERT(exec_ok(db, "users += { id: 5, email: \"new@x.io\", country: \"JP\" }", NULL, 0, true));

    vx_db_close(db);
    unlink(wal);
    unlink(snap);
}

/* ==================== GAP 8: LIVE RAM COMPACTION ON CHECKPOINT ==================== */

static void test_m10_live_ram_compaction_on_checkpoint(void) {
    char wal[160], snap[176];
    snprintf(wal, sizeof(wal), "/tmp/vx_m10_compact_%ld.wal", (long)getpid());
    snprintf(snap, sizeof(snap), "%s.snap", wal);
    unlink(wal);
    unlink(snap);

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.chunk_rows = 64;
    vx_db_t *db = vx_db_open(wal, &cfg);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table kv { id: u64 key, v: i64 }", NULL, 0, true));

    const char *cols[2] = { "id", "v" };
    for (uint64_t i = 1; i <= 400; i++) {
        vx_val_t v[2] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_I64, .v.i64 = (int64_t)i }
        };
        VX_ASSERT(vx_db_put_row(db, "kv", cols, v, 2, false, false));
    }
    vx_db_flush(db);

    /* Generate 250 tombstones via O(1) PK updates & deletes */
    for (uint64_t k = 1; k <= 100; k++) {
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = k } };
        VX_ASSERT(exec_ok(db, "kv[$k] = { v: v + 1000 }", &p, 1, true));
    }
    for (uint64_t k = 251; k <= 400; k++) {
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = k } };
        VX_ASSERT(exec_ok(db, "del kv[$k]", &p, 1, true));
    }

    vx_db_stats_t before = vx_db_stats(db);
    VX_ASSERT(before.dead_rows == 250);

    /* Checkpoint MUST compact live RAM chunks too, dropping dead_rows to 0 */
    VX_ASSERT(vx_db_checkpoint(db));
    vx_db_stats_t after = vx_db_stats(db);
    VX_ASSERT(after.dead_rows == 0);

    /* Verify live rows & PK index remained intact after in-memory compaction */
    vx_result_t *r = vx_db_query(db, "kv { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 250);
    vx_result_free(r);

    r = vx_db_query(db, "kv[1] v", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && vx_result_i64(r, 0, 0) == 1001);
    vx_result_free(r);

    r = vx_db_query(db, "kv[300] v", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 0);
    vx_result_free(r);

    vx_db_close(db);
    unlink(wal);
    unlink(snap);
}

/* ==================== GAP 9: WMESH THREAD RECYCLING & LARGE BATCH ARENA ==================== */

static void noop_wmesh_commit(const vx_write_op_t *ops, size_t count,
                              uint64_t batch_lsn, void *user_ctx) {
    (void)ops; (void)batch_lsn;
    atomic_uint_fast64_t *tot = (atomic_uint_fast64_t *)user_ctx;
    atomic_fetch_add_explicit(tot, count, memory_order_relaxed);
}

typedef struct {
    vx_db_t *db;
    uint64_t id;
    bool     ok;
} eph_writer_arg_t;

static void *eph_writer_fn(void *arg) {
    eph_writer_arg_t *a = (eph_writer_arg_t *)arg;
    const char *cols[2] = { "id", "v" };
    vx_val_t v[2] = {
        { .type = VX_TYPE_U64, .v.u64 = a->id },
        { .type = VX_TYPE_I64, .v.i64 = (int64_t)a->id }
    };
    a->ok = vx_db_put_row(a->db, "w", cols, v, 2, false, true);
    return NULL;
}

static void test_m10_wmesh_recycling_and_large_batch_query(void) {
    /* 1. Direct wmesh unit test: max_producers=4 recycled 32 times */
    atomic_uint_fast64_t committed = 0;
    vx_wmesh_cfg_t wcfg = {
        .max_producers = 4,
        .ring_cap_pow2 = 16,
        .batch_max_ops = 8,
        .flush_interval_us = 1000,
        .commit_fn = noop_wmesh_commit,
        .user_ctx = &committed
    };
    vx_wmesh_t *mesh = vx_wmesh_create(&wcfg);
    VX_ASSERT(mesh != NULL);

    for (uint64_t i = 1; i <= 32; i++) {
        vx_wmesh_tx_t *tx = vx_wmesh_register(mesh);
        VX_ASSERT(tx != NULL);
        vx_write_op_t op = { .key = i, .table_id = 1, .op_type = VX_OP_INSERT };
        uint64_t tok = 0;
        VX_ASSERT(vx_wmesh_submit(tx, &op, &tok));
        VX_ASSERT(vx_wmesh_wait(tx, tok));
        vx_wmesh_unregister(tx);
    }
    VX_ASSERT(atomic_load(&committed) == 32);
    vx_wmesh_destroy(mesh);

    /* 2. DB integration: 80 ephemeral writer threads (> 64 ring limit) */
    vx_db_t *db = vx_db_open(NULL, NULL);
    VX_ASSERT(db != NULL);
    VX_ASSERT(exec_ok(db, "table w { id: u64 key, v: i64 }", NULL, 0, true));

    for (uint64_t wave = 0; wave < 20; wave++) {
        pthread_t th[4];
        eph_writer_arg_t args[4];
        for (uint64_t j = 0; j < 4; j++) {
            args[j] = (eph_writer_arg_t){ .db = db, .id = wave * 4 + j + 1, .ok = false };
            VX_ASSERT(pthread_create(&th[j], NULL, eph_writer_fn, &args[j]) == 0);
        }
        for (int j = 0; j < 4; j++) {
            pthread_join(th[j], NULL);
            VX_ASSERT(args[j].ok);
        }
    }

    vx_result_t *r = vx_db_query(db, "w { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 80);
    vx_result_free(r);

    /* 3. Large 600-row batch insert exceeds old 64KB parser arena */
    size_t qcap = 64 * 1024;
    char *big_q = (char *)malloc(qcap);
    VX_ASSERT(big_q != NULL);
    size_t pos = (size_t)snprintf(big_q, qcap, "w += [");
    for (int i = 1000; i < 1600; i++) {
        pos += (size_t)snprintf(big_q + pos, qcap - pos, "%s{ id: %d, v: %d }",
                                i > 1000 ? "," : "", i, i * 2);
    }
    snprintf(big_q + pos, qcap - pos, "]");
    VX_ASSERT(exec_ok(db, big_q, NULL, 0, true));
    free(big_q);

    r = vx_db_query(db, "w { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 680);
    vx_result_free(r);

    vx_db_close(db);
}

int main(void) {
    VX_RUN(test_m10_bounded_slice_update_and_delete);
    VX_RUN(test_m10_null_equality_filtering);
    VX_RUN(test_m10_sum_u64_exact_arithmetic);
    VX_RUN(test_m10_duplicate_pk_rejection);
    VX_RUN(test_m10_put_row_schema_validation);
    VX_RUN(test_m10_string_unescaping);
    VX_RUN(test_m10_unique_and_index_enforcement);
    VX_RUN(test_m10_live_ram_compaction_on_checkpoint);
    VX_RUN(test_m10_wmesh_recycling_and_large_batch_query);
    return vx_test_summary("M10 Pre-Release Audit: 7 Bugs + RAM Compaction + WMesh Recycling");
}
