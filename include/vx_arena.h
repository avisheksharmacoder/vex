#ifndef VX_ARENA_H
#define VX_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    size_t   cap;
    size_t   offset;
} vx_arena_t;

bool   vx_arena_init(vx_arena_t *a, size_t capacity);
void   vx_arena_destroy(vx_arena_t *a);
void  *vx_arena_alloc(vx_arena_t *a, size_t size, size_t align);
void   vx_arena_reset(vx_arena_t *a);
size_t vx_arena_used(const vx_arena_t *a);
size_t vx_arena_cap(const vx_arena_t *a);

#endif /* VX_ARENA_H */

