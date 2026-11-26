#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vx_arena.h"
#include "vx_platform_win32.h"

#define VX_ARENA_DEF_INIT_CAP (64u * 1024u)       /* 64 KiB */
#define VX_ARENA_DEF_MAX_CAP  (4u * 1024u * 1024u) /* 4 MiB  */

static inline bool vx_is_pow2_size(size_t x) {
    return x != 0u && (x & (x - 1u)) == 0u;
}

static inline size_t vx_align_up_size(size_t v, size_t align) {
    return (v + (align - 1u)) & ~(align - 1u);
}

static inline uint8_t *vx_slab_payload(vx_slab_t *s) {
    return (uint8_t *)s + sizeof(vx_slab_t);
}

static void vx_slab_free_chain(vx_arena_t *a, vx_slab_t *s) {
    while (s) {
        vx_slab_t *next = s->next;
        size_t     tot  = sizeof(vx_slab_t) + s->cap;
        a->bytes_allocated = (a->bytes_allocated >= tot) ? (a->bytes_allocated - tot) : 0u;
        vx_aligned_free(s);
        s = next;
    }
}

static void vx_arena_destroy(vx_owner_t *owner) {
    vx_arena_t *a = (vx_arena_t *)owner;
    if (!a) return;
    vx_slab_free_chain(a, a->head);
    free(a);
}

static vx_slab_t *vx_arena_new_slab(vx_arena_t *a, size_t min_payload) {
    size_t cap = a->init_slab_cap;
    if (a->curr) {
        size_t doubled = (a->curr->cap <= SIZE_MAX / 2u) ? (a->curr->cap * 2u) : a->max_slab_cap;
        cap = (doubled < a->max_slab_cap) ? doubled : a->max_slab_cap;
        if (cap < a->init_slab_cap) cap = a->init_slab_cap;
    }
    if (cap < min_payload) {
        if (min_payload > SIZE_MAX - (VX_ALIGN - 1u)) { a->oom_hit = true; return NULL; }
        cap = vx_align_up_size(min_payload, VX_ALIGN);
    }

    if (cap > SIZE_MAX - sizeof(vx_slab_t)) {
        a->oom_hit = true;
        return NULL;
    }
    const size_t total = sizeof(vx_slab_t) + cap;

    if (a->bytes_limit > 0u) {
        if (total > a->bytes_limit || a->bytes_allocated > a->bytes_limit - total) {
            a->oom_hit = true;
            return NULL;
        }
    }

    vx_slab_t *s = NULL;
    if (vx_aligned_alloc_posix((void **)&s, VX_ALIGN, total) != 0 || !s) {
        a->oom_hit = true;
        return NULL;
    }
    s->next = NULL;
    s->cap  = cap;
    s->used = 0u;

    if (!a->head) {
        a->head = s;
    } else {
        a->curr->next = s;
    }
    a->curr             = s;
    a->bytes_allocated += total;
    return s;
}

vx_arena_t *vx_arena_create(size_t init_slab_cap, size_t max_slab_cap, size_t bytes_limit) {
    vx_arena_t *a = (vx_arena_t *)calloc(1u, sizeof(*a));
    if (!a) return NULL;

    size_t icap = init_slab_cap ? vx_align_up_size(init_slab_cap, VX_ALIGN) : VX_ARENA_DEF_INIT_CAP;
    size_t mcap = max_slab_cap  ? vx_align_up_size(max_slab_cap,  VX_ALIGN) : VX_ARENA_DEF_MAX_CAP;
    if (mcap < icap) mcap = icap;

    vx_owner_init(&a->hdr, VX_OWN_ARENA, vx_arena_destroy);
    a->init_slab_cap = icap;
    a->max_slab_cap  = mcap;
    a->bytes_limit   = bytes_limit;
    return a;
}

void *vx_arena_alloc_aligned(vx_arena_t *a, size_t size, size_t align) {
    if (!a || !vx_is_pow2_size(align)) {
        if (a) a->oom_hit = true;
        return NULL;
    }

    /* Try bumping inside current slab first */
    if (a->curr) {
        uintptr_t base    = (uintptr_t)vx_slab_payload(a->curr);
        uintptr_t cur_ptr = base + a->curr->used;
        uintptr_t mask    = (uintptr_t)align - 1u;

        if (cur_ptr <= UINTPTR_MAX - mask) {
            uintptr_t aligned_ptr = (cur_ptr + mask) & ~mask;
            size_t    aligned_off = (size_t)(aligned_ptr - base);
            if (aligned_off <= a->curr->cap && size <= a->curr->cap - aligned_off) {
                a->curr->used = aligned_off + size;
                return (void *)aligned_ptr;
            }
        }
    }

    /* Allocate a fresh slab (add alignment slack only when align > 64B slab alignment) */
    size_t slack = (align > VX_ALIGN) ? (align - VX_ALIGN) : 0u;
    if (size > SIZE_MAX - slack) {
        a->oom_hit = true;
        return NULL;
    }
    vx_slab_t *s = vx_arena_new_slab(a, size + slack);
    if (!s) return NULL;

    uintptr_t base        = (uintptr_t)vx_slab_payload(s);
    uintptr_t mask        = (uintptr_t)align - 1u;
    uintptr_t aligned_ptr = (base + mask) & ~mask;
    size_t    aligned_off = (size_t)(aligned_ptr - base);

    s->used = aligned_off + size;
    return (void *)aligned_ptr;
}

void *vx_arena_alloc(vx_arena_t *a, size_t size) {
    return vx_arena_alloc_aligned(a, size, VX_ALIGN);
}

void *vx_arena_calloc(vx_arena_t *a, size_t count, size_t elem_size) {
    if (!a) return NULL;
    if (count != 0u && elem_size > SIZE_MAX / count) {
        a->oom_hit = true;
        return NULL;
    }
    size_t total = count * elem_size;
    void  *p     = vx_arena_alloc_aligned(a, total, VX_ALIGN);
    if (p && total > 0u) memset(p, 0, total);
    return p;
}

vx_arena_mark_t vx_arena_mark(const vx_arena_t *a) {
    vx_arena_mark_t m = { .slab = NULL, .used = 0u };
    if (a && a->curr) {
        m.slab = a->curr;
        m.used = a->curr->used;
    }
    return m;
}

void vx_arena_rewind(vx_arena_t *a, vx_arena_mark_t mark) {
    if (!a) return;
    if (!mark.slab) {
        vx_slab_free_chain(a, a->head);
        a->head = NULL;
        a->curr = NULL;
        return;
    }
    vx_slab_free_chain(a, mark.slab->next);
    mark.slab->next = NULL;
    mark.slab->used = (mark.used <= mark.slab->cap) ? mark.used : mark.slab->cap;
    a->curr         = mark.slab;
}

void vx_arena_reset(vx_arena_t *a) {
    if (!a) return;
    if (a->head) {
        vx_slab_free_chain(a, a->head->next);
        a->head->next = NULL;
        a->head->used = 0u;
        a->curr       = a->head;
    }
    a->oom_hit = false;
}

