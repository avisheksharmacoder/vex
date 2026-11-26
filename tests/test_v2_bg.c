#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/vx_types.h"
#include "src/vx_owner.h"
#include "src/vx_chunk.h"
#include "src/vx_seg_fmt.h"
#include "src/vx_seg.h"
#include "src/vx_manifest_fmt.h"
#include "src/vx_manifest.h"
#include "src/vx_bg.h"

static void mock_chunk_destroy(vx_owner_t *o) {
    vx_chunk_t *ch = (vx_chunk_t *)o;
    if (ch->cols) {
        for (uint32_t i = 0; i < ch->ncols; ++i) {
            free((void *)ch->cols[i].data);
        }
        free(ch->cols);
    }
    free(ch);
}

static void mock_delmap_destroy(vx_owner_t *o) {
    free(o);
}

static void mock_seg_destroy(vx_owner_t *o) {
    vx_seg_t *seg = (vx_seg_t *)o;
    if (seg->chunk_views) {
        for (uint32_t i = 0; i < seg->nchunks; ++i) {
            if (seg->chunk_views[i]) {
                vx_owner_release(&seg->chunk_views[i]->hdr);
            }
        }
        free(seg->chunk_views);
    }
    free(seg);
}

static void setup_test_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    assert(system(cmd) == 0);
}

/* 1. Test Hot RAM accounting and max_memory_bytes backpressure */
static void test_flusher_reserve_and_backpressure(void) {
    printf("[1/5] Testing Hot RAM accounting & max_memory_bytes backpressure...\n");

    pthread_mutex_t manifest_mu;
    pthread_mutex_init(&manifest_mu, NULL);

    vx_bg_ops_t ops;
    memset(&ops, 0, sizeof(ops));

    vx_flusher_t f;
    memset(&f, 0, sizeof(f));
    f.max_memory_bytes = 1000; /* limit = 1000 B; high water = 700 B */
    pthread_mutex_init(&f.mu, NULL);
    pthread_cond_init(&f.wake_cv, NULL);
    pthread_cond_init(&f.freed_cv, NULL);
    atomic_init(&f.hot_bytes, 0);

    /* 1. Reserve 500 bytes (under high-water) */
    vx_status_t st = vx_flusher_reserve_hot(&f, 500);
    assert(st == VX_OK);
    assert(atomic_load(&f.hot_bytes) == 500);
    assert(f.flush_requested == false);

    /* 2. Reserve 300 bytes (total 800 >= 700 high-water) -> triggers flush request */
    st = vx_flusher_reserve_hot(&f, 300);
    assert(st == VX_OK);
    assert(atomic_load(&f.hot_bytes) == 800);
    assert(f.flush_requested == true);

    /* 3. Release 400 bytes -> hot_bytes = 400 */
    vx_flusher_release_hot(&f, 400);
    assert(atomic_load(&f.hot_bytes) == 400);

    /* 4. Reserve 500 bytes -> total 900 */
    st = vx_flusher_reserve_hot(&f, 500);
    assert(st == VX_OK);
    assert(atomic_load(&f.hot_bytes) == 900);

    /* 5. Attempt to reserve 200 bytes (900 + 200 > 1000) -> blocks, times out after 200ms -> VX_ERR_MEMLIM */
    st = vx_flusher_reserve_hot(&f, 200);
    assert(st == VX_ERR_MEMLIM);
    assert(atomic_load(&f.hot_bytes) == 900); /* reservation rejected */

    pthread_cond_destroy(&f.freed_cv);
    pthread_cond_destroy(&f.wake_cv);
    pthread_mutex_destroy(&f.mu);
    pthread_mutex_destroy(&manifest_mu);

    printf("      -> Hot RAM backpressure & timeout PASSED.\n");
}

/* Mock context for Flusher pipeline testing */
typedef struct mock_db {
    uint64_t next_seg_id;
    uint64_t last_ckpt_lsn;
    uint64_t last_wal_trunc_lsn;
    uint32_t commit_flush_calls;
    uint32_t swap_flush_calls;
} mock_db_t;

static uint64_t mock_alloc_seg_id(void *db_ctx) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    return db->next_seg_id++;
}

static vx_status_t mock_snapshot_flush(void *db_ctx, vx_flush_batch_t *out_batch) {
    (void)db_ctx;
    out_batch->flush_lsn = 100;
    out_batch->n_tables = 1;
    out_batch->tables = (vx_flush_table_snap_t *)calloc(1, sizeof(vx_flush_table_snap_t));

    vx_flush_table_snap_t *ts = &out_batch->tables[0];
    ts->table_id = 1;
    ts->ncols = 1;
    ts->pk_col_idx = 0;
    static const vx_type_t col_types[1] = { VX_TYPE_U64 };
    ts->col_types = col_types;
    ts->nchunks = 1;
    ts->hot_bytes = 4096;

    ts->chunks = (vx_chunk_t **)calloc(1, sizeof(vx_chunk_t *));
    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(vx_chunk_t));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, mock_chunk_destroy);
    ch->nrows = 50; ch->ncols = 1;
    ch->cols = (vx_col_chunk_t *)calloc(1, sizeof(vx_col_chunk_t));
    ch->cols[0].type = VX_TYPE_U64;
    uint64_t *data = (uint64_t *)malloc(50 * sizeof(uint64_t));
    for (uint32_t i = 0; i < 50; ++i) data[i] = i + 1;
    ch->cols[0].data = data;
    ts->chunks[0] = ch;

    /* Dirty delete snapshot */
    out_batch->n_dirty_dels = 1;
    out_batch->dirty_dels = (vx_dirty_del_snap_t *)calloc(1, sizeof(vx_dirty_del_snap_t));
    vx_dirty_del_snap_t *ds = &out_batch->dirty_dels[0];
    ds->table_id = 1;
    ds->segment_id = 10;
    ds->new_del_gen = 2;
    ds->nrows = 100;
    ds->nchunks = 1;
    ds->chunk_delmaps = (vx_delmap_t **)calloc(1, sizeof(vx_delmap_t *));
    vx_delmap_t *dm = (vx_delmap_t *)calloc(1, sizeof(vx_delmap_t));
    vx_owner_init(&dm->hdr, VX_OWN_DELMAP, mock_delmap_destroy);
    dm->nrows = 100;
    dm->live_count = 99;
    dm->bits[0] = 1ULL; /* delete row 0 */
    ds->chunk_delmaps[0] = dm;

    return VX_OK;
}

static vx_status_t mock_commit_flush_manifest(void *db_ctx, const vx_flush_batch_t *batch,
                                             const vx_man_seg_t *new_segs, uint32_t n_new_segs) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    db->commit_flush_calls++;
    db->last_ckpt_lsn = batch->flush_lsn;
    assert(n_new_segs == 1);
    assert(new_segs[0].nrows == 50);
    return VX_OK;
}

static vx_status_t mock_swap_flush_catalog(void *db_ctx, const vx_flush_batch_t *batch,
                                           vx_seg_t *const *new_segs, uint32_t n_new_segs,
                                           size_t *out_freed_hot_bytes) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    db->swap_flush_calls++;
    (void)batch;
    assert(n_new_segs == 1);
    assert(new_segs[0] != NULL);
    *out_freed_hot_bytes = 4096;
    return VX_OK;
}

static vx_status_t mock_truncate_wal(void *db_ctx, uint64_t checkpoint_lsn) {
    mock_db_t *db = (mock_db_t *)db_ctx;
    db->last_wal_trunc_lsn = checkpoint_lsn;
    return VX_OK;
}

/* 2. Test single flush pass: Hot chunks + dirty .del -> L0 .vx + .del sidecar */
static void test_flusher_run_once_pipeline(void) {
    printf("[2/5] Testing vx_flusher_run_once pipeline & catalog swap...\n");

    const char *dir = "/tmp/vex_test_flusher";
    setup_test_dir(dir);

    pthread_mutex_t manifest_mu;
    pthread_mutex_init(&manifest_mu, NULL);

    mock_db_t db = { .next_seg_id = 1 };
    vx_bg_ops_t ops = {
        .alloc_seg_id = mock_alloc_seg_id,
        .snapshot_flush = mock_snapshot_flush,
        .commit_flush_manifest = mock_commit_flush_manifest,
        .swap_flush_catalog = mock_swap_flush_catalog,
        .truncate_wal = mock_truncate_wal
    };

    vx_flusher_t f;
    memset(&f, 0, sizeof(f));
    f.dir_path = dir;
    f.spill_dir = "/tmp";
    f.db_ctx = &db;
    f.ops = &ops;
    f.manifest_mu = &manifest_mu;
    f.max_memory_bytes = 100000;
    atomic_init(&f.hot_bytes, 4096);
    pthread_mutex_init(&f.mu, NULL);
    pthread_cond_init(&f.wake_cv, NULL);
    pthread_cond_init(&f.freed_cv, NULL);

    bool did_work = false;
    vx_status_t st = vx_flusher_run_once(&f, &did_work);
    assert(st == VX_OK);
    assert(did_work == true);
    assert(db.commit_flush_calls == 1);
    assert(db.swap_flush_calls == 1);
    assert(db.last_ckpt_lsn == 100);
    assert(db.last_wal_trunc_lsn == 100);
    assert(atomic_load(&f.hot_bytes) == 0); /* 4096 bytes freed */

    /* Verify files created on disk */
    char seg_path[512], del_path[512];
    assert(vx_seg_format_path(dir, 1, seg_path, sizeof(seg_path)) == VX_OK);
    assert(vx_del_format_path(dir, 10, 2, del_path, sizeof(del_path)) == VX_OK);
    assert(access(seg_path, F_OK) == 0);
    assert(access(del_path, F_OK) == 0);

    pthread_cond_destroy(&f.freed_cv);
    pthread_cond_destroy(&f.wake_cv);
    pthread_mutex_destroy(&f.mu);
    pthread_mutex_destroy(&manifest_mu);

    printf("      -> vx_flusher_run_once PASSED.\n");
}

/* 3. Test concurrent delete catch-up during leveled compaction */
static void test_compactor_catchup_deletes(void) {
    printf("[3/5] Testing concurrent delete catch-up (XOR & row_map)...\n");

    /* Create victim with 10 rows (rows 0..9) */
    vx_compact_victim_t victim;
    memset(&victim, 0, sizeof(victim));
    victim.meta.nrows = 10;

    vx_seg_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.nchunks = 1;
    victim.seg = &seg;

    /* Snapshot delmap: row 2 was deleted at start of compaction */
    vx_delmap_t snap_dm;
    memset(&snap_dm, 0, sizeof(snap_dm));
    snap_dm.generation = 1;
    snap_dm.nrows = 10;
    snap_dm.live_count = 9;
    snap_dm.bits[0] = (1ULL << 2); /* row 2 deleted */
    vx_delmap_t *snap_dms[1] = { &snap_dm };
    victim.snap_delmaps = snap_dms;

    /* row_map for live rows:
     * old row 0 -> new 0
     * old row 1 -> new 1
     * old row 2 -> UINT32_MAX (already dead)
     * old row 3 -> new 2
     * old row 4 -> new 3
     * old row 5 -> new 4
     * old row 6 -> new 5
     * old row 7 -> new 6
     * old row 8 -> new 7
     * old row 9 -> new 8 */
    uint32_t row_map[10] = { 0, 1, UINT32_MAX, 2, 3, 4, 5, 6, 7, 8 };
    victim.row_map = row_map;

    /* Current live delmap under write_mu: row 2 AND row 5 are now deleted (row 5 deleted mid-compaction!) */
    vx_delmap_t curr_dm;
    memset(&curr_dm, 0, sizeof(curr_dm));
    curr_dm.generation = 2; /* changed generation */
    curr_dm.nrows = 10;
    curr_dm.live_count = 8;
    curr_dm.bits[0] = (1ULL << 2) | (1ULL << 5); /* rows 2 and 5 deleted */
    const vx_delmap_t *curr_dms[1] = { &curr_dm };

    /* New segment has 9 rows initially, 1 chunk */
    vx_delmap_t *new_delmaps[1] = { NULL };
    uint32_t new_live = 9;

    vx_status_t st = vx_compact_catchup_deletes(&victim, curr_dms, 9, 1, new_delmaps, &new_live);
    assert(st == VX_OK);
    assert(new_delmaps[0] != NULL);
    assert(new_live == 8);

    /* Verify row 5 in old victim mapped to row 4 in new segment, and is tombstoned! */
    assert(((new_delmaps[0]->bits[0] >> 4) & 1ULL) == 1ULL);
    assert(new_delmaps[0]->live_count == 8);

    /* Clean up created delmap */
    vx_owner_release(&new_delmaps[0]->hdr);

    printf("      -> Concurrent delete catch-up PASSED.\n");
}

/* 4. Test leveled compaction execution pass */
static uint32_t g_compact_plan_calls = 0;
static uint32_t g_compact_commit_calls = 0;
static uint32_t g_compact_swap_calls = 0;

static vx_status_t mock_plan_compaction(void *db_ctx, vx_compact_plan_t *out_plan, bool *out_found) {
    (void)db_ctx;
    if (g_compact_plan_calls++ > 0) {
        *out_found = false;
        return VX_OK;
    }

    *out_found = true;
    out_plan->table_id = 1;
    out_plan->ncols = 1;
    out_plan->pk_col_idx = 0;
    static const vx_type_t col_types[1] = { VX_TYPE_U64 };
    out_plan->col_types = col_types;
    out_plan->target_level = 1;
    out_plan->nvictims = 1;
    out_plan->victims = (vx_compact_victim_t *)calloc(1, sizeof(vx_compact_victim_t));

    /* Victim segment with 20 rows */
    vx_compact_victim_t *cv = &out_plan->victims[0];
    cv->meta.segment_id = 99;
    cv->meta.nrows = 20;
    cv->meta.live_rows = 20;

    /* Mock segment with 1 chunk */
    vx_seg_t *seg = (vx_seg_t *)calloc(1, sizeof(vx_seg_t));
    vx_owner_init(&seg->hdr, VX_OWN_SEGMENT, mock_seg_destroy);
    seg->nchunks = 1;
    seg->chunk_views = (vx_chunk_t **)calloc(1, sizeof(vx_chunk_t *));
    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(vx_chunk_t));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, mock_chunk_destroy);
    ch->nrows = 20; ch->ncols = 1;
    ch->cols = (vx_col_chunk_t *)calloc(1, sizeof(vx_col_chunk_t));
    ch->cols[0].type = VX_TYPE_U64;
    uint64_t *vals = (uint64_t *)malloc(20 * sizeof(uint64_t));
    for (uint32_t i = 0; i < 20; ++i) vals[i] = i * 2;
    ch->cols[0].data = vals;
    seg->chunk_views[0] = ch;
    cv->seg = seg;

    return VX_OK;
}

static vx_status_t mock_commit_compact_manifest(void *db_ctx, const vx_compact_plan_t *plan,
                                                const vx_man_seg_t *new_meta) {
    (void)db_ctx; (void)plan;
    g_compact_commit_calls++;
    assert(new_meta != NULL);
    assert(new_meta->nrows == 20);
    assert(new_meta->level == 1);
    return VX_OK;
}

static vx_status_t mock_swap_compact_catalog(void *db_ctx, const vx_compact_plan_t *plan,
                                             vx_seg_t *new_seg, const vx_man_seg_t *new_meta,
                                             vx_delmap_t **new_delmaps, uint64_t *out_caught_up_lsn) {
    (void)db_ctx; (void)plan; (void)new_meta;
    g_compact_swap_calls++;
    assert(new_seg != NULL);
    *out_caught_up_lsn = 0;
    if (new_delmaps) {
        for (uint32_t i = 0; i < new_seg->nchunks; ++i) {
            if (new_delmaps[i]) vx_owner_release(&new_delmaps[i]->hdr);
        }
        free(new_delmaps);
    }
    return VX_OK;
}

static void test_compactor_run_once(void) {
    printf("[4/5] Testing vx_compactor_run_once leveled compaction pass...\n");

    const char *dir = "/tmp/vex_test_compactor";
    setup_test_dir(dir);

    pthread_mutex_t manifest_mu;
    pthread_mutex_init(&manifest_mu, NULL);

    mock_db_t db = { .next_seg_id = 500 };
    vx_bg_ops_t ops = {
        .alloc_seg_id = mock_alloc_seg_id,
        .plan_compaction = mock_plan_compaction,
        .commit_compact_manifest = mock_commit_compact_manifest,
        .swap_compact_catalog = mock_swap_compact_catalog
    };

    vx_compactor_t c;
    memset(&c, 0, sizeof(c));
    c.dir_path = dir;
    c.spill_dir = "/tmp";
    c.db_ctx = &db;
    c.ops = &ops;
    c.manifest_mu = &manifest_mu;

    bool did_work = false;
    vx_status_t st = vx_compactor_run_once(&c, &did_work);
    assert(st == VX_OK);
    assert(did_work == true);
    assert(g_compact_commit_calls == 1);
    assert(g_compact_swap_calls == 1);

    /* Verify new segment file created */
    char new_path[512];
    assert(vx_seg_format_path(dir, 500, new_path, sizeof(new_path)) == VX_OK);
    assert(access(new_path, F_OK) == 0);

    pthread_mutex_destroy(&manifest_mu);
    printf("      -> vx_compactor_run_once PASSED.\n");
}

/* 5. Test background thread lifecycle (start, signal, stop) */
static void test_flusher_and_compactor_threads(void) {
    printf("[5/5] Testing background flusher & compactor thread lifecycle...\n");

    const char *dir = "/tmp/vex_test_threads";
    setup_test_dir(dir);

    pthread_mutex_t manifest_mu;
    pthread_mutex_init(&manifest_mu, NULL);

    vx_bg_ops_t ops;
    memset(&ops, 0, sizeof(ops));

    /* Start flusher */
    vx_flusher_t f;
    vx_status_t st = vx_flusher_start(&f, dir, "/tmp", 1000000, &manifest_mu, &ops, NULL);
    assert(st == VX_OK);
    assert(f.started == true);

    /* Start compactor */
    vx_compactor_t c;
    st = vx_compactor_start(&c, dir, "/tmp", &manifest_mu, &ops, NULL);
    assert(st == VX_OK);
    assert(c.started == true);

    /* Kick compactor */
    vx_compactor_kick(&c);

    /* Stop compactor first (clean shutdown order) */
    vx_compactor_stop(&c);
    assert(c.started == false);

    /* Stop flusher */
    vx_flusher_stop(&f);
    assert(f.started == false);

    pthread_mutex_destroy(&manifest_mu);
    printf("      -> Thread lifecycle PASSED.\n");
}

int main(void) {
    printf("=== V2 Background Flusher & Leveled Compactor Verification ===\n");
    test_flusher_reserve_and_backpressure();
    test_flusher_run_once_pipeline();
    test_compactor_catchup_deletes();
    test_compactor_run_once();
    test_flusher_and_compactor_threads();
    printf("All 5 background coordinator tests passed successfully!\n");
    return 0;
}
