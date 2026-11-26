#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "vx_owner.h"

#define VX_IDX_MAGIC        "VXIDX02\0"
#define VX_IDX_VERSION      2u
#define VX_ART_EMPTY_REF    0ULL
#define VX_ART_LEAF_TAG     (1ULL << 63)
#define VX_ART_REF_MASK     (~VX_ART_LEAF_TAG)
#define VX_ART_MAX_PREFIX   12u
#define VX_ART_N48_EMPTY    0xFFu

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_idx_file_kind : uint8_t {
    VX_IDX_KIND_HASH = 1, /* Immutable Robin Hood hash index (equality / unique) */
    VX_IDX_KIND_ART  = 2  /* Immutable Adaptive Radix Tree (ordered range + equality) */
} vx_idx_file_kind_t;

/* 64-byte companion .idx file header (seg_<seg_id>_i<idx_id>.idx) */
typedef struct vx_idx_hdr {
    uint8_t  magic[8];      /* "VXIDX02\0" */
    uint32_t version;       /* VX_IDX_VERSION (2) */
    uint8_t  kind;          /* vx_idx_file_kind_t */
    uint8_t  is_unique;     /* 1 = unique(...), 0 = index(...) */
    uint16_t ncols;         /* number of indexed columns (1 = single, >1 = composite) */
    uint32_t table_id;
    uint32_t index_id;
    uint64_t segment_id;
    uint64_t nrows_indexed; /* non-null rows indexed in this segment */
    uint64_t root_off;      /* ART root ref (tagged) OR Hash table slot array offset */
    uint64_t file_bytes;
    uint32_t file_crc32;    /* CRC32(hdr[0..56) || file[64..file_bytes)) */
    uint32_t aux_param;     /* Hash: rh_cap (pow2) | ART: max_depth */
} vx_idx_hdr_t;
_Static_assert(sizeof(vx_idx_hdr_t) == 64, "vx_idx_hdr_t must be 64 bytes");

/* --- Disk-Mappable ART Node Types (all 8-byte aligned inside .idx mmap) --- */
#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_art_node_kind : uint8_t {
    VX_ART_NODE4   = 1,
    VX_ART_NODE16  = 2,
    VX_ART_NODE48  = 3,
    VX_ART_NODE256 = 4
} vx_art_node_kind_t;

/* 16-byte common header with inline optimistic path compression (up to 12 bytes) */
typedef struct vx_art_node_hdr {
    uint8_t  kind;                     /* vx_art_node_kind_t */
    uint8_t  count;                    /* active children (0..255; Node256 uses 0 => 256) */
    uint16_t prefix_len;               /* total compressed prefix bytes (>12 => check leaf) */
    uint8_t  prefix[VX_ART_MAX_PREFIX];
} vx_art_node_hdr_t;
_Static_assert(sizeof(vx_art_node_hdr_t) == 16, "vx_art_node_hdr_t must be 16 bytes");

/* Node4: sorted keys[0..count-1], parallel children[0..count-1] */
typedef struct vx_art_node4 {
    vx_art_node_hdr_t hdr;
    uint8_t           keys[4];
    uint8_t           _pad[4];
    uint64_t          children[4];
} vx_art_node4_t;
_Static_assert(sizeof(vx_art_node4_t) == 56, "vx_art_node4_t must be 56 bytes");

/* Node16: sorted keys[0..count-1] (SIMD 16-byte compare), parallel children[0..count-1] */
typedef struct vx_art_node16 {
    vx_art_node_hdr_t hdr;
    uint8_t           keys[16];
    uint64_t          children[16];
} vx_art_node16_t;
_Static_assert(sizeof(vx_art_node16_t) == 160, "vx_art_node16_t must be 160 bytes");

/* Node48: direct byte-indexed lookup child_idx[byte] -> children[0..47] */
typedef struct vx_art_node48 {
    vx_art_node_hdr_t hdr;
    uint8_t           child_idx[256]; /* VX_ART_N48_EMPTY (0xFF) = absent */
    uint64_t          children[48];
} vx_art_node48_t;
_Static_assert(sizeof(vx_art_node48_t) == 656, "vx_art_node48_t must be 656 bytes");

/* Node256: O(1) direct byte dispatch */
typedef struct vx_art_node256 {
    vx_art_node_hdr_t hdr;
    uint64_t          children[256];
} vx_art_node256_t;
_Static_assert(sizeof(vx_art_node256_t) == 2064, "vx_art_node256_t must be 2064 bytes");

/* Variable-length ART Leaf (8-byte aligned):
 *   [0..16)                  vx_art_leaf_t header
 *   [16 .. 16 + key_len)     order-preserving binary key bytes (padded to 8B)
 * If nrows == 1: row_or_post_off stores the single row_idx inline (0 extra seeks).
 * If nrows >  1: row_or_post_off is the file offset of sorted uint32_t row_ids[nrows]. */
typedef struct vx_art_leaf {
    uint32_t key_len;
    uint32_t nrows;
    uint64_t row_or_post_off;
} vx_art_leaf_t;
_Static_assert(sizeof(vx_art_leaf_t) == 16, "vx_art_leaf_t must be 16 bytes");

/* --- Order-Preserving Binary Key Encoders (big-endian unsigned byte order) --- */
static inline void vx_key_encode_u64(uint64_t v, uint8_t out[8]) {
    uint64_t be = __builtin_bswap64(v);
    memcpy(out, &be, 8u);
}

static inline void vx_key_encode_i64(int64_t v, uint8_t out[8]) {
    uint64_t flipped = (uint64_t)v ^ (1ULL << 63);
    vx_key_encode_u64(flipped, out);
}

static inline void vx_key_encode_f64(double d, uint8_t out[8]) {
    if (d == 0.0) d = +0.0; /* normalize -0.0 -> +0.0 */
    uint64_t u = 0;
    memcpy(&u, &d, 8u);
    /* Negative floats flip all bits; positive floats flip sign bit only */
    uint64_t mask = (u & (1ULL << 63)) ? ~0ULL : (1ULL << 63);
    vx_key_encode_u64(u ^ mask, out);
}

/* Escapes 0x00 -> 0x00 0x01 and terminates with 0x00 0x00 for prefix-free composite keys. */
static inline size_t vx_key_encode_str(const uint8_t *s, int32_t len, uint8_t *out) {
    size_t pos = 0;
    for (int32_t i = 0; i < len; ++i) {
        if (s[i] == 0x00u) { out[pos++] = 0x00u; out[pos++] = 0x01u; }
        else               { out[pos++] = s[i]; }
    }
    out[pos++] = 0x00u;
    out[pos++] = 0x00u;
    return pos;
}

