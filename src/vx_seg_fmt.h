#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include "vx_owner.h"
#include "vx_chunk.h"

#define VX_SEG_MAGIC     "VXSEG02\0"
#define VX_SEG_VERSION   2u
#define VX_SEG_FLAG_PK   (1u << 0) /* segment embeds Bloom + Robin Hood PK index */
#define VX_PK_SLOT_EMPTY UINT32_MAX

static inline uint64_t vx_align64(uint64_t v) {
    return (v + 63ULL) & ~63ULL;
}

/* 0x0000: 64-byte segment header */
typedef struct vx_seg_hdr {
    uint8_t  magic[8];       /* "VXSEG02\0" */
    uint32_t version;        /* VX_SEG_VERSION (2) */
    uint32_t flags;          /* VX_SEG_FLAG_* */
    uint32_t table_id;
    uint32_t ncols;
    uint64_t segment_id;
    uint64_t nrows;          /* <= UINT32_MAX - 1 */
    uint64_t lsn_max;        /* highest WAL LSN materialized in this segment */
    uint64_t min_pk;         /* valid iff flags & VX_SEG_FLAG_PK */
    uint64_t max_pk;
} vx_seg_hdr_t;
_Static_assert(sizeof(vx_seg_hdr_t) == 64, "vx_seg_hdr_t must be 64 bytes");

/* 96-byte per-column descriptor in metadata block */
typedef struct vx_seg_col_desc {
    uint32_t  col_id;
    uint8_t   type;           /* vx_type_t */
    uint8_t   encoding;       /* vx_encoding_t (0 = VX_ENC_PLAIN) */
    uint8_t   has_nulls;      /* 0 => validity_off == 0 */
    uint8_t   _pad0;
    uint64_t  data_off;       /* 64B-aligned file offset */
    uint64_t  data_len;
    uint64_t  validity_off;   /* 64B-aligned file offset (0 if !has_nulls) */
    uint64_t  validity_len;
    uint64_t  chunk_meta_off; /* file offset of vx_seg_chunk_meta_t[nchunks] */
    uint64_t  dict_off;       /* M15 dictionary offset (0 if plain) */
    uint64_t  dict_len;
    uint32_t  dict_count;     /* M15 dictionary cardinality */
    uint32_t  data_crc32;     /* IEEE CRC32 of data + validity + dict pages */
    vx_zone_t seg_zone;       /* 24B segment-level min/max/null_count */
} vx_seg_col_desc_t;
_Static_assert(sizeof(vx_seg_col_desc_t) == 96, "vx_seg_col_desc_t must be 96 bytes");

/* 40-byte per-4,096-row chunk metadata (zone map + M15 variable page bounds) */
typedef struct vx_seg_chunk_meta {
    vx_zone_t zone;           /* 24B chunk-level min/max/null_count */
    uint64_t  page_off;       /* byte offset relative to col_desc.data_off */
    uint32_t  page_len;       /* byte length of chunk's data slice */
    uint32_t  page_crc32;     /* per-chunk scrub checksum */
} vx_seg_chunk_meta_t;
_Static_assert(sizeof(vx_seg_chunk_meta_t) == 40, "vx_seg_chunk_meta_t must be 40 bytes");

/* 16-byte Arrow StringView variadic buffer locator */
typedef struct vx_seg_strbuf_desc {
    uint64_t file_off;        /* 64B-aligned file offset */
    uint64_t byte_len;        /* <= INT32_MAX so vx_strview_t.out.offset stays int32_t */
} vx_seg_strbuf_desc_t;
_Static_assert(sizeof(vx_seg_strbuf_desc_t) == 16, "vx_seg_strbuf_desc_t must be 16 bytes");

/* 32-byte per-segment PK index header (inside metadata block) */
typedef struct vx_seg_pk_hdr {
    uint64_t bloom_off;       /* 64B-aligned file offset of split-block Bloom filter */
    uint32_t bloom_nblocks;   /* power of 2; each block is 32B (8 x uint32_t) */
    uint32_t bloom_seed;
    uint64_t rh_off;          /* 64B-aligned file offset of vx_seg_pk_slot_t[rh_cap] */
    uint32_t rh_cap;          /* power of 2 (>= ceil(nrows / 0.7)) */
    uint32_t rh_max_psl;      /* hard upper bound on probe distance */
} vx_seg_pk_hdr_t;
_Static_assert(sizeof(vx_seg_pk_hdr_t) == 32, "vx_seg_pk_hdr_t must be 32 bytes");

/* 16-byte immutable Robin Hood slot: row_idx == UINT32_MAX marks empty,
 * supporting ALL 64-bit keys including 0 and UINT64_MAX without a separate bitmap. */
typedef struct vx_seg_pk_slot {
    uint64_t pk;
    uint32_t row_idx;         /* [0 .. nrows-1] or VX_PK_SLOT_EMPTY */
    uint32_t psl;             /* probe sequence length from ideal bucket */
} vx_seg_pk_slot_t;
_Static_assert(sizeof(vx_seg_pk_slot_t) == 16, "vx_seg_pk_slot_t must be 16 bytes");

/* EOF - 64: 64-byte footer */
typedef struct vx_seg_footer {
    uint64_t col_dir_off;
    uint64_t strbuf_dir_off;
    uint64_t pk_hdr_off;      /* 0 if !(flags & VX_SEG_FLAG_PK) */
    uint64_t meta_start_off;  /* start of metadata block covered by meta_crc32 */
    uint64_t file_bytes;      /* exact expected file size */
    uint32_t nchunks;         /* ceil(nrows / VX_BATCH_CAP) */
    uint32_t n_strbufs;
    uint32_t meta_crc32;      /* CRC32(hdr[0..64) || file[meta_start_off .. footer_off+48)) */
    uint32_t file_crc32;      /* CRC32(file[0 .. footer_off+52)) */
    uint8_t  magic[8];        /* "VXSEG02\0" */
} vx_seg_footer_t;
_Static_assert(sizeof(vx_seg_footer_t) == 64, "vx_seg_footer_t must be 64 bytes");

/* Parquet/Putze-style Split-Block Bloom Filter (8 odd 32-bit salts per 256-bit block) */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static const uint32_t VX_BLOOM_SALTS[8] = {
    0x47b6137bu, 0x44974d91u, 0x8824ad5bu, 0xa2b7289du,
    0x705495c7u, 0x2df1424bu, 0x9efc4947u, 0x5c6bfb31u
};

static inline uint64_t vx_pk_hash64(uint64_t pk, uint32_t seed) {
    uint64_t x = pk ^ ((uint64_t)seed * 0x9e3779b97f4a7c15ULL);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline void vx_bloom_insert(uint32_t *blocks, uint32_t nblocks_mask,
                                   uint32_t seed, uint64_t pk) {
    uint64_t  h   = vx_pk_hash64(pk, seed);
    uint32_t *blk = blocks + (((uint32_t)(h >> 32) & nblocks_mask) << 3);
    uint32_t  k   = (uint32_t)h;
    for (uint32_t i = 0; i < 8u; ++i) {
        uint32_t bit = (k * VX_BLOOM_SALTS[i]) >> 27; /* top 5 bits -> 0..31 */
        blk[i] |= (1u << bit);
    }
}

static inline bool vx_bloom_may_contain(const uint32_t *blocks, uint32_t nblocks_mask,
                                        uint32_t seed, uint64_t pk) {
    uint64_t        h   = vx_pk_hash64(pk, seed);
    const uint32_t *blk = blocks + (((uint32_t)(h >> 32) & nblocks_mask) << 3);
    uint32_t        k   = (uint32_t)h;
    uint32_t        ok  = 1u;
    for (uint32_t i = 0; i < 8u; ++i) {
        uint32_t bit = (k * VX_BLOOM_SALTS[i]) >> 27;
        ok &= (blk[i] >> bit) & 1u;
    }
    return ok != 0u;
}

/* Runtime mmap segment object (embedded vx_owner_t pins munmap + optional unlink) */
typedef struct vx_seg {
    vx_owner_t                 hdr;              /* VX_OWN_SEGMENT */
    int                        fd;
    const uint8_t             *mmap_base;
    size_t                     mmap_len;
    char                      *file_path;        /* used for deferred unlink() on retirement */
    _Atomic bool               unlink_on_destroy;/* set true by compaction before EBR retire */

    /* Direct pointers into mmap_base (zero heap deserialization) */
    const vx_seg_hdr_t        *file_hdr;
    const vx_seg_footer_t     *footer;
    const vx_seg_col_desc_t   *col_dir;
    const vx_seg_pk_hdr_t     *pk_hdr;
    const uint32_t            *bloom_blocks;
    const vx_seg_pk_slot_t    *pk_slots;

    /* Shared Arrow StringView pool pointing directly into mmap_base */
    vx_strpool_t               strpool;          /* owners=NULL; chunks retain &seg->hdr */

    /* Lightweight chunk view array for M12 VX_SOURCE_TABLE_SCAN */
    vx_chunk_t               **chunk_views;      /* [nchunks], backing = &seg->hdr */
    uint32_t                   nchunks;
} vx_seg_t;

/* O(1) Cold Segment PK Probe: zone map -> Bloom -> immutable Robin Hood -> delmap check */
static inline bool vx_seg_pk_lookup(const vx_seg_t *seg, const vx_delmap_t *const *chunk_delmaps,
                                    uint64_t pk, uint32_t *out_row_idx) {
    const vx_seg_hdr_t *h = seg->file_hdr;
    if (!(h->flags & VX_SEG_FLAG_PK) || h->nrows == 0) return false;
    if (pk < h->min_pk || pk > h->max_pk) return false;

    const vx_seg_pk_hdr_t *pkh = seg->pk_hdr;
    if (!vx_bloom_may_contain(seg->bloom_blocks, pkh->bloom_nblocks - 1u, pkh->bloom_seed, pk)) {
        return false;
    }

    const uint32_t mask = pkh->rh_cap - 1u;
    uint32_t       pos  = (uint32_t)vx_pk_hash64(pk, pkh->bloom_seed) & mask;

    for (uint32_t psl = 0; psl <= pkh->rh_max_psl; ++psl) {
        const vx_seg_pk_slot_t *s = &seg->pk_slots[pos];
        if (s->row_idx == VX_PK_SLOT_EMPTY || psl > s->psl) {
            return false; /* Robin Hood early termination */
        }
        if (s->pk == pk) {
            uint32_t r = s->row_idx;
            const vx_delmap_t *dm = chunk_delmaps ? chunk_delmaps[r >> 12] : NULL;
            if (dm && (((dm->bits[(r & 4095u) >> 6] >> (r & 63u)) & 1ULL) != 0)) {
                return false; /* tombstoned in active .del generation */
            }
            *out_row_idx = r;
            return true;
        }
        pos = (pos + 1u) & mask;
    }
    return false;
}

