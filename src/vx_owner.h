#pragma once
#include "vx_types.h"

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_owner_kind : uint32_t {
    VX_OWN_ARENA   = 1,
    VX_OWN_CHUNK   = 2,
    VX_OWN_DELMAP  = 3,
    VX_OWN_STRBUF  = 4,
    VX_OWN_SEGMENT = 5, /* M13: munmap() + close() + optional unlink() */
    VX_OWN_CATALOG = 6,
    VX_OWN_RESULT  = 7,
    VX_OWN_JOIN_HT = 8,
    VX_OWN_INDEX   = 9
} vx_owner_kind_t;

typedef struct vx_owner vx_owner_t;
typedef void (*vx_destroy_fn)(vx_owner_t *self);

struct vx_owner {
    _Atomic uint64_t refcnt;
    vx_destroy_fn    destroy;
    vx_owner_kind_t  kind;
    uint32_t         flags;
};

static inline void vx_owner_init(vx_owner_t *o, vx_owner_kind_t kind, vx_destroy_fn fn) {
    atomic_init(&o->refcnt, 1);
    o->destroy = fn;
    o->kind    = kind;
    o->flags   = 0;
}

static inline vx_owner_t *vx_owner_retain(vx_owner_t *o) {
    if (o) {
        atomic_fetch_add_explicit(&o->refcnt, 1, memory_order_relaxed);
    }
    return o;
}

static inline void vx_owner_release(vx_owner_t *o) {
    if (!o) return;
#if defined(__SANITIZE_THREAD__)
    if (atomic_fetch_sub_explicit(&o->refcnt, 1, memory_order_acq_rel) == 1) {
        o->destroy(o);
    }
#else
    if (atomic_fetch_sub_explicit(&o->refcnt, 1, memory_order_release) == 1) {
        atomic_thread_fence(memory_order_acquire);
        o->destroy(o);
    }
#endif
}

