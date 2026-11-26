#include <stdlib.h>
#include <string.h>
#include "vx_chunk.h"
#include "vx_platform_win32.h"

/* L1-hot 4-bit nibble expansion tables for branchless selection-vector compaction */
static const uint8_t k_nibble_idx[16][4] = {
    { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 0, 1, 0, 0 },
    { 2, 0, 0, 0 }, { 0, 2, 0, 0 }, { 1, 2, 0, 0 }, { 0, 1, 2, 0 },
    { 3, 0, 0, 0 }, { 0, 3, 0, 0 }, { 1, 3, 0, 0 }, { 0, 1, 3, 0 },
    { 2, 3, 0, 0 }, { 0, 2, 3, 0 }, { 1, 2, 3, 0 }, { 0, 1, 2, 3 }
};

static const uint8_t k_nibble_cnt[16] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
};

static void vx_delmap_destroy(vx_owner_t *owner) {
    vx_aligned_free(owner);
}

vx_delmap_t *vx_delmap_create(uint32_t nrows) {
    if (nrows > VX_BATCH_CAP) return NULL;

    vx_delmap_t *dm = NULL;
    if (vx_aligned_alloc_posix((void **)&dm, VX_ALIGN, sizeof(*dm)) != 0 || !dm) {
        return NULL;
    }
    memset(dm, 0, sizeof(*dm));
    vx_owner_init(&dm->hdr, VX_OWN_DELMAP, vx_delmap_destroy);
    dm->generation = 1ULL;
    dm->nrows      = nrows;
    dm->live_count = nrows;
    return dm;
}

vx_delmap_t *vx_delmap_clone_and_set(const vx_delmap_t *src, uint32_t row_idx) {
    if (!src || row_idx >= src->nrows) return NULL;

    const uint32_t w    = row_idx >> 6;
    const uint64_t mask = 1ULL << (row_idx & 63u);

    /* Idempotent delete: retain immutable src with zero allocation & unchanged generation */
    if ((src->bits[w] & mask) != 0ULL) {
        return (vx_delmap_t *)vx_owner_retain((vx_owner_t *)&src->hdr);
    }

    vx_delmap_t *dst = NULL;
    if (vx_aligned_alloc_posix((void **)&dst, VX_ALIGN, sizeof(*dst)) != 0 || !dst) {
        return NULL;
    }
    memcpy(dst, src, sizeof(*dst));
    vx_owner_init(&dst->hdr, VX_OWN_DELMAP, vx_delmap_destroy);
    dst->generation = src->generation + 1ULL;
    dst->bits[w]   |= mask;
    if (dst->live_count > 0u) dst->live_count--;
    return dst;
}

/* Branchless 4-row nibble emit.
 * Precondition: out_n + 4 <= nrows (guaranteed whenever base + 4 <= nrows since out_n <= base). */
static inline uint32_t vx_emit_nibble_branchless(uint32_t *out_sel, uint32_t out_n,
                                                 uint32_t base, uint32_t nib) {
    const uint8_t *t = k_nibble_idx[nib];
    out_sel[out_n + 0u] = base + t[0];
    out_sel[out_n + 1u] = base + t[1];
    out_sel[out_n + 2u] = base + t[2];
    out_sel[out_n + 3u] = base + t[3];
    return out_n + k_nibble_cnt[nib];
}

uint32_t vx_delmap_live_sel(const vx_delmap_t *dm, uint32_t nrows, uint32_t *out_sel) {
    if (!out_sel || nrows == 0u) return 0u;
    const uint32_t n = (nrows <= VX_BATCH_CAP) ? nrows : VX_BATCH_CAP;

    /* Fast path: NULL delmap or 0 tombstones -> contiguous identity fill */
    if (!dm || dm->live_count >= n) {
        for (uint32_t i = 0; i < n; ++i) out_sel[i] = i;
        return n;
    }
    if (dm->live_count == 0u) return 0u;

    const uint32_t nwords = (n + 63u) >> 6;
    uint32_t       out_n  = 0u;

    for (uint32_t w = 0; w < nwords; ++w) {
        const uint32_t base      = w << 6;
        const uint32_t word_rows = (n - base >= 64u) ? 64u : (n - base);
        uint64_t       live      = ~dm->bits[w];
        if (word_rows < 64u) {
            live &= (1ULL << word_rows) - 1ULL;
        }
        if (live == 0ULL) continue;

        /* Dense fast path: all 64 rows in this word are live */
        if (word_rows == 64u && live == ~0ULL) {
            for (uint32_t k = 0; k < 64u; ++k) out_sel[out_n + k] = base + k;
            out_n += 64u;
            continue;
        }

        /* Sparse fast path: <= 8 live rows in word -> tight ctzll loop */
        if (__builtin_popcountll(live) <= 8u) {
            while (live) {
                uint32_t b = (uint32_t)__builtin_ctzll(live);
                live      &= live - 1ULL;
                out_sel[out_n++] = base + b;
            }
            continue;
        }

        /* Mixed density: 100% branchless 4-bit nibble expansion (safe: out_n <= r0 <= n - 4) */
        uint32_t j = 0u;
        for (; j + 4u <= word_rows; j += 4u) {
            out_n = vx_emit_nibble_branchless(out_sel, out_n, base + j,
                                              (uint32_t)((live >> j) & 0xFu));
        }

        /* Final 1..3 rows of a non-multiple-of-4 partial chunk */
        if (j < word_rows) {
            uint64_t rem = live >> j;
            while (rem) {
                uint32_t b = (uint32_t)__builtin_ctzll(rem);
                rem       &= rem - 1ULL;
                out_sel[out_n++] = base + j + b;
            }
        }
    }

    return out_n;
}

