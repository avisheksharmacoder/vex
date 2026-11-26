#pragma once
#include "vx_owner.h"

typedef struct vx_slab {
    struct vx_slab *next;
    size_t          cap;   /* usable payload bytes after this 64B header */
    size_t          used;
    uint64_t        _pad[5];
} vx_slab_t;

_Static_assert(sizeof(vx_slab_t) == VX_ALIGN, "vx_slab_t must be 64B aligned");

typedef struct vx_arena {
    vx_owner_t  hdr;            /* refcounted so Result/ColumnView can pin output arena */
    vx_slab_t  *head;
    vx_slab_t  *curr;
    size_t      init_slab_cap;  /* e.g. 64 KiB */
    size_t      max_slab_cap;   /* e.g. 4 MiB; larger requests get dedicated slabs */
    size_t      bytes_allocated;/* total heap bytes across all live slabs */
    size_t      bytes_limit;    /* 0 = unlimited; >0 enforces work_mem / OOM cap */
    bool        oom_hit;
} vx_arena_t;

typedef struct vx_arena_mark {
    vx_slab_t *slab;
    size_t     used;
} vx_arena_mark_t;

/* Lifecycle */
vx_arena_t *vx_arena_create(size_t init_slab_cap, size_t max_slab_cap, size_t bytes_limit);
/* Destroy via vx_owner_release(&arena->hdr); */

/* Allocation (all return NULL on OOM and set arena->oom_hit = true) */
void *vx_arena_alloc(vx_arena_t *a, size_t size);                       /* default 64B aligned */
void *vx_arena_alloc_aligned(vx_arena_t *a, size_t size, size_t align); /* align = power of 2 */
void *vx_arena_calloc(vx_arena_t *a, size_t count, size_t elem_size);   /* overflow-checked */

/* Scratch-pad reuse inside M12 streaming loops */
vx_arena_mark_t vx_arena_mark(const vx_arena_t *a);
void            vx_arena_rewind(vx_arena_t *a, vx_arena_mark_t mark);   /* frees slabs after mark */
void            vx_arena_reset(vx_arena_t *a);                          /* keeps head slab, frees rest */

