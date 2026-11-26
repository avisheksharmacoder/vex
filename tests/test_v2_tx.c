#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vx_types.h"
#include "vx_owner.h"
#include "vx_manifest.h"
#include "vx_tx.h"
#include "vx_failpoint.h"

/* ============================================================================
 * Mock Catalog & DB Fixtures
 * ============================================================================ */
typedef struct mock_table {
    uint32_t table_id;
    uint32_t row_count;
    int64_t  balance_sum;
} mock_table_t;

typedef struct mock_cat {
    vx_owner_t   hdr;
    uint32_t     version;
    uint32_t     n_tables;
    mock_table_t tables[4];
} mock_cat_t;

static void mock_cat_destroy(vx_owner_t *o) {
    mock_cat_t *c = (mock_cat_t *)o;
    free(c);
}

static mock_cat_t *mock_cat_create(uint32_t version) {
    mock_cat_t *c = (mock_cat_t *)calloc(1, sizeof(*c));
    assert(c);
    vx_owner_init(&c->hdr, VX_OWN_CATALOG, mock_cat_destroy);
    c->version = version;
    c->n_tables = 2;
    for (uint32_t i = 0; i < 2; ++i) {
        c->tables[i].table_id = i;
        c->tables[i].row_count = 0;
        c->tables[i].balance_sum = 0;
    }
    return c;
}

static mock_cat_t *mock_cat_clone(const mock_cat_t *src, uint32_t new_version) {
    mock_cat_t *dst = (mock_cat_t *)calloc(1, sizeof(*dst));
    assert(dst);
    vx_owner_init(&dst->hdr, VX_OWN_CATALOG, mock_cat_destroy);
    dst->version = new_version;
    dst->n_tables = src->n_tables;
    memcpy(dst->tables, src->tables, sizeof(dst->tables));
    return dst;
}

typedef struct mock_db {
    pthread_mutex_t  write_mu;
    pthread_mutex_t  cat_mu; /* guards mock EBR retain/retire */
    vx_owner_t      *published_cat;
    uint64_t         commit_seq;
    uint64_t         lsn;

    /* Hot RAM accounting mock */
    size_t           hot_limit;
    _Atomic size_t   hot_bytes;

    /* Conflict tracking */
    uint64_t         conflict_commit_seq;
    uint64_t         conflict_write_mask;

    /* Operation call counters */
    _Atomic uint32_t wmesh_drains;
    _Atomic uint32_t ebr_publishes;
} mock_db_t;

/* Operations for tx */
static void mock_drain_wmesh(void *db_ctx) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    atomic_fetch_add(&db->wmesh_drains, 1);
}

static vx_owner_t *mock_acquire_snapshot(void *db_ctx, uint64_t *out_commit_seq) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    pthread_mutex_lock(&db->cat_mu);
    if (out_commit_seq) *out_commit_seq = db->commit_seq;
    vx_owner_t *cat = vx_owner_retain(db->published_cat);
    pthread_mutex_unlock(&db->cat_mu);
    return cat;
}

typedef struct mock_mutation_payload {
    uint32_t table_id;
    int64_t  delta_bal;
} mock_mutation_payload_t;

static vx_status_t mock_apply_cow_op(void *db_ctx, vx_owner_t *in_cat,
                                     uint32_t op_type, const uint8_t *payload, uint32_t payload_len,
                                     vx_owner_t **out_next_cat, uint64_t *out_table_bit,
                                     size_t *out_hot_bytes_added) {
    (void)db_ctx;
    (void)op_type;
    mock_cat_t *src = (mock_cat_t *)in_cat;
    if (payload_len < sizeof(mock_mutation_payload_t)) return VX_ERR_INVALID;

    mock_mutation_payload_t mut;
    memcpy(&mut, payload, sizeof(mut));
    if (mut.table_id >= src->n_tables) return VX_ERR_SCHEMA;

    mock_cat_t *next = mock_cat_clone(src, src->version + 1);
    next->tables[mut.table_id].row_count++;
    next->tables[mut.table_id].balance_sum += mut.delta_bal;

    *out_next_cat = &next->hdr;
    if (out_table_bit) *out_table_bit = (1ULL << mut.table_id);
    if (out_hot_bytes_added) *out_hot_bytes_added = 64u;
    return VX_OK;
}

static vx_status_t mock_reserve_hot(void *db_ctx, size_t add_bytes) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    if (db->hot_limit > 0) {
        size_t cur = atomic_load(&db->hot_bytes);
        while (1) {
            if (cur + add_bytes > db->hot_limit) {
                return VX_ERR_MEMLIM;
            }
            if (atomic_compare_exchange_weak(&db->hot_bytes, &cur, cur + add_bytes)) {
                break;
            }
        }
    } else {
        atomic_fetch_add(&db->hot_bytes, add_bytes);
    }
    return VX_OK;
}

static void mock_release_hot(void *db_ctx, size_t sub_bytes) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    size_t cur = atomic_load(&db->hot_bytes);
    while (1) {
        size_t nxt = (cur >= sub_bytes) ? (cur - sub_bytes) : 0;
        if (atomic_compare_exchange_weak(&db->hot_bytes, &cur, nxt)) {
            break;
        }
    }
}

static mock_cat_t *mock_get_published(mock_db_t *db) {
    pthread_mutex_lock(&db->cat_mu);
    mock_cat_t *cat = (mock_cat_t *)db->published_cat;
    pthread_mutex_unlock(&db->cat_mu);
    return cat;
}

static vx_owner_t *mock_current_published(void *db_ctx, uint64_t *out_commit_seq) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    pthread_mutex_lock(&db->cat_mu);
    if (out_commit_seq) *out_commit_seq = db->commit_seq;
    vx_owner_t *cat = db->published_cat;
    pthread_mutex_unlock(&db->cat_mu);
    return cat;
}

static bool mock_has_write_conflict(void *db_ctx, uint64_t base_commit_seq,
                                    uint64_t read_mask, uint64_t write_mask) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    if (db->commit_seq > base_commit_seq) {
        /* If committed changes overlap read or write tables */
        if ((read_mask | write_mask) & db->conflict_write_mask) {
            return true;
        }
    }
    return false;
}

static uint64_t mock_next_lsn(void *db_ctx) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    return ++db->lsn;
}

static void mock_publish_ebr(void *db_ctx, vx_owner_t *new_cat,
                             uint64_t new_lsn, uint64_t write_mask) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    (void)write_mask;
    db->lsn = new_lsn;
    pthread_mutex_lock(&db->cat_mu);
    db->commit_seq++;
    atomic_fetch_add(&db->ebr_publishes, 1);
    vx_owner_t *old = db->published_cat;
    db->published_cat = new_cat;
    pthread_mutex_unlock(&db->cat_mu);
    vx_owner_release(old);
}

static vx_tx_db_ops_t g_mock_ops = {
    .drain_wmesh        = mock_drain_wmesh,
    .acquire_snapshot   = mock_acquire_snapshot,
    .apply_cow_op       = mock_apply_cow_op,
    .reserve_hot        = mock_reserve_hot,
    .release_hot        = mock_release_hot,
    .current_published  = mock_current_published,
    .has_write_conflict = mock_has_write_conflict,
    .next_lsn           = mock_next_lsn,
    .publish_ebr        = mock_publish_ebr
};

static void setup_mock_db(mock_db_t *db, size_t hot_limit) {
    memset(db, 0, sizeof(*db));
    pthread_mutex_init(&db->write_mu, NULL);
    pthread_mutex_init(&db->cat_mu, NULL);
    mock_cat_t *cat = mock_cat_create(1);
    db->published_cat = &cat->hdr;
    db->commit_seq = 1;
    db->lsn        = 10;
    db->hot_limit  = hot_limit;
}

static void teardown_mock_db(mock_db_t *db) {
    pthread_mutex_lock(&db->cat_mu);
    vx_owner_t *cat = db->published_cat;
    db->published_cat = NULL;
    pthread_mutex_unlock(&db->cat_mu);
    if (cat) vx_owner_release(cat);
    pthread_mutex_destroy(&db->cat_mu);
    pthread_mutex_destroy(&db->write_mu);
}

/* ============================================================================
 * 1. Test Framed WAL Read/Write & CRC32 Validation
 * ============================================================================ */
static void test_wal_framed_io(void) {
    printf("[1/7] Testing framed WAL append, read, CRC32 verification & torn frame handling...\n");

    const char *wal_path = "/tmp/test_v2_wal_framed.wal";
    unlink(wal_path);

    int fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);

    /* 1. Append valid frames */
    const char *q1 = "table users (id: u64, name: str)";
    vx_status_t st = vx_wal_append_frame(fd, 100, VX_WAL_OP_QUERY, (const uint8_t *)q1, strlen(q1), false);
    assert(st == VX_OK);

    const char *q2 = "insert user 1";
    st = vx_wal_append_frame(fd, 101, VX_WAL_OP_PUT_ROW, (const uint8_t *)q2, strlen(q2), true);
    assert(st == VX_OK);

    /* TX_BATCH payload */
    uint8_t batch_payload[64];
    memset(batch_payload, 0, sizeof(batch_payload));
    vx_tx_wal_hdr_t wh = { .tx_id = 42, .n_ops = 1, ._reserved = 0 };
    memcpy(batch_payload, &wh, sizeof(wh));
    vx_tx_subop_hdr_t sh = { .op_type = VX_WAL_OP_PUT_ROW, .data_len = 16 };
    memcpy(batch_payload + sizeof(wh), &sh, sizeof(sh));
    st = vx_wal_append_frame(fd, 102, VX_WAL_OP_TX_BATCH, batch_payload, sizeof(batch_payload), false);
    assert(st == VX_OK);

    /* 2. Read frames with vx_wal_read_next_frame */
    vx_wal_reader_t r;
    memset(&r, 0, sizeof(r));
    uint64_t off = 0;
    vx_wal_frame_t frame;
    bool eof = false;

    /* Frame 1 */
    st = vx_wal_read_next_frame(&r, fd, &off, &frame, &eof);
    assert(st == VX_OK && !eof);
    assert(frame.lsn == 100);
    assert(frame.op_type == VX_WAL_OP_QUERY);
    assert(frame.payload_len == strlen(q1));
    assert(memcmp(frame.payload, q1, strlen(q1)) == 0);

    /* Frame 2 */
    st = vx_wal_read_next_frame(&r, fd, &off, &frame, &eof);
    assert(st == VX_OK && !eof);
    assert(frame.lsn == 101);
    assert(frame.op_type == VX_WAL_OP_PUT_ROW);
    assert(frame.payload_len == strlen(q2));
    assert(memcmp(frame.payload, q2, strlen(q2)) == 0);

    /* Frame 3 */
    st = vx_wal_read_next_frame(&r, fd, &off, &frame, &eof);
    assert(st == VX_OK && !eof);
    assert(frame.lsn == 102);
    assert(frame.op_type == VX_WAL_OP_TX_BATCH);
    assert(frame.payload_len == sizeof(batch_payload));

    /* Clean EOF */
    st = vx_wal_read_next_frame(&r, fd, &off, &frame, &eof);
    assert(st == VX_OK && eof);

    /* 3. Torn header test */
    off_t good_end = lseek(fd, 0, SEEK_END);
    uint8_t garbage[16] = { 'V', 'X', 'W', '2', 1, 0, 0, 0 };
    assert(write(fd, garbage, 16) == 16); /* partial header */

    uint64_t bad_off = (uint64_t)good_end;
    st = vx_wal_read_next_frame(&r, fd, &bad_off, &frame, &eof);
    assert(st == VX_ERR_CORRUPT);

    /* Truncate back to good */
    assert(ftruncate(fd, good_end) == 0);

    /* 4. Corrupted CRC test */
    st = vx_wal_append_frame(fd, 103, VX_WAL_OP_QUERY, (const uint8_t *)"hello", 5, false);
    assert(st == VX_OK);

    /* Flip one byte in the payload */
    off_t cur_end = lseek(fd, 0, SEEK_END);
    assert(pwrite(fd, "X", 1, cur_end - 2) == 1);

    bad_off = (uint64_t)good_end;
    st = vx_wal_read_next_frame(&r, fd, &bad_off, &frame, &eof);
    assert(st == VX_ERR_CORRUPT);

    vx_wal_reader_destroy(&r);
    close(fd);
    unlink(wal_path);
}

/* ============================================================================
 * 2. Test Transaction Lifecycle (Begin, RYOW, Commit, Rollback, Destroy)
 * ============================================================================ */
static void test_tx_lifecycle(void) {
    printf("[2/7] Testing transaction begin, RYOW, rollback & commit...\n");

    mock_db_t db;
    setup_mock_db(&db, 0);

    const char *wal_path = "/tmp/test_v2_wal_lifecycle.wal";
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    /* 1. Read-only transaction */
    vx_tx_t *tx_ro = NULL;
    vx_status_t st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, false, 1, &tx_ro);
    assert(st == VX_OK && tx_ro);

    vx_owner_t *ro_snap = vx_tx_get_read_catalog(tx_ro, (1ULL << 0));
    assert(ro_snap);
    assert(((mock_cat_t *)ro_snap)->version == 1);
    vx_owner_release(ro_snap);

    uint64_t commit_lsn = 0;
    st = vx_tx_commit(tx_ro, &commit_lsn);
    assert(st == VX_OK);
    assert(commit_lsn == 0); /* zero WAL I/O for read-only tx */
    assert(db.ebr_publishes == 0);
    vx_tx_destroy(tx_ro);

    /* 2. Mutating transaction with Rollback */
    vx_tx_t *tx_rb = NULL;
    st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, false, 2, &tx_rb);
    assert(st == VX_OK && tx_rb);

    mock_mutation_payload_t m1 = { .table_id = 0, .delta_bal = 100 };
    st = vx_tx_stage_op(tx_rb, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m1, sizeof(m1));
    assert(st == VX_OK);

    /* Check RYOW: staged_cat sees mutation */
    vx_owner_t *ryow = vx_tx_read_snapshot(tx_rb);
    assert(ryow);
    mock_cat_t *rcat = (mock_cat_t *)ryow;
    assert(rcat->tables[0].row_count == 1);
    assert(rcat->tables[0].balance_sum == 100);
    vx_owner_release(ryow);

    /* Published catalog outside tx remains untouched */
    mock_cat_t *pub = mock_get_published(&db);
    assert(pub->tables[0].row_count == 0);

    /* Rollback */
    vx_tx_rollback(tx_rb);
    assert(tx_rb->state == VX_TX_ABORTED);
    /* Re-rollback is idempotent */
    vx_tx_rollback(tx_rb);
    vx_tx_destroy(tx_rb);

    /* 3. Mutating transaction with Commit */
    vx_tx_t *tx_c = NULL;
    st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, false, 3, &tx_c);
    assert(st == VX_OK && tx_c);

    mock_mutation_payload_t m2 = { .table_id = 0, .delta_bal = 250 };
    mock_mutation_payload_t m3 = { .table_id = 1, .delta_bal = 500 };
    st = vx_tx_stage_op(tx_c, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m2, sizeof(m2));
    assert(st == VX_OK);
    st = vx_tx_stage_op(tx_c, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m3, sizeof(m3));
    assert(st == VX_OK);

    st = vx_tx_commit(tx_c, &commit_lsn);
    assert(st == VX_OK);
    assert(commit_lsn == 11);
    assert(db.ebr_publishes == 1);

    /* Published catalog now holds both mutations */
    pub = mock_get_published(&db);
    assert(pub->tables[0].row_count == 1 && pub->tables[0].balance_sum == 250);
    assert(pub->tables[1].row_count == 1 && pub->tables[1].balance_sum == 500);

    vx_tx_destroy(tx_c);
    close(wal_fd);
    unlink(wal_path);
    teardown_mock_db(&db);
}

/* ============================================================================
 * 3. Test Commit Rebase on Clean Background Flush
 * ============================================================================ */
static void test_tx_rebase_on_flush(void) {
    printf("[3/7] Testing commit rebase when background flush swaps published catalog...\n");

    mock_db_t db;
    setup_mock_db(&db, 0);

    const char *wal_path = "/tmp/test_v2_wal_rebase.wal";
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    /* TX begins on base catalog version 1 */
    vx_tx_t *tx = NULL;
    vx_status_t st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, false, 10, &tx);
    assert(st == VX_OK && tx);

    mock_mutation_payload_t mut = { .table_id = 0, .delta_bal = 75 };
    st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&mut, sizeof(mut));
    assert(st == VX_OK);

    /* Background flusher swaps published catalog WITHOUT incrementing commit_seq */
    pthread_mutex_lock(&db.cat_mu);
    mock_cat_t *old_pub = (mock_cat_t *)db.published_cat;
    mock_cat_t *flushed = mock_cat_clone(old_pub, 999); /* simulated new flushed catalog root */
    db.published_cat = &flushed->hdr;
    pthread_mutex_unlock(&db.cat_mu);
    vx_owner_release(&old_pub->hdr);

    /* Commit: detects pub_cat != base_cat, but cur_seq == base_commit_seq -> rebases cleanly */
    uint64_t commit_lsn = 0;
    st = vx_tx_commit(tx, &commit_lsn);
    assert(st == VX_OK);
    assert(commit_lsn == 11);

    /* Published catalog is now the rebased catalog based on flushed version */
    mock_cat_t *cur_pub = mock_get_published(&db);
    assert(cur_pub->version > 999);
    assert(cur_pub->tables[0].row_count == 1);
    assert(cur_pub->tables[0].balance_sum == 75);

    vx_tx_destroy(tx);
    close(wal_fd);
    unlink(wal_path);
    teardown_mock_db(&db);
}

/* ============================================================================
 * 4. Test Write-Conflict Detection (Optimistic Concurrency Control)
 * ============================================================================ */
static void test_tx_conflict_detection(void) {
    printf("[4/7] Testing optimistic concurrency write conflict detection...\n");

    mock_db_t db;
    setup_mock_db(&db, 0);

    const char *wal_path = "/tmp/test_v2_wal_conflict.wal";
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    /* TX1 begins */
    vx_tx_t *tx1 = NULL;
    vx_status_t st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, false, 100, &tx1);
    assert(st == VX_OK);

    /* TX1 reads table 0 */
    vx_tx_record_read(tx1, (1ULL << 0));

    /* TX1 stages write to table 0 */
    mock_mutation_payload_t mut1 = { .table_id = 0, .delta_bal = 50 };
    st = vx_tx_stage_op(tx1, VX_WAL_OP_PUT_ROW, (const uint8_t *)&mut1, sizeof(mut1));
    assert(st == VX_OK);

    /* Concurrent commit advances db.commit_seq and marks conflict on table 0 */
    db.commit_seq++;
    db.conflict_commit_seq = db.commit_seq;
    db.conflict_write_mask = (1ULL << 0);

    /* TX1 commit fails with VX_ERR_CONFLICT */
    uint64_t commit_lsn = 0;
    st = vx_tx_commit(tx1, &commit_lsn);
    assert(st == VX_ERR_CONFLICT);
    assert(tx1->state == VX_TX_ABORTED);

    vx_tx_destroy(tx1);
    close(wal_fd);
    unlink(wal_path);
    teardown_mock_db(&db);
}

/* ============================================================================
 * 5. Test Memory Backpressure & Deadlock-Free Hot Reservation
 * ============================================================================ */
static void test_tx_backpressure(void) {
    printf("[5/7] Testing memory limit backpressure and reservation release...\n");

    mock_db_t db;
    setup_mock_db(&db, 100); /* Limit = 100 B hot memory */

    const char *wal_path = "/tmp/test_v2_wal_memlim.wal";
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    /* TX begins and stages 2 mutations (each adds 64B -> total 128B > limit 100B) */
    vx_tx_t *tx = NULL;
    vx_status_t st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, false, 200, &tx);
    assert(st == VX_OK);

    mock_mutation_payload_t m = { .table_id = 0, .delta_bal = 10 };
    st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m, sizeof(m));
    assert(st == VX_OK);
    st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m, sizeof(m));
    assert(st == VX_OK);
    assert(tx->hot_bytes_delta == 128);

    /* Commit attempts reservation outside write_mu and gets VX_ERR_MEMLIM */
    uint64_t commit_lsn = 0;
    st = vx_tx_commit(tx, &commit_lsn);
    assert(st == VX_ERR_MEMLIM);
    assert(tx->state == VX_TX_ABORTED);
    assert(db.hot_bytes == 0); /* Quota not leaked */

    vx_tx_destroy(tx);
    close(wal_fd);
    unlink(wal_path);
    teardown_mock_db(&db);
}

/* ============================================================================
 * 6. Test Atomic Batch Replay in Crash Recovery (vx_tx_replay_batch)
 * ============================================================================ */
static void test_tx_batch_recovery_replay(void) {
    printf("[6/7] Testing atomic WAL batch recovery replay...\n");

    mock_db_t db;
    setup_mock_db(&db, 0);

    const char *wal_path = "/tmp/test_v2_wal_recovery.wal";
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    /* Commit a TX batch with 3 operations */
    vx_tx_t *tx = NULL;
    vx_status_t st = vx_tx_begin(&db, &g_mock_ops, &db.write_mu, wal_fd, true, 300, &tx);
    assert(st == VX_OK);

    mock_mutation_payload_t m1 = { .table_id = 0, .delta_bal = 100 };
    mock_mutation_payload_t m2 = { .table_id = 0, .delta_bal = 200 };
    mock_mutation_payload_t m3 = { .table_id = 1, .delta_bal = 300 };
    st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m1, sizeof(m1));
    assert(st == VX_OK);
    st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m2, sizeof(m2));
    assert(st == VX_OK);
    st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m3, sizeof(m3));
    assert(st == VX_OK);

    uint64_t commit_lsn = 0;
    st = vx_tx_commit(tx, &commit_lsn);
    assert(st == VX_OK && commit_lsn == 11);
    vx_tx_destroy(tx);

    /* Reset database to pristine state (simulating crash before checkpoint) */
    teardown_mock_db(&db);
    setup_mock_db(&db, 0);
    assert(mock_get_published(&db)->tables[0].balance_sum == 0);

    /* Run recovery read from WAL and replay batch */
    vx_wal_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    uint64_t io_off = 0;
    vx_wal_frame_t frame;
    bool eof = false;

    st = vx_wal_read_next_frame(&reader, wal_fd, &io_off, &frame, &eof);
    assert(st == VX_OK && !eof);
    assert(frame.op_type == VX_WAL_OP_TX_BATCH);
    assert(frame.lsn == 11);

    st = vx_tx_replay_batch(&db, &g_mock_ops, &frame);
    assert(st == VX_OK);

    /* Assert all 3 operations replayed atomically */
    mock_cat_t *rec_pub = mock_get_published(&db);
    assert(rec_pub->tables[0].row_count == 2);
    assert(rec_pub->tables[0].balance_sum == 300);
    assert(rec_pub->tables[1].row_count == 1);
    assert(rec_pub->tables[1].balance_sum == 300);

    vx_wal_reader_destroy(&reader);
    close(wal_fd);
    unlink(wal_path);
    teardown_mock_db(&db);
}

/* ============================================================================
 * 7. Multi-Threaded Concurrent Reader / Writer Stress Test
 * ============================================================================ */
typedef struct stress_ctx {
    mock_db_t      *db;
    int             wal_fd;
    _Atomic bool    stop;
    _Atomic uint32_t commits;
    _Atomic uint32_t reads;
} stress_ctx_t;

static void *stress_reader_thread(void *arg) {
    stress_ctx_t *ctx = (stress_ctx_t *)arg;
    while (!atomic_load(&ctx->stop)) {
        vx_owner_t *snap = mock_acquire_snapshot(ctx->db, NULL);
        if (snap) {
            mock_cat_t *cat = (mock_cat_t *)snap;
            /* Invariant check: balance sums are always non-negative */
            assert(cat->tables[0].balance_sum >= 0);
            assert(cat->tables[1].balance_sum >= 0);
            vx_owner_release(snap);
            atomic_fetch_add(&ctx->reads, 1);
        }
    }
    return NULL;
}

static void *stress_writer_thread(void *arg) {
    stress_ctx_t *ctx = (stress_ctx_t *)arg;
    uint32_t tid = (uint32_t)(uintptr_t)pthread_self();
    uint32_t seq = 0;

    while (!atomic_load(&ctx->stop) && atomic_load(&ctx->commits) < 200) {
        vx_tx_t *tx = NULL;
        vx_status_t st = vx_tx_begin(ctx->db, &g_mock_ops, &ctx->db->write_mu,
                                     ctx->wal_fd, false, ((uint64_t)tid << 32) | seq, &tx);
        if (st != VX_OK) continue;

        mock_mutation_payload_t m = { .table_id = seq % 2, .delta_bal = 1 };
        st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&m, sizeof(m));
        if (st == VX_OK) {
            uint64_t lsn = 0;
            st = vx_tx_commit(tx, &lsn);
            if (st == VX_OK) {
                atomic_fetch_add(&ctx->commits, 1);
            }
        }
        vx_tx_destroy(tx);
        seq++;
    }
    return NULL;
}

static void test_tx_concurrent_stress(void) {
    printf("[7/7] Testing multi-threaded concurrency (readers + writers under lock-free EBR)...\n");

    mock_db_t db;
    setup_mock_db(&db, 0);

    const char *wal_path = "/tmp/test_v2_wal_stress.wal";
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    stress_ctx_t ctx = {
        .db = &db,
        .wal_fd = wal_fd,
        .stop = false,
        .commits = 0,
        .reads = 0
    };

    pthread_t readers[4];
    pthread_t writers[4];

    for (int i = 0; i < 4; ++i) {
        assert(pthread_create(&readers[i], NULL, stress_reader_thread, &ctx) == 0);
    }
    for (int i = 0; i < 4; ++i) {
        assert(pthread_create(&writers[i], NULL, stress_writer_thread, &ctx) == 0);
    }

    for (int i = 0; i < 4; ++i) {
        pthread_join(writers[i], NULL);
    }
    atomic_store(&ctx.stop, true);
    for (int i = 0; i < 4; ++i) {
        pthread_join(readers[i], NULL);
    }

    printf("      -> Verified %u concurrent commits and %u lock-free snapshot reads.\n",
           atomic_load(&ctx.commits), atomic_load(&ctx.reads));

    close(wal_fd);
    unlink(wal_path);
    teardown_mock_db(&db);
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */
int main(void) {
    printf("=== Starting Milestone M17 Transaction & Framed WAL Test Suite ===\n");
    test_wal_framed_io();
    test_tx_lifecycle();
    test_tx_rebase_on_flush();
    test_tx_conflict_detection();
    test_tx_backpressure();
    test_tx_batch_recovery_replay();
    test_tx_concurrent_stress();
    printf("=== ALL M17 TRANSACTION TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
