#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vx_batch.h"

/* --- On-Disk Chunk Page Headers (embedded inside .vx column data pages) --- */

/* VX_ENC_FOR_BP: 16B header + ceil(nrows * bit_width / 64) uint64_t words */
typedef struct vx_for_hdr {
    uint64_t base;      /* minimum valid value as raw u64 bits */
    uint32_t nrows;
    uint8_t  bit_width; /* 0..64 */
    uint8_t  _pad[3];
} vx_for_hdr_t;
_Static_assert(sizeof(vx_for_hdr_t) == 16, "vx_for_hdr_t must be 16 bytes");

/* VX_ENC_DICT: 8B header + ceil(nrows * bit_width / 64) uint64_t words.
 * Segment-level dictionary (vx_strview_t[dict_count]) lives at col_desc.dict_off. */
typedef struct vx_dict_hdr {
    uint32_t nrows;
    uint8_t  bit_width; /* 0..32 */
    uint8_t  _pad[3];
} vx_dict_hdr_t;
_Static_assert(sizeof(vx_dict_hdr_t) == 8, "vx_dict_hdr_t must be 8 bytes");

/* VX_ENC_RLE: 16B header + uint32_t run_lens[nruns] (padded to 8B) + typed vals[nruns] */
typedef struct vx_rle_hdr {
    uint32_t nrows;
    uint32_t nruns;
    uint8_t  type;      /* vx_type_t */
    uint8_t  _pad[7];
} vx_rle_hdr_t;
_Static_assert(sizeof(vx_rle_hdr_t) == 16, "vx_rle_hdr_t must be 16 bytes");

/* --- Segment-Level String Dictionary Builder (used by vx_seg_write) --- */
typedef struct vx_dict_builder {
    vx_strview_t *vals;     /* unique dictionary entries [count] */
    uint64_t     *hashes;   /* [count] */
    uint32_t     *buckets;  /* open-addressing table: 0 = empty, else code + 1 */
    uint32_t      cap_bkt;  /* power of 2 */
    uint32_t      count;
    uint32_t      cap_vals;
    uint32_t      max_dict; /* cardinality ceiling (e.g. 4096) before falling back to PLAIN */
} vx_dict_builder_t;

vx_status_t vx_dict_builder_init(vx_dict_builder_t *db, uint32_t max_dict);
void        vx_dict_builder_destroy(vx_dict_builder_t *db);
/* Looks up or inserts `sv` (backed by `pool`). Returns VX_ERR_INVALID if cardinality > max_dict. */
vx_status_t vx_dict_builder_intern(vx_dict_builder_t *db, vx_strview_t sv,
                                   const vx_strpool_t *pool, uint32_t *out_code);

/* --- Chunk Encoders (write into caller buffer; set *out_len) --- */
vx_status_t vx_enc_for_bp_encode(const void *src_vals, vx_type_t type,
                                 const uint64_t *validity, uint32_t nrows,
                                 uint8_t *dst, size_t dst_cap, size_t *out_len);

vx_status_t vx_enc_dict_encode(const uint32_t *codes, uint32_t nrows, uint32_t dict_count,
                               uint8_t *dst, size_t dst_cap, size_t *out_len);

vx_status_t vx_enc_rle_encode(const void *src_vals, vx_type_t type,
                              const uint64_t *validity, const vx_strpool_t *strpool,
                              uint32_t nrows, uint8_t *dst, size_t dst_cap, size_t *out_len);

/* --- Unified Decoder Hook for M12 vx_bind_col_chunk() --- */
typedef struct vx_enc_col_ctx {
    const void         *page_data;  /* mmap pointer to chunk's encoded page */
    size_t              page_len;
    const uint64_t     *validity;   /* NULL if !has_nulls */
    const void         *dict_vals;  /* mmap pointer to vx_strview_t[dict_count] (VX_ENC_DICT) */
    uint32_t            dict_count;
    const vx_strpool_t *strpool;    /* segment strpool (VX_TYPE_STR) */
    vx_type_t           type;
    vx_encoding_t       encoding;
    uint8_t             has_nulls;
    uint32_t            nrows;
} vx_enc_col_ctx_t;

vx_status_t vx_enc_decode_col_chunk(const vx_enc_col_ctx_t *ec, vx_arena_t *scratch,
                                    vx_vec_t *out_vec);

