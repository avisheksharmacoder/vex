#ifndef VX_DB_H
#define VX_DB_H

#include "vx_chunk.h"
#include "vx_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_COL_NAME_MAX 64U

typedef struct vx_db vx_db_t;

typedef struct {
    uint32_t worker_threads;    /* morsel query workers (0 -> default 4) */
    uint32_t chunk_rows;        /* rows per immutable columnar chunk (0 -> 4096) */
    size_t   max_readers;       /* concurrent MVCC readers (0 -> 64) */
    uint32_t flush_interval_us; /* write-mesh micro-batch latency trigger (0 -> 2000) */
    bool     fsync_on_commit;   /* fdatasync WAL on every committed batch */
    vx_log_sink_fn log_sink;    /* optional custom sink for async SPSC binary logger */
    void          *log_ctx;     /* user context passed to log_sink */
} vx_db_cfg_t;

typedef struct {
    const char *name;           /* parameter name without leading '$' */
    vx_val_t    val;
} vx_param_t;

/* Contiguous 64-byte-aligned result column (zero-copy ready for Python Buffer Protocol) */
typedef struct {
    char       name[VX_COL_NAME_MAX];
    vx_type_t  type;
    void      *data;            /* int64_t*, uint64_t*, double*, uint8_t*, or vx_str_t* */
    uint64_t  *null_bits;       /* bit == 1 means NULL */
} vx_result_col_t;

typedef struct {
    bool             ok;
    char             err_msg[192];
    uint64_t         rows_affected;
    uint32_t         nrows;
    uint32_t         ncols;
    vx_result_col_t *cols;
    vx_arena_t       str_arena;
    char            *explain_text;
} vx_result_t;

typedef struct {
    uint64_t queries_executed;
    uint64_t mutations_committed;
    uint64_t wal_lsn;
    uint64_t chunks_pruned;
    uint64_t logs_emitted;
    uint64_t logs_dropped;
    uint64_t checkpoints;
    uint64_t dead_rows;         /* live tombstones currently in RAM across all tables */
} vx_db_stats_t;

vx_db_cfg_t  vx_db_default_cfg(void);

/* wal_path == NULL opens an in-memory database; otherwise opens/recovers WAL */
vx_db_t     *vx_db_open(const char *wal_path, const vx_db_cfg_t *cfg);
void         vx_db_close(vx_db_t *db);
void         vx_db_flush(vx_db_t *db);

/* Writes atomic binary snapshot (<wal_path>.snap), compacts tombstones, and
 * truncates the WAL. Returns false on in-memory DBs or I/O failure. */
bool          vx_db_checkpoint(vx_db_t *db);

/* Executes 1..N Vex statements (DDL, mutations, or queries).
 * sync == true enforces Read-Your-Own-Writes / commit barrier for mutations. */
vx_result_t *vx_db_query(vx_db_t *db, const char *query,
                         const vx_param_t *params, size_t nparams, bool sync);

/* Fast-path structured row insert/upsert (bypasses query parser; used by Python db.put) */
bool         vx_db_put_row(vx_db_t *db, const char *table,
                           const char *const *col_names, const vx_val_t *vals,
                           uint32_t ncols, bool upsert, bool sync);

vx_db_stats_t vx_db_stats(const vx_db_t *db);
void          vx_result_free(vx_result_t *res);

/* Result inspection helpers */
int      vx_result_col_idx(const vx_result_t *res, const char *name);
bool     vx_result_is_null(const vx_result_t *res, uint32_t col, uint32_t row);
int64_t  vx_result_i64(const vx_result_t *res, uint32_t col, uint32_t row);
uint64_t vx_result_u64(const vx_result_t *res, uint32_t col, uint32_t row);
double   vx_result_f64(const vx_result_t *res, uint32_t col, uint32_t row);
bool     vx_result_bool(const vx_result_t *res, uint32_t col, uint32_t row);
vx_str_t vx_result_str(const vx_result_t *res, uint32_t col, uint32_t row);

#endif /* VX_DB_H */

