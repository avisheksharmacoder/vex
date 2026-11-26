#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "vx_manifest.h"

#ifndef VX_ERR_CONFLICT
#define VX_ERR_CONFLICT ((vx_status_t)-9)
#endif

#define VX_WAL_MAGIC             "VXW2"
#define VX_WAL_OP_QUERY          1u
#define VX_WAL_OP_PUT_ROW        2u
#define VX_WAL_OP_QUERY_PARAMS   3u
#define VX_WAL_OP_TX_BATCH       4u /* M17: framed multi-statement atomic batch */

#define VX_WAL_FLAG_SYNC         (1u << 0)
#define VX_TX_MAX_BATCH_BYTES    (256u * 1024u * 1024u) /* 256 MiB hard framing cap */

/* --- 32-byte Outer WAL Frame Header (app.wal) ---
 * Wire layout: [vx_wal_rec_hdr_t (32B)] [payload (payload_len B)]
 * crc32 covers hdr[0..24) || payload[0..payload_len). */
typedef struct vx_wal_rec_hdr {
    uint8_t  magic[4];    /* "VXW2" */
    uint32_t op_type;     /* VX_WAL_OP_* */
    uint64_t lsn;         /* strictly monotonic commit LSN */
    uint32_t payload_len; /* bytes immediately following header */
    uint32_t flags;       /* VX_WAL_FLAG_* */
    uint32_t crc32;       /* IEEE 802.3 CRC32 */
    uint32_t _pad;
} vx_wal_rec_hdr_t;
_Static_assert(sizeof(vx_wal_rec_hdr_t) == 32, "vx_wal_rec_hdr_t must be 32 bytes");
_Static_assert(offsetof(vx_wal_rec_hdr_t, crc32) == 24, "vx_wal_rec_hdr_t crc offset mismatch");

/* --- 16-byte Payload Header for VX_WAL_OP_TX_BATCH ---
 * Followed by n_ops x [vx_tx_subop_hdr_t (8B) | data (len B, zero-padded to 8B)]. */
typedef struct vx_tx_wal_hdr {
    uint64_t tx_id;
    uint32_t n_ops;
    uint32_t _reserved;
} vx_tx_wal_hdr_t;
_Static_assert(sizeof(vx_tx_wal_hdr_t) == 16, "vx_tx_wal_hdr_t must be 16 bytes");

typedef struct vx_tx_subop_hdr {
    uint32_t op_type;     /* VX_WAL_OP_QUERY / PUT_ROW / QUERY_PARAMS */
    uint32_t data_len;    /* raw payload bytes (padded to 8B boundary on wire) */
} vx_tx_subop_hdr_t;
_Static_assert(sizeof(vx_tx_subop_hdr_t) == 8, "vx_tx_subop_hdr_t must be 8 bytes");

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_tx_state : uint8_t {
    VX_TX_ACTIVE    = 0,
    VX_TX_COMMITTED = 1,
    VX_TX_ABORTED   = 2
} vx_tx_state_t;

/* Narrow DB callback bridge implemented in vx_db.c */
typedef struct vx_tx_db_ops {
    /* Drains SPSC write mesh so published_cat reflects all prior fast-path writes */
    void         (*drain_wmesh)(void *db_ctx);

    /* Retains current published catalog (VX_OWN_CATALOG) and returns logical commit seq */
    vx_owner_t  *(*acquire_snapshot)(void *db_ctx, uint64_t *out_commit_seq);

    /* Applies one encoded mutation (QUERY / PUT_ROW / QUERY_PARAMS) onto COW fork `in_cat`,
     * returning a new retained catalog `*out_next_cat` without modifying `in_cat`.
     * Sets *out_table_bit (1ULL << (table_id & 63)) and *out_hot_bytes_added. */
    vx_status_t  (*apply_cow_op)(void *db_ctx, vx_owner_t *in_cat,
                                 uint32_t op_type, const uint8_t *payload, uint32_t payload_len,
                                 vx_owner_t **out_next_cat, uint64_t *out_table_bit,
                                 size_t *out_hot_bytes_added);

    /* Backpressure hooks (called OUTSIDE write_mu) */
    vx_status_t  (*reserve_hot)(void *db_ctx, size_t add_bytes);
    void         (*release_hot)(void *db_ctx, size_t sub_bytes);

    /* Commit critical section helpers (all called WHILE holding db->write_mu) */
    vx_owner_t  *(*current_published)(void *db_ctx, uint64_t *out_commit_seq);
    bool         (*has_write_conflict)(void *db_ctx, uint64_t base_commit_seq,
                                       uint64_t read_mask, uint64_t write_mask);
    uint64_t     (*next_lsn)(void *db_ctx);
    void         (*publish_ebr)(void *db_ctx, vx_owner_t *new_cat,
                                uint64_t new_lsn, uint64_t write_mask);
} vx_tx_db_ops_t;

typedef struct vx_tx {
    void                 *db_ctx;
    const vx_tx_db_ops_t *ops;
    pthread_mutex_t      *write_mu;
    int                   wal_fd;
    bool                  wal_sync;

    pthread_mutex_t       tx_mu;        /* guards wrapper access under 3.13t free-threading */
    vx_tx_state_t         state;
    uint64_t              tx_id;
    uint64_t              base_commit_seq;

    vx_owner_t           *base_cat;     /* retained starting snapshot */
    vx_owner_t           *staged_cat;   /* retained private COW fork (RYOW target) */

    uint64_t              read_tables_mask;
    uint64_t              write_tables_mask;
    size_t                hot_bytes_delta;

    /* Pre-framed VX_WAL_OP_TX_BATCH payload buffer: [vx_tx_wal_hdr_t | subops...] */
    uint8_t              *batch_buf;
    size_t                batch_len;
    size_t                batch_cap;
    uint32_t              n_ops;
} vx_tx_t;

/* --- Transaction Lifecycle API --- */
vx_status_t vx_tx_begin(void *db_ctx, const vx_tx_db_ops_t *ops, pthread_mutex_t *write_mu,
                        int wal_fd, bool wal_sync, uint64_t tx_id, vx_tx_t **out_tx);

/* Retains tx->staged_cat for read queries inside the transaction (records table read mask). */
vx_owner_t *vx_tx_get_read_catalog(vx_tx_t *tx, uint64_t read_table_bit);

static inline vx_owner_t *vx_tx_read_snapshot(vx_tx_t *tx) {
    return vx_tx_get_read_catalog(tx, 0ULL);
}

static inline void vx_tx_record_read(vx_tx_t *tx, uint64_t mask) {
    if (!tx) return;
    pthread_mutex_lock(&tx->tx_mu);
    tx->read_tables_mask |= mask;
    pthread_mutex_unlock(&tx->tx_mu);
}

/* Stages one mutation (db.q mutation or db.put) onto tx->staged_cat and batch_buf.
 * On constraint/schema/OOM error, tx->staged_cat and batch_buf stay unchanged. */
vx_status_t vx_tx_stage_op(vx_tx_t *tx, uint32_t op_type,
                           const uint8_t *payload, uint32_t payload_len);

/* Validates snapshot (or rebases if only background flush / disjoint write occurred),
 * writes single atomic VX_WAL_OP_TX_BATCH frame, and publishes via EBR root swap. */
vx_status_t vx_tx_commit(vx_tx_t *tx, uint64_t *out_commit_lsn);

/* Discards staged catalog & WAL buffer with zero disk I/O (idempotent). */
void        vx_tx_rollback(vx_tx_t *tx);
void        vx_tx_destroy(vx_tx_t *tx);

/* --- Framed WAL I/O & M13 Recovery Hooks --- */
vx_status_t vx_wal_append_frame(int wal_fd, uint64_t lsn, uint32_t op_type,
                                const uint8_t *payload, uint32_t payload_len, bool sync_disk);

/* M13 vx_wal_next_fn implementation decoding framed CRC32 WAL records (single & TX_BATCH). */
typedef struct vx_wal_reader {
    uint8_t *scratch;
    size_t   scratch_cap;
} vx_wal_reader_t;

void        vx_wal_reader_destroy(vx_wal_reader_t *r);
vx_status_t vx_wal_read_next_frame(void *wal_ctx, int wal_fd, uint64_t *io_off,
                                   vx_wal_frame_t *out_frame, bool *out_eof);

/* Replays a single VX_WAL_OP_TX_BATCH frame atomically during vex.open() recovery. */
vx_status_t vx_tx_replay_batch(void *db_ctx, const vx_tx_db_ops_t *ops,
                               const vx_wal_frame_t *frame);
