#include "vx_arena.h"
#include "vx_chunk.h"
#include "vx_db.h"
#include "vx_parser.h"
#include "vx_simd.h"
#include "vx_snap.h"
#include "vx_test.h"
#include "vx_wal.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool approx_eq(double a, double b) {
    return fabs(a - b) < 1e-6;
}

static bool str_eq_cstr(vx_str_t s, const char *cstr) {
    size_t n = strlen(cstr);
    return (size_t)s.len == n && (n == 0 || memcmp(vx_str_data(&s), cstr, n) == 0);
}

static uint64_t file_size_bytes(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_size > 0 ? (uint64_t)st.st_size : 0;
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

/* ==================== 1. ATOMIC WAL CHECKPOINT & COMPACTION ==================== */

static void test_m9_wal_checkpoint_compaction_and_recovery(void) {
    char wal_path[160], snap_path[176];
    snprintf(wal_path, sizeof(wal_path), "/tmp/vx_m9_ckpt_%ld.wal", (long)getpid());
    snprintf(snap_path, sizeof(snap_path), "%s.snap", wal_path);
    unlink(wal_path);
    unlink(snap_path);

    /* In-memory DB has no WAL -> checkpoint must return false cleanly */
    vx_db_t *mem_db = vx_db_open(NULL, NULL);
    VX_ASSERT(mem_db != NULL);
    VX_ASSERT(!vx_db_checkpoint(mem_db));
    vx_db_close(mem_db);

    vx_db_cfg_t cfg = vx_db_default_cfg();
    cfg.chunk_rows = 64;
    cfg.fsync_on_commit = false;

    vx_db_t *db = vx_db_open(wal_path, &cfg);
    VX_ASSERT(db != NULL);

    VX_ASSERT(exec_ok(db,
        "table users { id: u64 key, name: str, age: i64, score: f64, active: bool }\n"
        "table orders { id: u64 key, user_id: u64 -> users.id, amt: f64 }",
        NULL, 0, true));

    /* Insert 300 users (including NULL names and >12B strings) + 100 orders */
    const char *ucols[5] = { "id", "name", "age", "score", "active" };
    char nbuf[32];
    for (uint64_t i = 1; i <= 300; i++) {
        int n = snprintf(nbuf, sizeof(nbuf), "user_long_name_%04llu", (unsigned long long)i);
        vx_val_t v[5] = {
            { .type = VX_TYPE_U64,  .v.u64 = i },
            { .type = VX_TYPE_STR,  .is_null = (i % 10 == 0), .v.str = { nbuf, (uint32_t)n } },
            { .type = VX_TYPE_I64,  .v.i64 = (int64_t)(20 + (i % 40)) },
            { .type = VX_TYPE_F64,  .v.f64 = (double)i * 1.5 },
            { .type = VX_TYPE_BOOL, .v.b   = (i & 1U) != 0 }
        };
        VX_ASSERT(vx_db_put_row(db, "users", ucols, v, 5, false, false));
    }

    const char *ocols[3] = { "id", "user_id", "amt" };
    for (uint64_t i = 1; i <= 100; i++) {
        vx_val_t v[3] = {
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_U64, .v.u64 = i },
            { .type = VX_TYPE_F64, .v.f64 = 25.0 }
        };
        VX_ASSERT(vx_db_put_row(db, "orders", ocols, v, 3, false, false));
    }
    vx_db_flush(db);

    /* Mutate & delete to create tombstones and grow WAL */
    for (uint64_t k = 1; k <= 50; k++) {
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = k } };
        VX_ASSERT(exec_ok(db, "users[$k] = { score: score + 100.0 }", &p, 1, true));
    }
    for (uint64_t k = 201; k <= 300; k++) {
        vx_param_t p = { "k", { .type = VX_TYPE_U64, .v.u64 = k } };
        VX_ASSERT(exec_ok(db, "del users[$k]", &p, 1, true));
    }

    uint64_t wal_before = file_size_bytes(wal_path);
    VX_ASSERT(wal_before > 4096);

    /* Execute atomic checkpoint + WAL compaction */
    VX_ASSERT(vx_db_checkpoint(db));

    uint64_t wal_after = file_size_bytes(wal_path);
    VX_ASSERT(wal_after == 16); /* WAL compacted down to 16-byte header */

    vx_snap_info_t sinfo = {0};
    VX_ASSERT(vx_snap_inspect(snap_path, &sinfo));
    VX_ASSERT(sinfo.valid_crc);
    VX_ASSERT(sinfo.ntables == 2);
    VX_ASSERT(sinfo.total_rows == 300); /* 200 live users + 100 orders (tombstones stripped) */
    VX_ASSERT(sinfo.checkpoint_lsn > 0);
    VX_ASSERT(vx_db_stats(db).checkpoints == 1);

    /* Post-checkpoint writes (both parameterized query + put_row upsert) */
    vx_param_t pu[2] = {
        { "k",     { .type = VX_TYPE_U64, .v.u64 = 1 } },
        { "delta", { .type = VX_TYPE_F64, .v.f64 = 50.0 } }
    };
    VX_ASSERT(exec_ok(db, "users[$k] = { score: score + $delta }", pu, 2, true));

    vx_val_t u301[5] = {
        { .type = VX_TYPE_U64,  .v.u64 = 301 },
        { .type = VX_TYPE_STR,  .v.str = { "post_ckpt_user", 14 } },
        { .type = VX_TYPE_I64,  .v.i64 = 33 },
        { .type = VX_TYPE_F64,  .v.f64 = 500.0 },
        { .type = VX_TYPE_BOOL, .v.b   = true }
    };
    VX_ASSERT(vx_db_put_row(db, "users", ucols, u301, 5, true, true));

    /* Post-checkpoint WAL must have strictly monotonic LSN > checkpoint_lsn */
    VX_ASSERT(vx_db_stats(db).wal_lsn > sinfo.checkpoint_lsn);
    vx_db_close(db);

    /* Reopen: loads .snap + replays post-checkpoint WAL tail */
    db = vx_db_open(wal_path, &cfg);
    VX_ASSERT(db != NULL);

    /* Verify live counts, PK index rebuild, NULLs, FK join, and post-checkpoint edits */
    vx_result_t *r = vx_db_query(db, "users { n: count() }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 201);
    vx_result_free(r);

    r = vx_db_query(db, "users[1] name, score, active", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "user_long_name_0001"));
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 151.5)); /* 1.5 + 100.0 + 50.0 */
    VX_ASSERT(vx_result_bool(r, 2, 0) == true);
    vx_result_free(r);

    r = vx_db_query(db, "users[10] name", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1 && vx_result_is_null(r, 0, 0));
    vx_result_free(r);

    r = vx_db_query(db, "users[250] name", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 0); /* deleted pre-checkpoint */
    vx_result_free(r);

    r = vx_db_query(db, "users[301] name, score", NULL, 0, false);
    VX_ASSERT(r && r->ok && r->nrows == 1);
    VX_ASSERT(str_eq_cstr(vx_result_str(r, 0, 0), "post_ckpt_user"));
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 500.0));
    vx_result_free(r);

    /* Auto-FK join metadata must survive snapshot reload */
    r = vx_db_query(db, "orders & users { n: count(), s: sum(amt) }", NULL, 0, false);
    VX_ASSERT(r && r->ok && vx_result_u64(r, 0, 0) == 100);
    VX_ASSERT(approx_eq(vx_result_f64(r, 1, 0), 2500.0));
    vx_result_free(r);

    vx_db_close(db);

    /* Corrupt snapshot bit-flip MUST be detected (no silent empty-DB fallback) */
    int sfd = open(snap_path, O_RDWR);
    VX_ASSERT(sfd >= 0);
    off_t ssz = lseek(sfd, 0, SEEK_END);
    uint8_t b = 0;
    VX_ASSERT(pread(sfd, &b, 1, ssz - 1) == 1);
    b ^= 0xFF;
    VX_ASSERT(pwrite(sfd, &b, 1, ssz - 1) == 1);
    close(sfd);

    VX_ASSERT(!vx_snap_inspect(snap_path, &sinfo));
    VX_ASSERT(vx_db_open(wal_path, &cfg) == NULL);

    unlink(wal_path);
    unlink(snap_path);
}

/* ==================== 2. EXCLUSIVE FLOCK FILE LOCKING ==================== */

static void test_m9_exclusive_flock_intra_and_cross_process(void) {
    char wal_path[160];
    snprintf(wal_path, sizeof(wal_path), "/tmp/vx_m9_lock_%ld.wal", (long)getpid());
    unlink(wal_path);

    /* A. Cross-process fork() test on single-threaded vx_wal_open (TSan-safe) */
    vx_wal_t *w1 = vx_wal_open(wal_path, NULL);
    VX_ASSERT(w1 != NULL);
    VX_ASSERT(vx_wal_open(wal_path, NULL) == NULL); /* intra-process collision */

    pid_t pid = fork();
    VX_ASSERT(pid >= 0);
    if (pid == 0) {
        vx_wal_t *w_child = vx_wal_open(wal_path, NULL);
        _exit(w_child == NULL ? 0 : 1);
    }
    int status = 0;
    VX_ASSERT(waitpid(pid, &status, 0) == pid);
    VX_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    vx_wal_close(w1);

    /* B. Full vx_db_open exclusion + re-acquire after close */
    vx_db_t *db1 = vx_db_open(wal_path, NULL);
    VX_ASSERT(db1 != NULL);
    VX_ASSERT(vx_db_open(wal_path, NULL) == NULL);
    vx_db_close(db1);

    vx_db_t *db2 = vx_db_open(wal_path, NULL);
    VX_ASSERT(db2 != NULL);
    vx_db_close(db2);
    unlink(wal_path);
}

/* ==================== 3. RUNTIME SIMD DISPATCH PARITY ==================== */

static void test_m9_runtime_simd_dispatch_parity(void) {
    vx_simd_level_t detected = vx_simd_detect();
    VX_ASSERT(vx_simd_active() == detected);
    const char *name = vx_simd_name(detected);
    VX_ASSERT(name != NULL && strlen(name) > 0);
    printf("    [INFO] Runtime SIMD backend detected: %s\n", name);

    /* Forcing an unsupported ISA must fail cleanly and leave active level intact */
#if defined(__x86_64__) || defined(_M_X64)
    VX_ASSERT(!vx_simd_force_level(VX_SIMD_NEON));
#elif defined(__aarch64__)
    VX_ASSERT(!vx_simd_force_level(VX_SIMD_AVX2));
#endif
    VX_ASSERT(vx_simd_active() == detected);

    /* Build 4,099 f64 values (non-multiple of 4, includes NaN, -0.0, edges) */
    const uint32_t n = 4099;
    double   *col = (double *)malloc(n * sizeof(double));
    uint64_t *nb  = (uint64_t *)calloc((n + 63U) / 64U, sizeof(uint64_t));
    uint32_t *sel_scalar = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *sel_active = (uint32_t *)malloc(n * sizeof(uint32_t));
    VX_ASSERT(col && nb && sel_scalar && sel_active);

    for (uint32_t i = 0; i < n; i++) {
        if (i % 97 == 0) col[i] = NAN;
        else if (i % 53 == 0) col[i] = -0.0;
        else col[i] = (double)((int32_t)((i * 37U) % 1000U) - 500);
        if (i % 17 == 0) nb[i >> 6] |= (1ULL << (i & 63U));
    }

    const vx_cmp_op_t ops[6] = {
        VX_CMP_EQ, VX_CMP_NE, VX_CMP_LT, VX_CMP_LE, VX_CMP_GT, VX_CMP_GE
    };
    const double thresholds[3] = { 0.0, 125.0, NAN };

    for (int t = 0; t < 3; t++) {
        for (int oi = 0; oi < 6; oi++) {
            for (int masked = 0; masked < 2; masked++) {
                const uint64_t *null_ptr = masked ? nb : NULL;

                VX_ASSERT(vx_simd_force_level(VX_SIMD_SCALAR));
                uint32_t n_s = vx_filter_f64(col, null_ptr, n, ops[oi], thresholds[t], sel_scalar);

                VX_ASSERT(vx_simd_force_level(detected));
                uint32_t n_a = vx_filter_f64(col, null_ptr, n, ops[oi], thresholds[t], sel_active);

                VX_ASSERT(n_s == n_a);
                if (n_s > 0) {
                    VX_ASSERT(memcmp(sel_scalar, sel_active, n_s * sizeof(uint32_t)) == 0);
                }
            }
        }
    }

    free(col);
    free(nb);
    free(sel_scalar);
    free(sel_active);
}

/* ==================== 4. DETERMINISTIC FUZZ SMOKE (PARSER & WAL) ==================== */

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static bool fuzz_wal_cb(const vx_wal_rec_t *rec, void *ctx) {
    (void)rec; (void)ctx;
    return true;
}

static void test_m9_deterministic_fuzz_smoke(void) {
    /* 1. 1,000 hostile/random parser inputs must never crash or leak */
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    uint64_t rng = 0x565846555A5A39ULL;
    char buf[192];

    for (int i = 0; i < 1000; i++) {
        size_t len = (size_t)(xorshift64(&rng) % sizeof(buf));
        for (size_t j = 0; j < len; j++) {
            buf[j] = (char)(xorshift64(&rng) & 0xFFU);
        }
        vx_arena_reset(&arena);
        vx_parse_result_t pr;
        (void)vx_parse(buf, len, &arena, &pr);
    }
    vx_arena_destroy(&arena);

    /* 2. 64 hostile/corrupted WAL files must never crash or OOM */
    char fpath[160];
    snprintf(fpath, sizeof(fpath), "/tmp/vx_m9_fuzz_%ld.wal", (long)getpid());
    for (int i = 0; i < 64; i++) {
        size_t len = (size_t)(xorshift64(&rng) % sizeof(buf));
        for (size_t j = 0; j < len; j++) {
            buf[j] = (char)(xorshift64(&rng) & 0xFFU);
        }
        int fd = open(fpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        VX_ASSERT(fd >= 0);
        if (len > 0) {
            VX_ASSERT(write(fd, buf, len) == (ssize_t)len);
        }
        close(fd);

        vx_wal_recovery_t info = {0};
        (void)vx_wal_replay(fpath, fuzz_wal_cb, NULL, &info);
    }
    unlink(fpath);
}

int main(void) {
    VX_RUN(test_m9_wal_checkpoint_compaction_and_recovery);
    VX_RUN(test_m9_exclusive_flock_intra_and_cross_process);
    VX_RUN(test_m9_runtime_simd_dispatch_parity);
    VX_RUN(test_m9_deterministic_fuzz_smoke);
    return vx_test_summary("M9 Release: Checkpoint + Flock + Runtime SIMD + Fuzz Smoke");
}
