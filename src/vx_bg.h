#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include "vx_manifest.h"

#define VX_FLUSH_HIGH_WATER_PCT   70u
#define VX_FLUSH_WAIT_TIMEOUT_MS 200u
#define VX_COMPACT_TOMBSTONE_PCT  25u
#define VX_COMPACT_L0_TRIGGER      4u
#define VX_COMPACT_TARGET_BYTES  (128ULL * 1024ULL * 1024ULL) /* 128 MiB */

/* Snapshot of one table's sealed Hot chunks ready for eviction */
typedef struct vx_flush_table_snap {
    uint32_t          table_id;
    uint32_t          ncols;
    uint32_t          pk_col_idx;
    const vx_type_t  *col_types;
    vx_chunk_t      **chunks;     /* retained [nchunks] */
    vx_delmap_t     **delmaps;    /* retained [nchunks], entries may be NULL */
    uint32_t          nchunks;
    size_t            hot_bytes;
} vx_flush_table_snap_t;

/* Snapshot of one cold segment with dirty in-memory deletes <= flush_lsn */
typedef struct vx_dirty_del_snap {
    uint32_t      table_id;
    uint64_t      segment_id;
    uint64_t      new_del_gen;
    uint32_t      nrows;
    uint32_t      nchunks;
    vx_delmap_t **chunk_delmaps;  /* retained [nchunks] */
    uint32_t      live_rows;      /* populated by vx_del_write() */
} vx_dirty_del_snap_t;

typedef struct vx_flush_batch {
    uint64_t               flush_lsn;
    vx_flush_table_snap_t *tables;
    uint32_t               n_tables;
    vx_dirty_del_snap_t   *dirty_dels;
    uint32_t               n_dirty_dels;
} vx_flush_batch_t;

/* Snapshot of victim cold segments chosen for leveled compaction */
typedef struct vx_compact_victim {
    vx_man_seg_t  meta;
    vx_seg_t     *seg;            /* retained */
    vx_delmap_t **snap_delmaps;   /* retained [seg->nchunks] */
    uint32_t     *row_map;        /* [meta.nrows]: old_row -> new_row, or UINT32_MAX */
} vx_compact_victim_t;

typedef struct vx_compact_plan {
    uint32_t             table_id;
    uint32_t             ncols;
    uint32_t             pk_col_idx;
    const vx_type_t     *col_types;
    uint16_t             target_level;
    vx_compact_victim_t *victims;
    uint32_t             nvictims;
} vx_compact_plan_t;

/* Narrow DB hooks implemented in vx_db.c */
typedef struct vx_bg_ops {
    /* Allocates monotonic segment_id from db->manifest.next_seg_id (called under manifest_mu) */
    uint64_t    (*alloc_seg_id)(void *db_ctx);

    /* Flusher hooks */
    vx_status_t (*snapshot_flush)(void *db_ctx, vx_flush_batch_t *out_batch);
    vx_status_t (*commit_flush_manifest)(void *db_ctx, const vx_flush_batch_t *batch,
                                         const vx_man_seg_t *new_segs, uint32_t n_new_segs);
    vx_status_t (*swap_flush_catalog)(void *db_ctx, const vx_flush_batch_t *batch,
                                      vx_seg_t *const *new_segs, uint32_t n_new_segs,
                                      size_t *out_freed_hot_bytes);
    vx_status_t (*truncate_wal)(void *db_ctx, uint64_t checkpoint_lsn);

    /* Compaction hooks */
    vx_status_t (*plan_compaction)(void *db_ctx, vx_compact_plan_t *out_plan, bool *out_found);
    vx_status_t (*commit_compact_manifest)(void *db_ctx, const vx_compact_plan_t *plan,
                                           const vx_man_seg_t *new_meta);
    /* Acquires write_mu, returns current live chunk_delmaps for victim[v] so compactor can XOR */
    vx_status_t (*swap_compact_catalog)(void *db_ctx, const vx_compact_plan_t *plan,
                                        vx_seg_t *new_seg, const vx_man_seg_t *new_meta,
                                        vx_delmap_t **new_delmaps, uint64_t *out_caught_up_lsn);
} vx_bg_ops_t;

/* --- Flusher & Compactor Handles --- */
typedef struct vx_flusher {
    const char        *dir_path;
    const char        *spill_dir;
    void              *db_ctx;
    const vx_bg_ops_t *ops;
    pthread_mutex_t   *manifest_mu; /* shared with compactor & DDL */

    size_t             max_memory_bytes;
    _Atomic size_t     hot_bytes;

    pthread_mutex_t    mu;
    pthread_cond_t     wake_cv;     /* writers -> flusher */
    pthread_cond_t     freed_cv;    /* flusher -> blocked writers */
    pthread_t          thread;
    bool               started;
    bool               stop;
    bool               flush_requested;
    vx_status_t        last_err;
} vx_flusher_t;

typedef struct vx_compactor {
    const char        *dir_path;
    const char        *spill_dir;
    void              *db_ctx;
    const vx_bg_ops_t *ops;
    pthread_mutex_t   *manifest_mu;

    pthread_mutex_t    mu;
    pthread_cond_t     wake_cv;
    pthread_t          thread;
    bool               started;
    bool               stop;
    bool               kick_requested;
    vx_status_t        last_err;
} vx_compactor_t;

/* Flusher API */
vx_status_t vx_flusher_start(vx_flusher_t *f, const char *dir_path, const char *spill_dir,
                             size_t max_memory_bytes, pthread_mutex_t *manifest_mu,
                             const vx_bg_ops_t *ops, void *db_ctx);
void        vx_flusher_stop(vx_flusher_t *f);
vx_status_t vx_flusher_reserve_hot(vx_flusher_t *f, size_t add_bytes);
void        vx_flusher_release_hot(vx_flusher_t *f, size_t sub_bytes);
vx_status_t vx_flusher_run_once(vx_flusher_t *f, bool *out_did_work);

/* Compactor API */
vx_status_t vx_compactor_start(vx_compactor_t *c, const char *dir_path, const char *spill_dir,
                               pthread_mutex_t *manifest_mu, const vx_bg_ops_t *ops, void *db_ctx);
void        vx_compactor_stop(vx_compactor_t *c);
void        vx_compactor_kick(vx_compactor_t *c);
vx_status_t vx_compactor_run_once(vx_compactor_t *c, bool *out_did_work);
vx_status_t vx_compact_catchup_deletes(const vx_compact_victim_t *victim,
                                       const vx_delmap_t *const *curr_delmaps,
                                       uint32_t new_nrows, uint32_t new_nchunks,
                                       vx_delmap_t **new_delmaps, uint32_t *io_new_live);
