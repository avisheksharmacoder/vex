#include "vx_ebr.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define VX_EBR_CACHELINE 64U
#define VX_EBR_INACTIVE  UINT64_MAX

struct vx_ebr_reader {
    _Alignas(VX_EBR_CACHELINE) atomic_uint_fast64_t pinned_epoch;
    atomic_int in_use;
    vx_ebr_t  *owner;
};

typedef struct vx_ebr_node {
    void               *ptr;
    vx_ebr_free_fn      free_fn;
    void               *user_ctx;
    uint64_t            retire_epoch;
    struct vx_ebr_node *next;
} vx_ebr_node_t;

struct vx_ebr {
    _Alignas(VX_EBR_CACHELINE) _Atomic(void *)      root;
    _Alignas(VX_EBR_CACHELINE) atomic_uint_fast64_t epoch;
    _Alignas(VX_EBR_CACHELINE) atomic_size_t        reg_count;

    size_t            max_readers;
    vx_ebr_reader_t  *readers;

    /* Single-writer retired queue */
    vx_ebr_node_t    *ret_head;
    vx_ebr_node_t    *ret_tail;
    atomic_size_t     pending_retired;
    atomic_uint_fast64_t total_reclaimed;
};

vx_ebr_t *vx_ebr_create(size_t max_readers, void *initial_root) {
    if (max_readers == 0) {
        return NULL;
    }

    void *mem = NULL;
    if (posix_memalign(&mem, VX_EBR_CACHELINE, sizeof(vx_ebr_t)) != 0 || !mem) {
        return NULL;
    }
    vx_ebr_t *ebr = (vx_ebr_t *)mem;
    memset(ebr, 0, sizeof(*ebr));

    void *rmem = NULL;
    if (posix_memalign(&rmem, VX_EBR_CACHELINE, max_readers * sizeof(vx_ebr_reader_t)) != 0 || !rmem) {
        free(ebr);
        return NULL;
    }
    ebr->readers = (vx_ebr_reader_t *)rmem;
    ebr->max_readers = max_readers;

    atomic_init(&ebr->root, initial_root);
    atomic_init(&ebr->epoch, 0);
    atomic_init(&ebr->reg_count, 0);
    atomic_init(&ebr->pending_retired, 0);
    atomic_init(&ebr->total_reclaimed, 0);

    for (size_t i = 0; i < max_readers; i++) {
        atomic_init(&ebr->readers[i].pinned_epoch, VX_EBR_INACTIVE);
        atomic_init(&ebr->readers[i].in_use, 0);
        ebr->readers[i].owner = ebr;
    }

    return ebr;
}

void vx_ebr_destroy(vx_ebr_t *ebr) {
    if (!ebr) {
        return;
    }
    vx_ebr_node_t *cur = ebr->ret_head;
    while (cur) {
        vx_ebr_node_t *next = cur->next;
        if (cur->free_fn) {
            cur->free_fn(cur->ptr, cur->user_ctx);
        }
        free(cur);
        cur = next;
    }
    free(ebr->readers);
    free(ebr);
}

vx_ebr_reader_t *vx_ebr_register_reader(vx_ebr_t *ebr) {
    if (!ebr) {
        return NULL;
    }
    for (size_t i = 0; i < ebr->max_readers; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&ebr->readers[i].in_use,
                                                    &expected, 1,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            atomic_store_explicit(&ebr->readers[i].pinned_epoch,
                                  VX_EBR_INACTIVE, memory_order_release);
            return &ebr->readers[i];
        }
    }
    return NULL;
}

void vx_ebr_unregister_reader(vx_ebr_reader_t *reader) {
    if (!reader) {
        return;
    }
    atomic_store_explicit(&reader->pinned_epoch, VX_EBR_INACTIVE, memory_order_seq_cst);
    atomic_store_explicit(&reader->in_use, 0, memory_order_release);
}

void *vx_ebr_enter(vx_ebr_reader_t *reader) {
    if (!reader || !reader->owner) {
        return NULL;
    }
    vx_ebr_t *ebr = reader->owner;
    uint64_t e = atomic_load_explicit(&ebr->epoch, memory_order_seq_cst);
    atomic_store_explicit(&reader->pinned_epoch, e, memory_order_seq_cst);
    return atomic_load_explicit(&ebr->root, memory_order_seq_cst);
}

void vx_ebr_exit(vx_ebr_reader_t *reader) {
    if (!reader) {
        return;
    }
    atomic_store_explicit(&reader->pinned_epoch, VX_EBR_INACTIVE, memory_order_seq_cst);
}

void *vx_ebr_publish(vx_ebr_t *ebr, void *new_root) {
    if (!ebr) {
        return NULL;
    }
    void *old = atomic_exchange_explicit(&ebr->root, new_root, memory_order_seq_cst);
    atomic_fetch_add_explicit(&ebr->epoch, 1, memory_order_seq_cst);
    return old;
}

bool vx_ebr_retire(vx_ebr_t *ebr, void *old_ptr,
                   vx_ebr_free_fn free_fn, void *user_ctx) {
    if (!ebr || !free_fn) {
        return false;
    }
    vx_ebr_node_t *node = (vx_ebr_node_t *)malloc(sizeof(vx_ebr_node_t));
    if (!node) {
        return false;
    }
    node->ptr = old_ptr;
    node->free_fn = free_fn;
    node->user_ctx = user_ctx;
    node->retire_epoch = atomic_load_explicit(&ebr->epoch, memory_order_acquire);
    node->next = NULL;

    if (!ebr->ret_tail) {
        ebr->ret_head = node;
        ebr->ret_tail = node;
    } else {
        ebr->ret_tail->next = node;
        ebr->ret_tail = node;
    }
    atomic_fetch_add_explicit(&ebr->pending_retired, 1, memory_order_release);
    return true;
}

size_t vx_ebr_reclaim(vx_ebr_t *ebr) {
    if (!ebr || !ebr->ret_head) {
        return 0;
    }

    uint64_t min_active = VX_EBR_INACTIVE;
    for (size_t i = 0; i < ebr->max_readers; i++) {
        uint64_t pe = atomic_load_explicit(&ebr->readers[i].pinned_epoch,
                                           memory_order_seq_cst);
        if (pe < min_active) {
            min_active = pe;
        }
    }

    size_t reclaimed = 0;
    vx_ebr_node_t *prev = NULL;
    vx_ebr_node_t *cur = ebr->ret_head;

    while (cur) {
        vx_ebr_node_t *next = cur->next;
        if (cur->retire_epoch <= min_active) {
            if (prev) {
                prev->next = next;
            } else {
                ebr->ret_head = next;
            }
            if (cur == ebr->ret_tail) {
                ebr->ret_tail = prev;
            }
            cur->free_fn(cur->ptr, cur->user_ctx);
            free(cur);
            reclaimed++;
        } else {
            prev = cur;
        }
        cur = next;
    }

    if (reclaimed > 0) {
        atomic_fetch_sub_explicit(&ebr->pending_retired, reclaimed, memory_order_release);
        atomic_fetch_add_explicit(&ebr->total_reclaimed, reclaimed, memory_order_release);
    }
    return reclaimed;
}

void *vx_ebr_current_root(const vx_ebr_t *ebr) {
    if (!ebr) {
        return NULL;
    }
    return atomic_load_explicit(&ebr->root, memory_order_acquire);
}

vx_ebr_stats_t vx_ebr_stats(const vx_ebr_t *ebr) {
    vx_ebr_stats_t st = {0};
    if (!ebr) {
        return st;
    }
    st.epoch = atomic_load_explicit(&ebr->epoch, memory_order_acquire);
    st.pending_retired = atomic_load_explicit(&ebr->pending_retired, memory_order_acquire);
    st.total_reclaimed = atomic_load_explicit(&ebr->total_reclaimed, memory_order_acquire);
    return st;
}

