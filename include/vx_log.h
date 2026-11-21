#ifndef VX_LOG_H
#define VX_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VX_LOG_DEBUG = 0,
    VX_LOG_INFO  = 1,
    VX_LOG_WARN  = 2,
    VX_LOG_ERROR = 3
} vx_log_level_t;

/* Fixed 64-byte binary log event (1 CPU cache line, zero hot-path formatting) */
typedef struct {
    uint64_t    ts_ns;
    uint32_t    tid;
    uint16_t    event_id;
    uint8_t     level;
    uint8_t     _pad;
    uint64_t    a0;
    uint64_t    a1;
    uint64_t    a2;
    uint64_t    a3;
    const char *tag;   /* static literal pointer only; never freed by logger */
    uint64_t    seq;   /* producer-local sequence */
} vx_log_rec_t;

_Static_assert(sizeof(vx_log_rec_t) == 64, "vx_log_rec_t must be 64 bytes");

typedef void (*vx_log_sink_fn)(const vx_log_rec_t *recs, size_t count, void *user_ctx);

typedef struct {
    size_t         max_producers;     /* >= 1 */
    size_t         ring_cap_pow2;     /* power of 2 >= 2 */
    vx_log_level_t min_level;
    uint32_t       flush_interval_us; /* e.g., 2000 (2ms) */
    vx_log_sink_fn sink;              /* required */
    void          *user_ctx;
} vx_log_cfg_t;

typedef struct {
    uint64_t emitted;
    uint64_t dropped;
    uint64_t flushes;
} vx_log_stats_t;

typedef struct vx_logger vx_logger_t;
typedef struct vx_log_tx vx_log_tx_t;

vx_logger_t   *vx_log_create(const vx_log_cfg_t *cfg);
void           vx_log_destroy(vx_logger_t *log);
vx_log_tx_t   *vx_log_register(vx_logger_t *log, uint32_t tid);

/* Strictly non-blocking: returns false and increments dropped counter if full.
 * Records below min_level are ignored and return true. */
bool           vx_log_emit(vx_log_tx_t *tx, vx_log_level_t level,
                           uint16_t event_id, const char *tag,
                           uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

void           vx_log_flush(vx_logger_t *log);
vx_log_stats_t vx_log_stats(const vx_logger_t *log);

#endif /* VX_LOG_H */

