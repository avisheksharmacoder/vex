#ifndef VX_DELMAP_H
#define VX_DELMAP_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Refcounted COW per-chunk deletion bitmap (tombstones).
 * Immutable once shared across MVCC snapshots; deleting a row clones only this
 * tiny bitmap (~512 bytes for a 4,096-row chunk) instead of copying column data. */
typedef struct {
    atomic_uint_fast32_t refcnt;
    uint32_t             cap;
    uint32_t             del_count;
    uint64_t            *bits; /* 64-byte aligned; bit == 1 means deleted */
} vx_delmap_t;

vx_delmap_t *vx_delmap_create(uint32_t cap);
vx_delmap_t *vx_delmap_clone(const vx_delmap_t *src);
vx_delmap_t *vx_delmap_inc(vx_delmap_t *dm);
void         vx_delmap_dec(vx_delmap_t *dm);

/* Marks row deleted; returns true if it was previously live, false if already deleted */
bool         vx_delmap_set(vx_delmap_t *dm, uint32_t row);

static inline bool vx_delmap_is_set(const vx_delmap_t *dm, uint32_t row) {
    if (!dm || dm->del_count == 0 || row >= dm->cap) {
        return false;
    }
    return (dm->bits[row >> 6] & (1ULL << (row & 63U))) != 0;
}

static inline uint32_t vx_delmap_count(const vx_delmap_t *dm) {
    return dm ? dm->del_count : 0;
}

/* Branchless compaction: strips deleted row indices from a SIMD selection vector in-place */
uint32_t vx_delmap_filter_sel(const vx_delmap_t *dm, uint32_t *sel, uint32_t n);

#endif /* VX_DELMAP_H */

