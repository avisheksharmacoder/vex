#ifndef VX_RHMAP_H
#define VX_RHMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Open-addressing Robin Hood hash map (uint64_t -> uint64_t) with backward-shift
 * deletion. psl == 0 marks an empty slot, so ALL 64-bit keys (including 0 and
 * UINT64_MAX) are valid without sentinel restrictions. */
typedef struct {
    uint64_t key;
    uint64_t val;
    uint32_t psl; /* 0 = empty, 1 = ideal home bucket, >1 = displaced */
    uint32_t _pad;
} vx_rh_slot_t;

typedef struct {
    vx_rh_slot_t *slots;
    size_t        cap;  /* power of 2 */
    size_t        mask;
    size_t        len;
} vx_rhmap_t;

static inline uint64_t vx_pk_pack(uint32_t chunk_idx, uint32_t row_idx) {
    return ((uint64_t)chunk_idx << 32) | (uint64_t)row_idx;
}

static inline uint32_t vx_pk_chunk(uint64_t packed) {
    return (uint32_t)(packed >> 32);
}

static inline uint32_t vx_pk_row(uint64_t packed) {
    return (uint32_t)(packed & 0xFFFFFFFFULL);
}

bool        vx_rhmap_init(vx_rhmap_t *m, size_t initial_cap_pow2);
void        vx_rhmap_destroy(vx_rhmap_t *m);
vx_rhmap_t *vx_rhmap_clone(const vx_rhmap_t *src);

/* Inserts or overwrites key; grows automatically at 75% load factor */
bool        vx_rhmap_put(vx_rhmap_t *m, uint64_t key, uint64_t val);

/* O(1) lookup with early Robin Hood exit */
bool        vx_rhmap_get(const vx_rhmap_t *m, uint64_t key, uint64_t *out_val);

/* Backward-shift delete (zero tombstone buildup); returns true if key existed */
bool        vx_rhmap_del(vx_rhmap_t *m, uint64_t key, uint64_t *out_old_val);

size_t      vx_rhmap_len(const vx_rhmap_t *m);
size_t      vx_rhmap_cap(const vx_rhmap_t *m);
uint32_t    vx_rhmap_max_psl(const vx_rhmap_t *m);

#endif /* VX_RHMAP_H */

