#pragma once
#include "vx_seg_fmt.h"

typedef struct vx_seg_write_cfg {
    uint32_t                   table_id;
    uint64_t                   segment_id;
    uint64_t                   lsn_max;
    uint32_t                   ncols;
    uint32_t                   pk_col_idx;  /* UINT32_MAX if no u64 primary key */
    const vx_type_t           *col_types;
    const vx_chunk_t   *const *chunks;      /* sealed hot chunks or compaction sources */
    const vx_delmap_t  *const *delmaps;     /* optional [nchunks]; NULL entries => all live */
    uint32_t                   nchunks;
    const char                *spill_dir;   /* NULL => "/tmp" (for string spool) */
} vx_seg_write_cfg_t;

typedef struct vx_seg_write_info {
    uint64_t nrows;
    uint64_t file_bytes;
    uint64_t min_pk;
    uint64_t max_pk;
    uint32_t flags;
    uint32_t nchunks;
} vx_seg_write_info_t;

/* Streams live rows to <final_path>.tmp -> fdatasync -> rename(final_path).
 * Caller (vx_manifest.c) is responsible for fsync(dir_fd) before publishing MANIFEST. */
vx_status_t vx_seg_write(const char *final_path, const vx_seg_write_cfg_t *cfg,
                         vx_seg_write_info_t *out_info);

/* Zero-deserialization mmap open: verifies header, footer, bounds, and meta_crc32 only. */
vx_status_t vx_seg_open(const char *path, vx_seg_t **out_seg);

/* Full-file CRC32 scrub (for background integrity checks or explicit verify). */
vx_status_t vx_seg_verify_full_crc(const vx_seg_t *seg);

/* IEEE 802.3 CRC32 calculation helper */
uint32_t vx_crc32_update(uint32_t crc, const void *buf, size_t len);

/* Marks retired segment for unlink() once its final vx_owner_release() hits refcnt == 0. */
static inline void vx_seg_mark_unlink(vx_seg_t *seg) {
    if (seg) atomic_store_explicit(&seg->unlink_on_destroy, true, memory_order_release);
}
