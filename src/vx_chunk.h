#pragma once
#include "vx_owner.h"
#include "vx_str.h"

typedef struct vx_delmap {
    vx_owner_t hdr;
    uint64_t   generation;                /* monotonic per chunk/segment for M13 .del sidecars */
    uint32_t   nrows;
    uint32_t   live_count;
    alignas(64) uint64_t bits[VX_BITMAP_WORDS]; /* 1 = deleted, 0 = live */
} vx_delmap_t;

typedef struct vx_col_chunk {
    vx_type_t      type;
    vx_encoding_t  encoding;
    uint8_t        has_nulls;             /* fast skip when 0 */
    uint8_t        _pad[5];
    const void    *data;                  /* 64B-aligned typed array or encoded payload */
    const uint64_t *validity;             /* 64B-aligned, 1 = valid, 0 = null (Arrow-native) */
    vx_strpool_t  *strpool;               /* non-NULL iff type == VX_TYPE_STR */
    vx_zone_t      zone;
} vx_col_chunk_t;

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_tier : uint8_t {
    VX_TIER_HOT_RAM   = 0,
    VX_TIER_COLD_MMAP = 1
} vx_tier_t;

typedef struct vx_chunk {
    vx_owner_t      hdr;
    uint64_t        chunk_id;             /* unique within table */
    uint64_t        row_id_base;          /* logical row base for PK/secondary indexes */
    uint32_t        nrows;                /* <= VX_BATCH_CAP for hot chunks */
    uint32_t        ncols;
    vx_tier_t       tier;
    uint8_t         _pad[7];
    vx_owner_t     *backing;              /* NULL for hot self-owned; &seg->hdr for cold mmap */
    vx_col_chunk_t *cols;                 /* array of length ncols */
} vx_chunk_t;

/* COW delmap helpers */
vx_delmap_t *vx_delmap_create(uint32_t nrows);
vx_delmap_t *vx_delmap_clone_and_set(const vx_delmap_t *src, uint32_t row_idx);
uint32_t     vx_delmap_live_sel(const vx_delmap_t *dm, uint32_t nrows, uint32_t *out_sel);

static inline bool vx_del_is_deleted(const vx_delmap_t *dm, uint32_t row_idx) {
    if (!dm) return false;
    uint32_t w = row_idx >> 6;
    if (w >= VX_BITMAP_WORDS) return false;
    return (dm->bits[w] & (1ULL << (row_idx & 63u))) != 0ULL;
}

