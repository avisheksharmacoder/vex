#include "vx_delmap.h"

#include <stdlib.h>
#include <string.h>

static size_t delmap_bytes(uint32_t cap) {
    size_t words = ((size_t)cap + 63U) / 64U;
    size_t bytes = words * sizeof(uint64_t);
    return bytes < 64U ? 64U : bytes;
}

vx_delmap_t *vx_delmap_create(uint32_t cap) {
    if (cap == 0) {
        return NULL;
    }
    vx_delmap_t *dm = (vx_delmap_t *)calloc(1, sizeof(vx_delmap_t));
    if (!dm) {
        return NULL;
    }
    void *mem = NULL;
    size_t bytes = delmap_bytes(cap);
    if (posix_memalign(&mem, 64U, bytes) != 0 || !mem) {
        free(dm);
        return NULL;
    }
    memset(mem, 0, bytes);
    atomic_init(&dm->refcnt, 1);
    dm->cap = cap;
    dm->del_count = 0;
    dm->bits = (uint64_t *)mem;
    return dm;
}

vx_delmap_t *vx_delmap_clone(const vx_delmap_t *src) {
    if (!src || src->cap == 0) {
        return NULL;
    }
    vx_delmap_t *dst = vx_delmap_create(src->cap);
    if (!dst) {
        return NULL;
    }
    dst->del_count = src->del_count;
    memcpy(dst->bits, src->bits, delmap_bytes(src->cap));
    return dst;
}

vx_delmap_t *vx_delmap_inc(vx_delmap_t *dm) {
    if (dm) {
        atomic_fetch_add_explicit(&dm->refcnt, 1, memory_order_relaxed);
    }
    return dm;
}

void vx_delmap_dec(vx_delmap_t *dm) {
    if (!dm) {
        return;
    }
    if (atomic_fetch_sub_explicit(&dm->refcnt, 1, memory_order_acq_rel) == 1) {
        free(dm->bits);
        free(dm);
    }
}

bool vx_delmap_set(vx_delmap_t *dm, uint32_t row) {
    if (!dm || row >= dm->cap) {
        return false;
    }
    uint64_t mask = 1ULL << (row & 63U);
    if ((dm->bits[row >> 6] & mask) != 0ULL) {
        return false;
    }
    dm->bits[row >> 6] |= mask;
    dm->del_count++;
    return true;
}

uint32_t vx_delmap_filter_sel(const vx_delmap_t *dm, uint32_t *sel, uint32_t n) {
    if (!dm || dm->del_count == 0 || !sel || n == 0) {
        return n;
    }
    const uint64_t *restrict bits = dm->bits;
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = sel[i];
        uint32_t live = (uint32_t)(((bits[r >> 6] >> (r & 63U)) & 1ULL) == 0ULL);
        sel[out] = r;
        out += live;
    }
    return out;
}

