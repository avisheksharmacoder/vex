#include "../include/vx_arena.h"

#include <stdlib.h>
#include <string.h>

#define VX_ARENA_BASE_ALIGN 64

static inline bool is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

bool vx_arena_init(vx_arena_t *a, size_t capacity) {
    if (!a || capacity == 0) {
        return false;
    }

    void *mem = NULL;
    if (posix_memalign(&mem, VX_ARENA_BASE_ALIGN, capacity) != 0 || !mem) {
        a->base = NULL;
        a->cap = 0;
        a->offset = 0;
        return false;
    }

    a->base = (uint8_t *)mem;
    a->cap = capacity;
    a->offset = 0;
    return true;
}

void vx_arena_destroy(vx_arena_t *a) {
    if (!a) {
        return;
    }
    free(a->base);
    a->base = NULL;
    a->cap = 0;
    a->offset = 0;
}

void *vx_arena_alloc(vx_arena_t *a, size_t size, size_t align) {
    if (!a || !a->base || size == 0 || !is_pow2(align)) {
        return NULL;
    }

    uintptr_t curr = (uintptr_t)(a->base + a->offset);
    uintptr_t rem = curr & ((uintptr_t)align - 1);
    size_t pad = rem ? (size_t)((uintptr_t)align - rem) : 0;

    size_t avail = a->cap - a->offset;
    if (pad > avail || size > avail - pad) {
        return NULL;
    }

    size_t aligned_off = a->offset + pad;
    a->offset = aligned_off + size;
    return a->base + aligned_off;
}

void vx_arena_reset(vx_arena_t *a) {
    if (!a) {
        return;
    }
    a->offset = 0;
}

size_t vx_arena_used(const vx_arena_t *a) {
    return a ? a->offset : 0;
}

size_t vx_arena_cap(const vx_arena_t *a) {
    return a ? a->cap : 0;
}

