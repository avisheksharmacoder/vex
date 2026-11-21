#ifndef VX_EBR_H
#define VX_EBR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*vx_ebr_free_fn)(void *ptr, void *user_ctx);

typedef struct vx_ebr        vx_ebr_t;
typedef struct vx_ebr_reader vx_ebr_reader_t;

typedef struct {
    uint64_t epoch;
    size_t   pending_retired;
    uint64_t total_reclaimed;
} vx_ebr_stats_t;

/* Caller retains ownership of the live root; retired roots are freed via retire's free_fn */
vx_ebr_t        *vx_ebr_create(size_t max_readers, void *initial_root);
void             vx_ebr_destroy(vx_ebr_t *ebr);

vx_ebr_reader_t *vx_ebr_register_reader(vx_ebr_t *ebr);
/* Releases a reader slot back to the pool so ephemeral threads can recycle it */
void             vx_ebr_unregister_reader(vx_ebr_reader_t *reader);

/* Lock-free MVCC read critical section: pins reader epoch and returns current root */
void            *vx_ebr_enter(vx_ebr_reader_t *reader);
void             vx_ebr_exit(vx_ebr_reader_t *reader);

/* Single-writer MVCC snapshot publish: swaps root, advances epoch, returns previous root */
void            *vx_ebr_publish(vx_ebr_t *ebr, void *new_root);

/* Queues old_ptr for deferred reclamation once all readers advance past current epoch */
bool             vx_ebr_retire(vx_ebr_t *ebr, void *old_ptr,
                               vx_ebr_free_fn free_fn, void *user_ctx);

/* Reclaims all retired pointers safe to free now; returns number reclaimed */
size_t           vx_ebr_reclaim(vx_ebr_t *ebr);

void            *vx_ebr_current_root(const vx_ebr_t *ebr);
vx_ebr_stats_t   vx_ebr_stats(const vx_ebr_t *ebr);

#endif /* VX_EBR_H */

