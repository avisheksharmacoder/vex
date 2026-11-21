#ifndef VX_WAL_H
#define VX_WAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_WAL_MAX_PAYLOAD (16U * 1024U * 1024U)

typedef struct {
    uint64_t       lsn;         /* strictly monotonic (> previous LSN) */
    uint32_t       table_id;
    uint16_t       op_type;
    uint16_t       flags;
    uint32_t       payload_len;
    const uint8_t *payload;
} vx_wal_rec_t;

typedef struct {
    uint64_t records_replayed;
    uint64_t last_valid_lsn;
    uint64_t valid_end_offset;
    uint64_t truncated_bytes;
    bool     had_corrupt_tail;
} vx_wal_recovery_t;

typedef bool (*vx_wal_replay_fn)(const vx_wal_rec_t *rec, void *user_ctx);

typedef struct vx_wal vx_wal_t;

uint32_t  vx_crc32(const void *data, size_t len);

/* Opens or creates a deterministic WAL; automatically heals/truncates corrupt tail */
vx_wal_t *vx_wal_open(const char *path, vx_wal_recovery_t *out_recovery);
void      vx_wal_close(vx_wal_t *wal);

/* Appends a batch in one contiguous write; enforces strictly monotonic LSNs */
bool      vx_wal_append_batch(vx_wal_t *wal, const vx_wal_rec_t *recs,
                              size_t count, bool fsync_now);

uint64_t  vx_wal_last_lsn(const vx_wal_t *wal);

/* Atomically truncates WAL back to a fresh 16-byte header after checkpointing
 * while preserving strictly monotonic LSN continuity (new_last_lsn). */
bool      vx_wal_reset(vx_wal_t *wal, uint64_t new_last_lsn);

/* Read-only replay with CRC32 & torn-write verification (does not modify file) */
bool      vx_wal_replay(const char *path, vx_wal_replay_fn cb, void *user_ctx,
                        vx_wal_recovery_t *out_info);

#endif /* VX_WAL_H */

