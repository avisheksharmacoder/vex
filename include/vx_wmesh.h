#ifndef VX_WMESH_H
#define VX_WMESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_OP_INLINE_MAX 32

typedef enum {
    VX_OP_NOP    = 0,
    VX_OP_INSERT = 1,
    VX_OP_UPSERT = 2,
    VX_OP_UPDATE = 3,
    VX_OP_DELETE = 4
} vx_op_type_t;

/* Fixed 64-byte write operation (1 CPU cache line) */
typedef struct {
    uint64_t seq;      /* populated by vx_wmesh_submit / try_submit */
    uint64_t key;
    uint32_t table_id;
    uint16_t op_type;
    uint16_t flags;
    uint32_t len;
    uint32_t _pad;
    uint8_t  data[VX_OP_INLINE_MAX];
} vx_write_op_t;

_Static_assert(sizeof(vx_write_op_t) == 64, "vx_write_op_t must be 64 bytes");

typedef void (*vx_wmesh_commit_fn)(const vx_write_op_t *ops, size_t count,
                                   uint64_t batch_lsn, void *user_ctx);

typedef struct {
    size_t             max_producers;     /* >= 1 */
    size_t             ring_cap_pow2;     /* power of 2 >= 2 */
    size_t             batch_max_ops;     /* count trigger, >= 1 */
    uint32_t           flush_interval_us; /* latency trigger, >= 100 */
    vx_wmesh_commit_fn commit_fn;         /* required */
    void              *user_ctx;
} vx_wmesh_cfg_t;

typedef struct {
    uint64_t ops_committed;
    uint64_t batches_committed;
    uint64_t last_lsn;
    uint64_t backpressure_waits;
} vx_wmesh_stats_t;

typedef struct vx_wmesh    vx_wmesh_t;
typedef struct vx_wmesh_tx vx_wmesh_tx_t;

vx_wmesh_t      *vx_wmesh_create(const vx_wmesh_cfg_t *cfg);
void             vx_wmesh_destroy(vx_wmesh_t *mesh);
vx_wmesh_tx_t   *vx_wmesh_register(vx_wmesh_t *mesh);
/* Releases a producer SPSC slot back to the pool so ephemeral writer threads can recycle it */
void             vx_wmesh_unregister(vx_wmesh_tx_t *tx);

/* Non-blocking push; returns false if this producer's ring is full */
bool             vx_wmesh_try_submit(vx_wmesh_tx_t *tx, const vx_write_op_t *op,
                                     uint64_t *out_token);

/* Blocking push with backpressure until ring space opens or mesh shuts down */
bool             vx_wmesh_submit(vx_wmesh_tx_t *tx, const vx_write_op_t *op,
                                 uint64_t *out_token);

/* Read-Your-Own-Writes / sync durability barrier for a specific producer token */
bool             vx_wmesh_wait(vx_wmesh_tx_t *tx, uint64_t token);

/* Global barrier: waits until all ops currently in all rings are committed */
void             vx_wmesh_flush(vx_wmesh_t *mesh);

vx_wmesh_stats_t vx_wmesh_stats(const vx_wmesh_t *mesh);

#endif /* VX_WMESH_H */

