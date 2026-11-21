#include "vx_spsc.h"

#include <stdlib.h>
#include <string.h>

static inline bool is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

bool vx_spsc_init(vx_spsc_t *q, size_t capacity_pow2, size_t slot_size) {
    if (!q || capacity_pow2 < 2 || !is_pow2(capacity_pow2) || slot_size == 0) {
        return false;
    }
    if (capacity_pow2 > SIZE_MAX / slot_size) {
        return false;
    }

    size_t total_bytes = capacity_pow2 * slot_size;
    void *mem = NULL;
    if (posix_memalign(&mem, VX_CACHELINE, total_bytes) != 0 || !mem) {
        return false;
    }

    atomic_init(&q->head, 0);
    q->cached_tail = 0;

    atomic_init(&q->tail, 0);
    q->cached_head = 0;

    q->cap = capacity_pow2;
    q->mask = capacity_pow2 - 1;
    q->slot_size = slot_size;
    q->slots = (uint8_t *)mem;
    return true;
}

void vx_spsc_destroy(vx_spsc_t *q) {
    if (!q) {
        return;
    }
    free(q->slots);
    q->slots = NULL;
    q->cap = 0;
    q->mask = 0;
    q->slot_size = 0;
    q->cached_head = 0;
    q->cached_tail = 0;
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
}

bool vx_spsc_try_push(vx_spsc_t *q, const void *item) {
    if (!q || !q->slots || !item) {
        return false;
    }

    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (head - q->cached_tail == q->cap) {
        q->cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head - q->cached_tail == q->cap) {
            return false;
        }
    }

    size_t idx = head & q->mask;
    memcpy(q->slots + (idx * q->slot_size), item, q->slot_size);
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

bool vx_spsc_try_pop(vx_spsc_t *q, void *out_item) {
    if (!q || !q->slots || !out_item) {
        return false;
    }

    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (tail == q->cached_head) {
        q->cached_head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (tail == q->cached_head) {
            return false;
        }
    }

    size_t idx = tail & q->mask;
    memcpy(out_item, q->slots + (idx * q->slot_size), q->slot_size);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

size_t vx_spsc_drain(vx_spsc_t *q, void *out_buf, size_t max_items) {
    if (!q || !q->slots || !out_buf || max_items == 0) {
        return 0;
    }

    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t avail = q->cached_head - tail;
    if (avail < max_items) {
        q->cached_head = atomic_load_explicit(&q->head, memory_order_acquire);
        avail = q->cached_head - tail;
        if (avail == 0) {
            return 0;
        }
    }

    size_t n = avail < max_items ? avail : max_items;
    size_t idx = tail & q->mask;
    size_t first = q->cap - idx;
    if (first > n) {
        first = n;
    }

    memcpy(out_buf, q->slots + (idx * q->slot_size), first * q->slot_size);
    size_t rem = n - first;
    if (rem > 0) {
        memcpy((uint8_t *)out_buf + (first * q->slot_size),
               q->slots,
               rem * q->slot_size);
    }

    atomic_store_explicit(&q->tail, tail + n, memory_order_release);
    return n;
}

size_t vx_spsc_size_approx(const vx_spsc_t *q) {
    if (!q || !q->slots) {
        return 0;
    }
    /* Load tail before head so monotonic head >= tail prevents underflow */
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t diff = head - tail;
    return diff > q->cap ? q->cap : diff;
}
