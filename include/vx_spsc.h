#ifndef VX_SPSC_H
#define VX_SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_CACHELINE 64

typedef struct {
    /* Producer-owned cache line */
    _Alignas(VX_CACHELINE) atomic_size_t head;
    size_t cached_tail;

    /* Consumer-owned cache line */
    _Alignas(VX_CACHELINE) atomic_size_t tail;
    size_t cached_head;

    /* Read-only geometry */
    _Alignas(VX_CACHELINE) size_t cap;
    size_t   mask;
    size_t   slot_size;
    uint8_t *slots;
} vx_spsc_t;

/* capacity must be a power of 2 >= 2; all `capacity` slots are usable */
bool   vx_spsc_init(vx_spsc_t *q, size_t capacity_pow2, size_t slot_size);
void   vx_spsc_destroy(vx_spsc_t *q);
bool   vx_spsc_try_push(vx_spsc_t *q, const void *item);
bool   vx_spsc_try_pop(vx_spsc_t *q, void *out_item);
size_t vx_spsc_drain(vx_spsc_t *q, void *out_buf, size_t max_items);
size_t vx_spsc_size_approx(const vx_spsc_t *q);

#endif /* VX_SPSC_H */
