#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "vx_enc.h"

/* ============================================================================
 * 1. Bit-Width & Bitpacking Primitives (Zero-UB across 0..64 bits)
 * ============================================================================ */
static inline uint8_t vx_bits_needed_u64(uint64_t max_val) {
    if (max_val == 0ULL) return 0u;
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse64(&idx, max_val);
    return (uint8_t)(idx + 1u);
#else
    return (uint8_t)(64u - (uint32_t)__builtin_clzll(max_val));
#endif
}

static inline size_t vx_packed_words(uint32_t nrows, uint8_t bw) {
    if (bw == 0 || nrows == 0) return 0u;
    uint64_t bits = (uint64_t)nrows * (uint64_t)bw;
    return (size_t)((bits + 63ULL) >> 6);
}

static inline bool vx_enc_valid_bit(const uint64_t *val, uint32_t r) {
    return !val || (((val[r >> 6] >> (r & 63u)) & 1ULL) != 0);
}

static inline size_t vx_enc_elem_size(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return sizeof(uint8_t);
    case VX_TYPE_I64:  return sizeof(int64_t);
    case VX_TYPE_U64:  return sizeof(uint64_t);
    case VX_TYPE_F64:  return sizeof(double);
    case VX_TYPE_STR:  return sizeof(vx_strview_t);
    }
    return 0;
}

static inline uint64_t vx_enc_align8(uint64_t v) {
    return (v + 7ULL) & ~7ULL;
}

static void vx_bitpack_u64(const uint64_t *src, uint32_t n, uint8_t bw, uint64_t *dst_words) {
    if (bw == 0 || n == 0) return;
    if (bw == 64u) {
        memcpy(dst_words, src, (size_t)n * sizeof(uint64_t));
        return;
    }

    const size_t nwords = vx_packed_words(n, bw);
    memset(dst_words, 0, nwords * sizeof(uint64_t));

    const uint64_t mask    = (1ULL << bw) - 1ULL;
    uint64_t       bit_pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v   = src[i] & mask;
        size_t   wi  = (size_t)(bit_pos >> 6);
        uint32_t bo  = (uint32_t)(bit_pos & 63u);
        dst_words[wi] |= (v << bo);
        if (bo + bw > 64u) {
            dst_words[wi + 1u] |= (v >> (64u - bo));
        }
        bit_pos += bw;
    }
}

static void vx_bitunpack_u64_add(const uint64_t *src_words, uint32_t n, uint8_t bw,
                                 uint64_t base, uint64_t *dst) {
    if (n == 0) return;
    if (bw == 0) {
        for (uint32_t i = 0; i < n; ++i) dst[i] = base;
        return;
    }
    if (bw == 64u) {
        for (uint32_t i = 0; i < n; ++i) dst[i] = base + src_words[i];
        return;
    }

    const uint64_t mask    = (1ULL << bw) - 1ULL;
    uint64_t       bit_pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        size_t   wi = (size_t)(bit_pos >> 6);
        uint32_t bo = (uint32_t)(bit_pos & 63u);
        uint64_t v  = src_words[wi] >> bo;
        if (bo + bw > 64u) {
            v |= src_words[wi + 1u] << (64u - bo);
        }
        dst[i]   = base + (v & mask);
        bit_pos += bw;
    }
}

static void vx_bitpack_u32(const uint32_t *src, uint32_t n, uint8_t bw, uint64_t *dst_words) {
    if (bw == 0 || n == 0) return;
    const size_t nwords = vx_packed_words(n, bw);
    memset(dst_words, 0, nwords * sizeof(uint64_t));

    const uint64_t mask    = (bw == 32u) ? 0xFFFFFFFFULL : ((1ULL << bw) - 1ULL);
    uint64_t       bit_pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v  = (uint64_t)src[i] & mask;
        size_t   wi = (size_t)(bit_pos >> 6);
        uint32_t bo = (uint32_t)(bit_pos & 63u);
        dst_words[wi] |= (v << bo);
        if (bo + bw > 64u) {
            dst_words[wi + 1u] |= (v >> (64u - bo));
        }
        bit_pos += bw;
    }
}

static bool vx_bitunpack_u32_checked(const uint64_t *src_words, uint32_t n, uint8_t bw,
                                     uint32_t dict_count, uint32_t *dst) {
    if (n == 0) return true;
    if (bw == 0) {
        if (dict_count == 0) return false;
        memset(dst, 0, (size_t)n * sizeof(uint32_t));
        return true;
    }

    const uint64_t mask    = (bw == 32u) ? 0xFFFFFFFFULL : ((1ULL << bw) - 1ULL);
    uint64_t       bit_pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        size_t   wi = (size_t)(bit_pos >> 6);
        uint32_t bo = (uint32_t)(bit_pos & 63u);
        uint64_t v  = src_words[wi] >> bo;
        if (bo + bw > 64u) {
            v |= src_words[wi + 1u] << (64u - bo);
        }
        uint32_t code = (uint32_t)(v & mask);
        if (code >= dict_count) return false; /* fuzz/corrupt guard */
        dst[i]   = code;
        bit_pos += bw;
    }
    return true;
}

/* ============================================================================
 * 2. Frame-of-Reference + Bitpacking (VX_ENC_FOR_BP for i64 / u64)
 * ============================================================================ */
vx_status_t vx_enc_for_bp_encode(const void *src_vals, vx_type_t type,
                                 const uint64_t *validity, uint32_t nrows,
                                 uint8_t *dst, size_t dst_cap, size_t *out_len) {
    if (!src_vals || !dst || !out_len || nrows > VX_BATCH_CAP) return VX_ERR_INVALID;
    if (type != VX_TYPE_I64 && type != VX_TYPE_U64) return VX_ERR_SCHEMA;

    uint64_t base      = 0;
    uint64_t max_delta = 0;
    bool     has_valid = false;

    if (type == VX_TYPE_I64) {
        const int64_t *s = (const int64_t *)src_vals;
        int64_t        mn = 0, mx = 0;
        for (uint32_t i = 0; i < nrows; ++i) {
            if (!vx_enc_valid_bit(validity, i)) continue;
            if (!has_valid) { mn = mx = s[i]; has_valid = true; }
            else {
                if (s[i] < mn) mn = s[i];
                if (s[i] > mx) mx = s[i];
            }
        }
        base      = (uint64_t)mn;
        max_delta = has_valid ? ((uint64_t)mx - (uint64_t)mn) : 0ULL;
    } else {
        const uint64_t *s = (const uint64_t *)src_vals;
        uint64_t        mn = 0, mx = 0;
        for (uint32_t i = 0; i < nrows; ++i) {
            if (!vx_enc_valid_bit(validity, i)) continue;
            if (!has_valid) { mn = mx = s[i]; has_valid = true; }
            else {
                if (s[i] < mn) mn = s[i];
                if (s[i] > mx) mx = s[i];
            }
        }
        base      = mn;
        max_delta = has_valid ? (mx - mn) : 0ULL;
    }

    const uint8_t bw     = vx_bits_needed_u64(max_delta);
    const size_t  nwords = vx_packed_words(nrows, bw);
    const size_t  need   = sizeof(vx_for_hdr_t) + nwords * sizeof(uint64_t);
    if (need > dst_cap) return VX_ERR_OOM;

    vx_for_hdr_t hdr = { .base = base, .nrows = nrows, .bit_width = bw, ._pad = { 0, 0, 0 } };
    memcpy(dst, &hdr, sizeof(hdr));

    if (bw > 0 && nrows > 0) {
        uint64_t deltas[VX_BATCH_CAP];
        const uint64_t *raw = (const uint64_t *)src_vals;
        for (uint32_t i = 0; i < nrows; ++i) {
            deltas[i] = vx_enc_valid_bit(validity, i) ? (raw[i] - base) : 0ULL;
        }
        vx_bitpack_u64(deltas, nrows, bw, (uint64_t *)(dst + sizeof(hdr)));
    }

    *out_len = need;
    return VX_OK;
}

static vx_status_t vx_enc_for_bp_decode(const vx_enc_col_ctx_t *ec, vx_arena_t *scratch,
                                        vx_vec_t *out) {
    if (ec->type != VX_TYPE_I64 && ec->type != VX_TYPE_U64) return VX_ERR_CORRUPT;
    if (ec->page_len < sizeof(vx_for_hdr_t)) return VX_ERR_CORRUPT;

    vx_for_hdr_t hdr;
    memcpy(&hdr, ec->page_data, sizeof(hdr));
    if (hdr.nrows != ec->nrows || hdr.bit_width > 64u) return VX_ERR_CORRUPT;

    const size_t nwords = vx_packed_words(hdr.nrows, hdr.bit_width);
    const size_t need   = sizeof(hdr) + nwords * sizeof(uint64_t);
    if (ec->page_len < need) return VX_ERR_CORRUPT;

    uint64_t *dst = (uint64_t *)vx_arena_alloc(scratch, (size_t)hdr.nrows * sizeof(uint64_t));
    if (!dst && hdr.nrows > 0) return VX_ERR_OOM;

    const uint64_t *words = (const uint64_t *)((const uint8_t *)ec->page_data + sizeof(hdr));
    vx_bitunpack_u64_add(words, hdr.nrows, hdr.bit_width, hdr.base, dst);

    out->type      = ec->type;
    out->kind      = VX_VEC_FLAT;
    out->has_nulls = ec->has_nulls;
    out->phys_len  = hdr.nrows;
    out->data      = dst;
    out->validity  = ec->has_nulls ? ec->validity : NULL;
    out->owner     = vx_owner_retain(&scratch->hdr);
    return VX_OK;
}

/* ============================================================================
 * 3. String Dictionary Builder & VX_ENC_DICT Encode / Decode
 * ============================================================================ */
static uint64_t vx_dict_hash_sv(vx_strview_t sv, const vx_strpool_t *pool) {
    const uint8_t *p = vx_strview_ptr(&sv, pool);
    uint64_t       h = 14695981039346656037ULL ^ (uint64_t)(uint32_t)sv.inl.len;
    for (int32_t i = 0; i < sv.inl.len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    return h ^ (h >> 33);
}

vx_status_t vx_dict_builder_init(vx_dict_builder_t *db, uint32_t max_dict) {
    memset(db, 0, sizeof(*db));
    db->max_dict = max_dict ? max_dict : 4096u;
    return VX_OK;
}

void vx_dict_builder_destroy(vx_dict_builder_t *db) {
    if (!db) return;
    free(db->vals);
    free(db->hashes);
    free(db->buckets);
    memset(db, 0, sizeof(*db));
}

static vx_status_t vx_dict_builder_rehash(vx_dict_builder_t *db, uint32_t new_cap) {
    uint32_t *nb = (uint32_t *)calloc(new_cap, sizeof(*nb));
    if (!nb) return VX_ERR_OOM;
    const uint32_t mask = new_cap - 1u;
    for (uint32_t i = 0; i < db->count; ++i) {
        uint32_t pos = (uint32_t)db->hashes[i] & mask;
        while (nb[pos] != 0) pos = (pos + 1u) & mask;
        nb[pos] = i + 1u;
    }
    free(db->buckets);
    db->buckets = nb;
    db->cap_bkt = new_cap;
    return VX_OK;
}

vx_status_t vx_dict_builder_intern(vx_dict_builder_t *db, vx_strview_t sv,
                                   const vx_strpool_t *pool, uint32_t *out_code) {
    if (db->cap_bkt == 0 || ((size_t)(db->count + 1u) * 10u >= (size_t)db->cap_bkt * 7u)) {
        uint32_t    ncap = db->cap_bkt ? db->cap_bkt * 2u : 64u;
        vx_status_t st   = vx_dict_builder_rehash(db, ncap);
        if (st != VX_OK) return st;
    }

    const uint64_t h    = vx_dict_hash_sv(sv, pool);
    const uint32_t mask = db->cap_bkt - 1u;
    uint32_t       pos  = (uint32_t)h & mask;

    for (;;) {
        uint32_t slot = db->buckets[pos];
        if (slot == 0) {
            if (db->count >= db->max_dict) return VX_ERR_INVALID; /* cardinality too high */
            if (db->count == db->cap_vals) {
                uint32_t      ncap = db->cap_vals ? db->cap_vals * 2u : 32u;
                vx_strview_t *nv   = (vx_strview_t *)realloc(db->vals, (size_t)ncap * sizeof(*nv));
                uint64_t     *nh   = (uint64_t *)realloc(db->hashes, (size_t)ncap * sizeof(*nh));
                if (!nv || !nh) {
                    if (nv) db->vals   = nv;
                    if (nh) db->hashes = nh;
                    return VX_ERR_OOM;
                }
                db->vals     = nv;
                db->hashes   = nh;
                db->cap_vals = ncap;
            }
            uint32_t code = db->count++;
            db->vals[code]   = sv;
            db->hashes[code] = h;
            db->buckets[pos] = code + 1u;
            *out_code        = code;
            return VX_OK;
        }
        uint32_t code = slot - 1u;
        if (db->hashes[code] == h && vx_strview_eq(db->vals[code], pool, sv, pool)) {
            *out_code = code;
            return VX_OK;
        }
        pos = (pos + 1u) & mask;
    }
}

vx_status_t vx_enc_dict_encode(const uint32_t *codes, uint32_t nrows, uint32_t dict_count,
                               uint8_t *dst, size_t dst_cap, size_t *out_len) {
    if (!codes || !dst || !out_len || nrows > VX_BATCH_CAP) return VX_ERR_INVALID;

    const uint8_t bw     = (dict_count <= 1u) ? 0u : vx_bits_needed_u64((uint64_t)(dict_count - 1u));
    const size_t  nwords = vx_packed_words(nrows, bw);
    const size_t  need   = sizeof(vx_dict_hdr_t) + nwords * sizeof(uint64_t);
    if (need > dst_cap) return VX_ERR_OOM;

    vx_dict_hdr_t hdr = { .nrows = nrows, .bit_width = bw, ._pad = { 0, 0, 0 } };
    memcpy(dst, &hdr, sizeof(hdr));
    if (bw > 0 && nrows > 0) {
        vx_bitpack_u32(codes, nrows, bw, (uint64_t *)(dst + sizeof(hdr)));
    }
    *out_len = need;
    return VX_OK;
}

static vx_status_t vx_enc_dict_decode(const vx_enc_col_ctx_t *ec, vx_arena_t *scratch,
                                      vx_vec_t *out) {
    if (!ec->dict_vals || ec->dict_count == 0 || ec->page_len < sizeof(vx_dict_hdr_t)) {
        return VX_ERR_CORRUPT;
    }
    vx_dict_hdr_t hdr;
    memcpy(&hdr, ec->page_data, sizeof(hdr));
    if (hdr.nrows != ec->nrows || hdr.bit_width > 32u) return VX_ERR_CORRUPT;

    const size_t nwords = vx_packed_words(hdr.nrows, hdr.bit_width);
    const size_t need   = sizeof(hdr) + nwords * sizeof(uint64_t);
    if (ec->page_len < need) return VX_ERR_CORRUPT;

    uint32_t *codes = (uint32_t *)vx_arena_alloc(scratch, (size_t)hdr.nrows * sizeof(uint32_t));
    if (!codes && hdr.nrows > 0) return VX_ERR_OOM;

    const uint64_t *words = (const uint64_t *)((const uint8_t *)ec->page_data + sizeof(hdr));
    if (!vx_bitunpack_u32_checked(words, hdr.nrows, hdr.bit_width, ec->dict_count, codes)) {
        return VX_ERR_CORRUPT;
    }

    /* Bind directly as VX_VEC_DICT: zero string expansion! */
    out->type      = ec->type;
    out->kind      = VX_VEC_DICT;
    out->has_nulls = ec->has_nulls;
    out->phys_len  = hdr.nrows;
    out->dict_size = ec->dict_count;
    out->data      = codes;
    out->dict_vals = ec->dict_vals;
    out->validity  = ec->has_nulls ? ec->validity : NULL;
    out->strpool   = ec->strpool;
    out->owner     = vx_owner_retain(&scratch->hdr);
    return VX_OK;
}

/* ============================================================================
 * 4. Run-Length Encoding (VX_ENC_RLE for all types)
 * ============================================================================ */
static bool vx_rle_vals_eq(const void *a, const void *b, vx_type_t type,
                           const vx_strpool_t *pool) {
    switch (type) {
    case VX_TYPE_BOOL: return *(const uint8_t *)a == *(const uint8_t *)b;
    case VX_TYPE_I64:
    case VX_TYPE_U64:
    case VX_TYPE_F64:  return memcmp(a, b, 8u) == 0; /* bit-exact: distinguishes -0.0 and NaN */
    case VX_TYPE_STR:  return vx_strview_eq(*(const vx_strview_t *)a, pool,
                                            *(const vx_strview_t *)b, pool);
    }
    return false;
}

vx_status_t vx_enc_rle_encode(const void *src_vals, vx_type_t type,
                              const uint64_t *validity, const vx_strpool_t *strpool,
                              uint32_t nrows, uint8_t *dst, size_t dst_cap, size_t *out_len) {
    if (!src_vals || !dst || !out_len || nrows > VX_BATCH_CAP) return VX_ERR_INVALID;
    const size_t elem_sz = vx_enc_elem_size(type);
    if (elem_sz == 0) return VX_ERR_SCHEMA;

    uint32_t run_lens[VX_BATCH_CAP];
    uint8_t  run_vals[VX_BATCH_CAP * sizeof(vx_strview_t)];
    uint32_t nruns = 0;

    for (uint32_t i = 0; i < nrows; ++i) {
        const uint8_t *cur = (const uint8_t *)src_vals + (size_t)i * elem_sz;
        const bool     val = vx_enc_valid_bit(validity, i);

        if (nruns == 0) {
            run_lens[0] = 1u;
            if (val) memcpy(run_vals, cur, elem_sz);
            else     memset(run_vals, 0, elem_sz);
            nruns = 1u;
        } else if (!val || vx_rle_vals_eq(cur, run_vals + (size_t)(nruns - 1u) * elem_sz,
                                          type, strpool)) {
            /* NULLs extend the active run without breaking it (validity bitmap restores NULL) */
            run_lens[nruns - 1u]++;
        } else {
            run_lens[nruns] = 1u;
            memcpy(run_vals + (size_t)nruns * elem_sz, cur, elem_sz);
            nruns++;
        }
    }

    const size_t lens_bytes = (size_t)nruns * sizeof(uint32_t);
    const size_t vals_off   = (size_t)vx_enc_align8(sizeof(vx_rle_hdr_t) + lens_bytes);
    const size_t vals_bytes = (size_t)nruns * elem_sz;
    const size_t need       = vals_off + vals_bytes;
    if (need > dst_cap) return VX_ERR_OOM;

    vx_rle_hdr_t hdr = { .nrows = nrows, .nruns = nruns, .type = (uint8_t)type, ._pad = { 0 } };
    memset(dst, 0, vals_off);
    memcpy(dst, &hdr, sizeof(hdr));
    if (lens_bytes) memcpy(dst + sizeof(hdr), run_lens, lens_bytes);
    if (vals_bytes) memcpy(dst + vals_off, run_vals, vals_bytes);

    *out_len = need;
    return VX_OK;
}

static vx_status_t vx_enc_rle_decode(const vx_enc_col_ctx_t *ec, vx_arena_t *scratch,
                                     vx_vec_t *out) {
    if (ec->page_len < sizeof(vx_rle_hdr_t)) return VX_ERR_CORRUPT;
    vx_rle_hdr_t hdr;
    memcpy(&hdr, ec->page_data, sizeof(hdr));
    if (hdr.nrows != ec->nrows || hdr.type != (uint8_t)ec->type || hdr.nruns > hdr.nrows) {
        return VX_ERR_CORRUPT;
    }

    const size_t elem_sz    = vx_enc_elem_size(ec->type);
    const size_t lens_bytes = (size_t)hdr.nruns * sizeof(uint32_t);
    const size_t vals_off   = (size_t)vx_enc_align8(sizeof(hdr) + lens_bytes);
    const size_t vals_bytes = (size_t)hdr.nruns * elem_sz;
    if (vals_off > ec->page_len || vals_bytes > ec->page_len - vals_off) {
        return VX_ERR_CORRUPT;
    }

    uint8_t *dst = (uint8_t *)vx_arena_alloc(scratch, (size_t)hdr.nrows * elem_sz);
    if (!dst && hdr.nrows > 0) return VX_ERR_OOM;

    const uint32_t *run_lens = (const uint32_t *)((const uint8_t *)ec->page_data + sizeof(hdr));
    const uint8_t  *run_vals = (const uint8_t *)ec->page_data + vals_off;

    uint32_t pos = 0;
    for (uint32_t r = 0; r < hdr.nruns; ++r) {
        uint32_t len = run_lens[r];
        if (len == 0 || len > hdr.nrows - pos) return VX_ERR_CORRUPT;

        if (elem_sz == 1u) {
            memset(dst + pos, run_vals[r], len);
        } else if (elem_sz == 8u) {
            uint64_t  v   = ((const uint64_t *)run_vals)[r];
            uint64_t *d64 = (uint64_t *)dst + pos;
            for (uint32_t k = 0; k < len; ++k) d64[k] = v;
        } else {
            vx_strview_t  sv  = ((const vx_strview_t *)run_vals)[r];
            vx_strview_t *dsv = (vx_strview_t *)dst + pos;
            for (uint32_t k = 0; k < len; ++k) dsv[k] = sv;
        }
        pos += len;
    }
    if (pos != hdr.nrows) return VX_ERR_CORRUPT;

    out->type      = ec->type;
    out->kind      = VX_VEC_FLAT;
    out->has_nulls = ec->has_nulls;
    out->phys_len  = hdr.nrows;
    out->data      = dst;
    out->validity  = ec->has_nulls ? ec->validity : NULL;
    out->strpool   = ec->strpool;
    out->owner     = vx_owner_retain(&scratch->hdr);
    return VX_OK;
}

/* ============================================================================
 * 5. Unified Decoder Entry Point (called from M12 vx_bind_col_chunk)
 * ============================================================================ */
vx_status_t vx_enc_decode_col_chunk(const vx_enc_col_ctx_t *ec, vx_arena_t *scratch,
                                    vx_vec_t *out_vec) {
    if (!ec || !scratch || !out_vec) return VX_ERR_INVALID;
    memset(out_vec, 0, sizeof(*out_vec));

    switch (ec->encoding) {
    case VX_ENC_DICT:   return vx_enc_dict_decode(ec, scratch, out_vec);
    case VX_ENC_FOR_BP: return vx_enc_for_bp_decode(ec, scratch, out_vec);
    case VX_ENC_RLE:    return vx_enc_rle_decode(ec, scratch, out_vec);
    case VX_ENC_PLAIN:  return VX_ERR_INVALID; /* handled zero-copy in vx_bind_col_chunk */
    }
    return VX_ERR_CORRUPT;
}

