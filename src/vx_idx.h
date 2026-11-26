#pragma once
#include "vx_art_fmt.h"
#include "vx_manifest.h"
#include "vx_seg.h"

/* Runtime mmap'd companion .idx object (embedded vx_owner_t pins munmap + deferred unlink) */
typedef struct vx_idx_file {
    vx_owner_t          hdr;               /* VX_OWN_SEGMENT or VX_OWN_INDEX */
    int                 fd;
    const uint8_t      *mmap_base;
    size_t              mmap_len;
    char               *file_path;
    _Atomic bool        unlink_on_destroy;
    const vx_idx_hdr_t *file_hdr;
} vx_idx_file_t;

/* Range scan bounds (encoded via vx_key_encode_*) */
typedef struct vx_art_bound {
    const uint8_t *key;
    uint32_t       len;
    bool           inclusive;
    bool           unbounded;
} vx_art_bound_t;

/* Callback invoked for every live matching row_idx in a cold segment */
typedef vx_status_t (*vx_idx_hit_fn)(void *ctx, uint32_t row_idx, bool *out_stop);

/* Entry for ART index creation */
typedef struct vx_art_entry {
    const uint8_t *key;
    uint32_t       key_len;
    uint32_t       row_idx;
} vx_art_entry_t;

/* Serializes an immutable ART .idx file atomically (<final_path>.tmp -> fdatasync -> rename). */
vx_status_t vx_art_build_file(const char *final_path, uint32_t table_id, uint32_t index_id,
                              uint64_t segment_id, uint16_t ncols, bool is_unique,
                              vx_art_entry_t *entries, uint32_t n_entries);

/* Open an existing ART .idx file (verifies header, CRC32, mmaps read-only) */
vx_status_t vx_art_open(const char *path, vx_idx_file_t **out_idx);

/* Open an existing .idx file (verifies header, CRC32, mmaps read-only, handles ART and HASH) */
vx_status_t vx_idx_open(const char *path, vx_idx_file_t **out_idx);

/* Close / release index file */
static inline void vx_idx_close(vx_idx_file_t *idx) {
    if (idx) vx_owner_release(&idx->hdr);
}

/* Point equality lookup on ART */
vx_status_t vx_art_lookup_eq(const vx_idx_file_t *idx, const vx_delmap_t *const *chunk_delmaps,
                             const uint8_t *enc_key, uint32_t key_len,
                             vx_idx_hit_fn on_hit, void *ctx);

/* Point equality lookup (works on both VX_IDX_KIND_HASH and VX_IDX_KIND_ART) */
vx_status_t vx_idx_lookup_eq(const vx_idx_file_t *idx, const vx_delmap_t *const *chunk_delmaps,
                             const uint8_t *enc_key, uint32_t key_len,
                             vx_idx_hit_fn on_hit, void *ctx);

/* Ordered range scan on VX_IDX_KIND_ART (emits live row_idx hits in key order) */
vx_status_t vx_art_scan_range(const vx_idx_file_t *idx, const vx_delmap_t *const *chunk_delmaps,
                              vx_art_bound_t lo, vx_art_bound_t hi,
                              vx_idx_hit_fn on_hit, void *ctx);

/* Builders used by vx_flusher.c, vx_compact.c, and CREATE INDEX */
vx_status_t vx_idx_build_from_seg(const char *dir_path, const vx_seg_t *seg,
                                  const vx_man_index_def_t *idx_def,
                                  vx_idx_file_t **out_idx);
