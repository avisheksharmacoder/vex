#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "vx_manifest_fmt.h"
#include "vx_seg.h"

#define VX_IDX_FLAG_UNIQUE (1u << 0) /* unique(...) constraint */
#define VX_IDX_FLAG_ART    (1u << 1) /* M16 ordered ART range index */
#define VX_IDX_FLAG_HASH   (1u << 2) /* M16 equality/unique Robin Hood index */

/* Wire header for persisted index/unique metadata inside MANIFEST tables section.
 * Followed on disk by:
 *   - name bytes (name_len, zero-padded to 8B)
 *   - uint32_t col_ids[ncols] (zero-padded to 8B) */
typedef struct vx_man_index {
    uint32_t index_id;
    uint32_t name_len;
    uint32_t ncols;
    uint32_t flags; /* VX_IDX_FLAG_* */
} vx_man_index_t;
_Static_assert(sizeof(vx_man_index_t) == 16, "vx_man_index_t must be 16 bytes");

/* --- In-Memory Manifest Model --- */
typedef struct vx_man_col_def {
    uint32_t  col_id;
    char     *name;
    vx_type_t type;
    uint8_t   nullable;
} vx_man_col_def_t;

typedef struct vx_man_index_def {
    uint32_t  index_id;
    char     *name;
    uint32_t *col_ids;
    uint32_t  ncols;
    uint32_t  flags;
} vx_man_index_def_t;

typedef struct vx_man_table_def {
    uint32_t            table_id;
    char               *name;
    uint32_t            pk_col_idx; /* UINT32_MAX if none */
    vx_man_col_def_t   *cols;
    uint32_t            ncols;
    vx_man_index_def_t *indexes;
    uint32_t            n_indexes;
} vx_man_table_def_t;

typedef struct vx_manifest {
    uint64_t            manifest_gen;
    uint64_t            checkpoint_lsn;
    uint64_t            next_seg_id;
    uint32_t            flags;
    vx_man_table_def_t *tables;
    uint32_t            ntables;
    vx_man_seg_t       *segments;
    uint32_t            nsegments;
} vx_manifest_t;

/* --- Mounted Cold Segment + Sparse Per-Chunk Delmap Overlay --- */
typedef struct vx_mounted_seg {
    vx_man_seg_t  meta;
    vx_seg_t     *seg;            /* refcounted mmap segment */
    vx_delmap_t **chunk_delmaps;  /* [seg->nchunks]; NULL entries => 100% live chunk */
    uint32_t      nchunks;
    uint64_t      dirty_lsn;      /* 0 = clean; >0 = WAL-replayed/live deletes pending flush */
} vx_mounted_seg_t;

/* --- WAL Replay Hook (implemented in vx_wal.c / vx_db.c) --- */
typedef struct vx_wal_frame {
    uint64_t       lsn;
    uint32_t       op_type;       /* VX_WAL_OP_QUERY / PUT_ROW / QUERY_PARAMS / TX_BATCH */
    const uint8_t *payload;
    uint32_t       payload_len;
} vx_wal_frame_t;

/* Reads next valid CRC32 frame from wal_fd starting at *io_off.
 * Returns:
 *   VX_OK          + *out_eof = false  -> valid frame decoded, *io_off advanced
 *   VX_OK          + *out_eof = true   -> clean EOF reached
 *   VX_ERR_CORRUPT                     -> torn tail frame at *io_off (triggers auto-heal)
 *   VX_ERR_IO / VX_ERR_OOM             -> hard failure */
typedef vx_status_t (*vx_wal_next_fn)(void *wal_ctx, int wal_fd, uint64_t *io_off,
                                      vx_wal_frame_t *out_frame, bool *out_eof);

/* Applies a single WAL frame with lsn > manifest.checkpoint_lsn into Hot RAM / cold overlays. */
typedef vx_status_t (*vx_wal_apply_fn)(void *apply_ctx, const vx_wal_frame_t *frame);

typedef struct vx_recovery_state {
    int               wal_fd;       /* exclusively flocked app.wal fd (kept open by db) */
    uint64_t          wal_valid_len;
    uint64_t          max_lsn;      /* max(checkpoint_lsn, last replayed WAL LSN) */
    uint32_t          orphans_swept;
    uint64_t          wal_skipped;
    uint64_t          wal_replayed;
    bool              wal_healed_tail;
    vx_manifest_t     manifest;
    vx_mounted_seg_t *mounted;      /* [nmounted], 1:1 with manifest.segments */
    uint32_t          nmounted;
} vx_recovery_state_t;

/* --- Path & Durability Helpers --- */
vx_status_t vx_fsync_dir(const char *dir_path);
vx_status_t vx_seg_format_path(const char *dir_path, uint64_t segment_id,
                               char *out_buf, size_t buf_sz);
vx_status_t vx_del_format_path(const char *dir_path, uint64_t segment_id, uint64_t del_gen,
                               char *out_buf, size_t buf_sz);

/* --- Versioned .del Sidecar Writer & Loader --- */
vx_status_t vx_del_write(const char *dir_path, uint32_t table_id, uint64_t segment_id,
                         uint64_t generation, uint32_t nrows, uint32_t nchunks,
                         const vx_delmap_t *const *chunk_delmaps, uint32_t *out_live_rows);

vx_status_t vx_del_load(const char *dir_path, uint32_t expected_table_id,
                        uint64_t expected_seg_id, uint64_t expected_gen,
                        uint32_t expected_nrows, uint32_t expected_nchunks,
                        vx_delmap_t ***out_chunk_delmaps, uint32_t *out_live_rows);

/* --- Atomic MANIFEST Lifecycle --- */
void        vx_manifest_free(vx_manifest_t *man);
vx_status_t vx_manifest_publish(const char *dir_path, vx_manifest_t *man);
vx_status_t vx_manifest_load(const char *dir_path, vx_manifest_t *out_man, bool *out_existed);
vx_status_t vx_manifest_sweep_orphans(const char *dir_path, const vx_manifest_t *man,
                                      uint32_t *out_removed);

/* --- Full vex.open() Crash-Recovery Entry Point --- */
vx_status_t vx_recover_open(const char *dir_path, vx_wal_next_fn wal_next, void *wal_ctx,
                            vx_wal_apply_fn wal_apply, void *apply_ctx,
                            vx_recovery_state_t *out_state);
void        vx_recovery_state_destroy(vx_recovery_state_t *st);
