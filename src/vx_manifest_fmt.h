#pragma once
#include <stdint.h>

#define VX_DEL_MAGIC     "VXDEL02\0"
#define VX_MAN_MAGIC     "VXMAN02\0"
#define VX_FMT_VERSION   2u

/* --- Versioned .del Tombstone Sidecar (seg_<id>_g<gen>.del) ---
 * Layout:
 *   [0..64)               vx_del_hdr_t
 *   [64 .. bits_off)      uint32_t chunk_live[nchunks] (zero-padded to 64B boundary)
 *   [bits_off .. eof)     uint64_t del_bits[nchunks * 64] (1 = deleted, 512B/chunk) */
typedef struct vx_del_hdr {
    uint8_t  magic[8];       /* "VXDEL02\0" */
    uint32_t version;        /* VX_FMT_VERSION (2) */
    uint32_t table_id;
    uint64_t segment_id;
    uint64_t generation;     /* monotonic >= 1 */
    uint32_t nrows;
    uint32_t live_rows;
    uint32_t nchunks;
    uint32_t bits_off;       /* vx_align64(64 + nchunks * sizeof(uint32_t)) */
    uint64_t file_bytes;     /* bits_off + nchunks * 512ULL */
    uint32_t file_crc32;     /* CRC32(hdr[0..56) || file[64..file_bytes)) */
    uint32_t _pad;
} vx_del_hdr_t;
_Static_assert(sizeof(vx_del_hdr_t) == 64, "vx_del_hdr_t must be 64 bytes");

/* --- Atomic MANIFEST File ---
 * Layout:
 *   [0..64)               vx_man_hdr_t
 *   [tables section]      ntables x (vx_man_table_t + name + cols + indexes)
 *   [segments section]    vx_man_seg_t[nsegments] (64B each, sorted by table_id, level, seg_id) */
typedef struct vx_man_hdr {
    uint8_t  magic[8];       /* "VXMAN02\0" */
    uint32_t version;        /* VX_FMT_VERSION (2) */
    uint32_t flags;
    uint64_t manifest_gen;   /* monotonic commit generation */
    uint64_t checkpoint_lsn; /* WAL LSN <= checkpoint_lsn is fully durable in .vx/.del */
    uint64_t next_seg_id;    /* monotonic segment ID counter (never reused across restarts) */
    uint32_t ntables;
    uint32_t nsegments;
    uint64_t file_bytes;
    uint32_t file_crc32;     /* CRC32(hdr[0..56) || file[64..file_bytes)) */
    uint32_t _pad;
} vx_man_hdr_t;
_Static_assert(sizeof(vx_man_hdr_t) == 64, "vx_man_hdr_t must be 64 bytes");

typedef struct vx_man_table {
    uint32_t table_id;
    uint32_t name_len;       /* UTF-8 bytes immediately following this struct (padded to 8B) */
    uint32_t ncols;
    uint32_t pk_col_idx;     /* UINT32_MAX if no explicit PK */
    uint32_t n_indexes;      /* unique(...) + index(...) metadata descriptors */
    uint32_t _pad;
} vx_man_table_t;
_Static_assert(sizeof(vx_man_table_t) == 24, "vx_man_table_t must be 24 bytes");

typedef struct vx_man_col {
    uint32_t col_id;
    uint32_t name_len;       /* UTF-8 bytes immediately following (padded to 8B) */
    uint8_t  type;           /* vx_type_t */
    uint8_t  nullable;
    uint8_t  _pad[6];
} vx_man_col_t;
_Static_assert(sizeof(vx_man_col_t) == 16, "vx_man_col_t must be 16 bytes");

typedef struct vx_man_seg {
    uint64_t segment_id;
    uint64_t del_gen;        /* 0 => no .del sidecar (100% live); >=1 => seg_<id>_g<gen>.del */
    uint64_t lsn_max;        /* max WAL LSN inside this segment */
    uint64_t file_bytes;     /* expected .vx byte length */
    uint64_t min_pk;
    uint64_t max_pk;
    uint32_t table_id;
    uint32_t nrows;
    uint32_t live_rows;
    uint16_t level;          /* 0 = freshly flushed L0; 1..N = compacted levels */
    uint16_t flags;          /* VX_SEG_FLAG_* */
} vx_man_seg_t;
_Static_assert(sizeof(vx_man_seg_t) == 64, "vx_man_seg_t must be 64 bytes");

