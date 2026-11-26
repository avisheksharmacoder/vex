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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vx_alloc.h"
#include "vx_bg.h"
#include "vx_owner.h"
#include "vx_tx.h"
#include "vx_types.h"

#define NUM_ACCOUNTS 16
#define INITIAL_BALANCE 10000
#define TOTAL_SUPPLY (NUM_ACCOUNTS * INITIAL_BALANCE)

static _Atomic int64_t  g_active_owners = 0;
static _Atomic uint64_t g_last_lsn      = 100;
static _Atomic bool     g_stop          = false;

typedef struct tsan_cat {
    vx_owner_t hdr;
    uint64_t   version;
    int64_t    balances[NUM_ACCOUNTS];
} tsan_cat_t;

static void tsan_cat_destroy(vx_owner_t *owner) {
    atomic_fetch_sub_explicit(&g_active_owners, 1, memory_order_relaxed);
    free(owner);
}

static tsan_cat_t *tsan_cat_create(uint64_t version) {
    tsan_cat_t *c = (tsan_cat_t *)malloc(sizeof(tsan_cat_t));
    if (!c) return NULL;
    vx_owner_init(&c->hdr, VX_OWN_CATALOG, tsan_cat_destroy);
    c->version = version;
    atomic_fetch_add_explicit(&g_active_owners, 1, memory_order_relaxed);
    return c;
}

static tsan_cat_t *tsan_cat_clone(const tsan_cat_t *src, uint64_t new_version) {
    tsan_cat_t *c = tsan_cat_create(new_version);
    if (!c) return NULL;
    memcpy(c->balances, src->balances, sizeof(src->balances));
    return c;
}

typedef struct tsan_db {
    pthread_mutex_t write_mu;
    pthread_mutex_t manifest_mu;
    pthread_mutex_t cat_mu;
    vx_owner_t     *published_cat;
    uint64_t        commit_seq;
    _Atomic size_t  hot_bytes;
    int             wal_fd;
} tsan_db_t;

typedef struct tsan_tx_transfer {
    uint32_t from_acct;
    uint32_t to_acct;
    int64_t  amount;
} tsan_tx_transfer_t;

/* --- Transaction Database Callbacks --- */

static vx_owner_t *tsan_tx_acquire_snapshot(void *db_ctx, uint64_t *out_commit_seq) {
    tsan_db_t *db = (tsan_db_t *)db_ctx;
    pthread_mutex_lock(&db->cat_mu);
    vx_owner_t *snap = vx_owner_retain(db->published_cat);
    if (out_commit_seq) *out_commit_seq = db->commit_seq;
    pthread_mutex_unlock(&db->cat_mu);
    return snap;
}

static vx_owner_t *tsan_tx_current_published(void *db_ctx, uint64_t *out_commit_seq) {
    tsan_db_t *db = (tsan_db_t *)db_ctx;
    pthread_mutex_lock(&db->cat_mu);
    vx_owner_t *cur = db->published_cat;
    if (out_commit_seq) *out_commit_seq = db->commit_seq;
    pthread_mutex_unlock(&db->cat_mu);
    return cur;
}

static vx_status_t tsan_tx_apply_cow_op(void *db_ctx, vx_owner_t *in_cat,
                                        uint32_t op_type, const uint8_t *payload, uint32_t payload_len,
                                        vx_owner_t **out_next_cat, uint64_t *out_table_bit,
                                        size_t *out_hot_bytes_added) {
    (void)db_ctx;
    if (op_type != VX_WAL_OP_PUT_ROW || payload_len != sizeof(tsan_tx_transfer_t)) {
        return VX_ERR_INVALID;
    }
    const tsan_tx_transfer_t *xfer = (const tsan_tx_transfer_t *)payload;
    if (xfer->from_acct >= NUM_ACCOUNTS || xfer->to_acct >= NUM_ACCOUNTS) {
        return VX_ERR_INVALID;
    }

    const tsan_cat_t *src = (const tsan_cat_t *)in_cat;
    tsan_cat_t       *dst = tsan_cat_clone(src, src->version + 1);
    if (!dst) return VX_ERR_OOM;

    dst->balances[xfer->from_acct] -= xfer->amount;
    dst->balances[xfer->to_acct]   += xfer->amount;

    *out_next_cat = &dst->hdr;
    if (out_table_bit) {
        *out_table_bit = (1ULL << (xfer->from_acct % 64)) | (1ULL << (xfer->to_acct % 64));
    }
    if (out_hot_bytes_added) *out_hot_bytes_added = 64;
    return VX_OK;
}

static bool tsan_tx_has_write_conflict(void *db_ctx, uint64_t base_commit_seq,
                                       uint64_t read_table_mask, uint64_t write_table_mask) {
    tsan_db_t *db = (tsan_db_t *)db_ctx;
    (void)read_table_mask;
    if (base_commit_seq != db->commit_seq) {
        /* Write-write conflict on overlapping table bits */
        return (write_table_mask & 0xFFFFULL) != 0;
    }
    return false;
}

static uint64_t tsan_tx_next_lsn(void *db_ctx) {
    (void)db_ctx;
    return atomic_fetch_add_explicit(&g_last_lsn, 1, memory_order_acq_rel) + 1;
}

static void tsan_tx_publish_ebr(void *db_ctx, vx_owner_t *new_cat, uint64_t commit_lsn, uint64_t write_table_mask) {
    tsan_db_t *db = (tsan_db_t *)db_ctx;
    (void)commit_lsn; (void)write_table_mask;
    pthread_mutex_lock(&db->cat_mu);
    vx_owner_t *old = db->published_cat;
    db->published_cat = new_cat;
    db->commit_seq++;
    pthread_mutex_unlock(&db->cat_mu);
    if (old) vx_owner_release(old);
}

static const vx_tx_db_ops_t g_tsan_tx_ops = {
    .acquire_snapshot   = tsan_tx_acquire_snapshot,
    .current_published  = tsan_tx_current_published,
    .apply_cow_op       = tsan_tx_apply_cow_op,
    .has_write_conflict = tsan_tx_has_write_conflict,
    .next_lsn           = tsan_tx_next_lsn,
    .publish_ebr        = tsan_tx_publish_ebr
};

/* --- Flusher & Compactor Hooks --- */

static uint64_t tsan_bg_alloc_seg_id(void *db_ctx) {
    (void)db_ctx;
    static _Atomic uint64_t next_id = 1;
    return atomic_fetch_add_explicit(&next_id, 1, memory_order_relaxed);
}

static vx_status_t tsan_bg_snapshot_flush(void *db_ctx, vx_flush_batch_t *out_batch) {
    (void)db_ctx;
    memset(out_batch, 0, sizeof(*out_batch));
    out_batch->flush_lsn = atomic_load_explicit(&g_last_lsn, memory_order_relaxed);
    return VX_OK;
}

static vx_status_t tsan_bg_commit_flush_manifest(void *db_ctx, const vx_flush_batch_t *batch,
                                                 const vx_man_seg_t *new_segs, uint32_t n_new_segs) {
    (void)db_ctx; (void)batch; (void)new_segs; (void)n_new_segs;
    return VX_OK;
}

static vx_status_t tsan_bg_swap_flush_catalog(void *db_ctx, const vx_flush_batch_t *batch,
                                              vx_seg_t *const *new_segs, uint32_t n_new_segs,
                                              size_t *out_freed_hot_bytes) {
    tsan_db_t *db = (tsan_db_t *)db_ctx;
    (void)batch; (void)new_segs; (void)n_new_segs;
    pthread_mutex_lock(&db->cat_mu);
    tsan_cat_t *old_pub = (tsan_cat_t *)db->published_cat;
    tsan_cat_t *flushed = tsan_cat_clone(old_pub, old_pub->version);
    db->published_cat = &flushed->hdr;
    pthread_mutex_unlock(&db->cat_mu);
    vx_owner_release(&old_pub->hdr);

    if (out_freed_hot_bytes) *out_freed_hot_bytes = 128;
    return VX_OK;
}

static vx_status_t tsan_bg_truncate_wal(void *db_ctx, uint64_t checkpoint_lsn) {
    tsan_db_t *db = (tsan_db_t *)db_ctx;
    (void)checkpoint_lsn;
    pthread_mutex_lock(&db->write_mu);
    if (db->wal_fd >= 0) {
        int trc = ftruncate(db->wal_fd, 0);
        (void)trc;
        off_t sk = lseek(db->wal_fd, 0, SEEK_SET);
        (void)sk;
    }
    pthread_mutex_unlock(&db->write_mu);
    return VX_OK;
}

static vx_status_t tsan_bg_plan_compaction(void *db_ctx, vx_compact_plan_t *out_plan, bool *out_found) {
    (void)db_ctx;
    memset(out_plan, 0, sizeof(*out_plan));
    *out_found = false;
    return VX_OK;
}

static vx_status_t tsan_bg_commit_compact_manifest(void *db_ctx, const vx_compact_plan_t *plan,
                                                   const vx_man_seg_t *new_meta) {
    (void)db_ctx; (void)plan; (void)new_meta;
    return VX_OK;
}

static vx_status_t tsan_bg_swap_compact_catalog(void *db_ctx, const vx_compact_plan_t *plan,
                                                vx_seg_t *new_seg, const vx_man_seg_t *new_meta,
                                                vx_delmap_t **new_delmaps, uint64_t *out_caught_up_lsn) {
    (void)db_ctx; (void)plan; (void)new_seg; (void)new_meta; (void)new_delmaps;
    if (out_caught_up_lsn) *out_caught_up_lsn = atomic_load_explicit(&g_last_lsn, memory_order_relaxed);
    return VX_OK;
}

static const vx_bg_ops_t g_tsan_bg_ops = {
    .alloc_seg_id            = tsan_bg_alloc_seg_id,
    .snapshot_flush          = tsan_bg_snapshot_flush,
    .commit_flush_manifest   = tsan_bg_commit_flush_manifest,
    .swap_flush_catalog      = tsan_bg_swap_flush_catalog,
    .truncate_wal            = tsan_bg_truncate_wal,
    .plan_compaction         = tsan_bg_plan_compaction,
    .commit_compact_manifest = tsan_bg_commit_compact_manifest,
    .swap_compact_catalog    = tsan_bg_swap_compact_catalog
};

/* --- Worker Threads --- */

typedef struct thread_arg {
    tsan_db_t *db;
    int        tid;
    uint64_t   success_count;
    uint64_t   conflict_count;
} thread_arg_t;

/* 4 Readers: retain snapshot refcount, pause, verify total supply invariance */
static void *reader_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    tsan_db_t    *db = ta->db;

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        uint64_t seq = 0;
        vx_owner_t *snap = tsan_tx_acquire_snapshot(db, &seq);
        assert(snap != NULL);

        /* Simulate non-trivial query execution while holding snapshot */
        usleep(1000); /* 1ms pause under EBR retention */

        const tsan_cat_t *cat = (const tsan_cat_t *)snap;
        int64_t sum = 0;
        for (int i = 0; i < NUM_ACCOUNTS; ++i) {
            sum += cat->balances[i];
        }

        /* Strict ACID conservation law assertion */
        assert(sum == TOTAL_SUPPLY);

        vx_owner_release(snap);
        ta->success_count++;
    }
    return NULL;
}

/* 4 Writers: atomic banking transfers across accounts */
static void *writer_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    tsan_db_t    *db = ta->db;
    unsigned int  seed = (unsigned int)(12345 + ta->tid * 100);

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        uint32_t a = (uint32_t)(rand_r(&seed) % NUM_ACCOUNTS);
        uint32_t b = (uint32_t)(rand_r(&seed) % NUM_ACCOUNTS);
        if (a == b) b = (b + 1) % NUM_ACCOUNTS;
        int64_t amt = (int64_t)(1 + (rand_r(&seed) % 25));

        tsan_tx_transfer_t xfer = { .from_acct = a, .to_acct = b, .amount = amt };

        vx_tx_t *tx = NULL;
        vx_status_t st = vx_tx_begin(db, &g_tsan_tx_ops, &db->write_mu, db->wal_fd, false, 777, &tx);
        assert(st == VX_OK && tx);

        st = vx_tx_stage_op(tx, VX_WAL_OP_PUT_ROW, (const uint8_t *)&xfer, sizeof(xfer));
        assert(st == VX_OK);

        uint64_t commit_lsn = 0;
        st = vx_tx_commit(tx, &commit_lsn);
        if (st == VX_OK) {
            assert(commit_lsn > 0);
            ta->success_count++;
        } else if (st == VX_ERR_CONFLICT) {
            vx_tx_rollback(tx);
            ta->conflict_count++;
            usleep(200); /* backoff before retry */
        } else {
            fprintf(stderr, "Unexpected tx error: %d\n", st);
            assert(false);
        }
        vx_tx_destroy(tx);
    }
    return NULL;
}

int main(void) {
    printf("=== Starting Pure-C TSan Multi-Threaded Core Torture Test ===\n");
    const char *test_dir = "/tmp/test_v2_tsan_c_core";
    (void)mkdir(test_dir, 0755);

    char wal_path[256];
    snprintf(wal_path, sizeof(wal_path), "%s/wal.wal", test_dir);
    unlink(wal_path);
    int wal_fd = open(wal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(wal_fd >= 0);

    tsan_db_t db;
    memset(&db, 0, sizeof(db));
    pthread_mutex_init(&db.write_mu, NULL);
    pthread_mutex_init(&db.manifest_mu, NULL);
    pthread_mutex_init(&db.cat_mu, NULL);
    db.wal_fd = wal_fd;
    db.commit_seq = 1;
    atomic_init(&db.hot_bytes, 0);

    /* Initialize base catalog */
    tsan_cat_t *init_cat = tsan_cat_create(1);
    assert(init_cat);
    for (int i = 0; i < NUM_ACCOUNTS; ++i) {
        init_cat->balances[i] = INITIAL_BALANCE;
    }
    db.published_cat = &init_cat->hdr;

    /* Start background flusher & compactor */
    vx_flusher_t flusher;
    vx_status_t st = vx_flusher_start(&flusher, test_dir, "/tmp", 4 * 1024 * 1024,
                                      &db.manifest_mu, &g_tsan_bg_ops, &db);
    assert(st == VX_OK);
    assert(flusher.started);

    vx_compactor_t compactor;
    st = vx_compactor_start(&compactor, test_dir, "/tmp", &db.manifest_mu,
                            &g_tsan_bg_ops, &db);
    assert(st == VX_OK);
    assert(compactor.started);

    /* Spawn 4 readers + 4 writers */
    pthread_t    readers[4];
    thread_arg_t r_args[4];
    pthread_t    writers[4];
    thread_arg_t w_args[4];

    for (int i = 0; i < 4; ++i) {
        r_args[i] = (thread_arg_t){ .db = &db, .tid = i, .success_count = 0, .conflict_count = 0 };
        int rc = pthread_create(&readers[i], NULL, reader_worker, &r_args[i]);
        assert(rc == 0);
    }
    for (int i = 0; i < 4; ++i) {
        w_args[i] = (thread_arg_t){ .db = &db, .tid = 10 + i, .success_count = 0, .conflict_count = 0 };
        int rc = pthread_create(&writers[i], NULL, writer_worker, &w_args[i]);
        assert(rc == 0);
    }

    printf("[INFO] Torture running across 4 readers, 4 writers, flusher & compactor...\n");

    /* Run concurrent torture for 2.0 seconds */
    for (int k = 0; k < 20; ++k) {
        usleep(100000); /* 100ms */
        vx_compactor_kick(&compactor);
    }

    /* Signal stop */
    atomic_store_explicit(&g_stop, true, memory_order_relaxed);

    /* Wake flusher & compactor if waiting */
    pthread_mutex_lock(&flusher.mu);
    pthread_cond_broadcast(&flusher.wake_cv);
    pthread_mutex_unlock(&flusher.mu);

    pthread_mutex_lock(&compactor.mu);
    pthread_cond_broadcast(&compactor.wake_cv);
    pthread_mutex_unlock(&compactor.mu);

    /* Join all threads */
    uint64_t total_reads = 0;
    uint64_t total_writes = 0;
    uint64_t total_conflicts = 0;

    for (int i = 0; i < 4; ++i) {
        pthread_join(readers[i], NULL);
        total_reads += r_args[i].success_count;
    }
    for (int i = 0; i < 4; ++i) {
        pthread_join(writers[i], NULL);
        total_writes += w_args[i].success_count;
        total_conflicts += w_args[i].conflict_count;
    }

    printf("[INFO] Torture finished: %llu reads, %llu commits, %llu conflicts\n",
           (unsigned long long)total_reads,
           (unsigned long long)total_writes,
           (unsigned long long)total_conflicts);

    assert(total_reads > 0);
    assert(total_writes > 0);

    /* Stop compactor & flusher in clean shutdown order */
    vx_compactor_stop(&compactor);
    vx_flusher_stop(&flusher);

    /* Release final published catalog */
    pthread_mutex_lock(&db.cat_mu);
    vx_owner_t *final_cat = db.published_cat;
    db.published_cat = NULL;
    pthread_mutex_unlock(&db.cat_mu);
    if (final_cat) vx_owner_release(final_cat);

    close(wal_fd);
    unlink(wal_path);
    pthread_mutex_destroy(&db.write_mu);
    pthread_mutex_destroy(&db.manifest_mu);
    pthread_mutex_destroy(&db.cat_mu);

    /* Assert refcnt == 0 across all dynamically allocated owners */
    int64_t remaining_owners = atomic_load_explicit(&g_active_owners, memory_order_relaxed);
    printf("[INFO] Remaining active owners on shutdown: %lld\n", (long long)remaining_owners);
    assert(remaining_owners == 0);

    printf("=== Pure-C TSan Multi-Threaded Core Torture PASSED 100%%! ===\n");
    return 0;
}
