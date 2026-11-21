#ifndef VX_INTERNAL_H
#define VX_INTERNAL_H

#include "vx_ast.h"
#include "vx_chunk.h"
#include "vx_db.h"
#include "vx_delmap.h"
#include "vx_ebr.h"
#include "vx_log.h"
#include "vx_rhmap.h"
#include "vx_wal.h"
#include "vx_wmesh.h"

#include <pthread.h>
#include <stdatomic.h>

#define VX_PK_SHARDS       128U
#define VX_MAX_TABLES      64U
#define VX_MAX_COLS        64U
#define VX_MAX_WORKERS     32U
#define VX_MAX_INDEXES     16U
#define VX_WAL_OP_QUERY    1U
#define VX_WAL_OP_PUT_ROW  2U
#define VX_WAL_OP_QUERY_PARAMS 3U
#define VX_LOG_EV_BATCH_COMMIT 1U
#define VX_LOG_EV_WRITE_STMT   2U

typedef struct {
    bool     is_unique;
    uint32_t ncols;
    uint32_t col_idxs[16];
} vx_index_def_t;

typedef struct {
    char      name[VX_COL_NAME_MAX];
    vx_type_t type;
    bool      is_key;
    bool      has_fk;
    char      fk_table[VX_COL_NAME_MAX];
    char      fk_col[VX_COL_NAME_MAX];
} vx_col_schema_t;

typedef struct {
    atomic_uint_fast32_t refcnt;
    vx_chunk_t          *chunk;
} vx_chunk_ref_t;

typedef struct {
    atomic_uint_fast32_t refcnt;
    vx_rhmap_t           map;
} vx_rhmap_ref_t;

/* 128-way sharded COW Robin Hood index: mutating 1 key clones only ~1/128th of the map */
typedef struct {
    atomic_uint_fast32_t refcnt;
    vx_rhmap_ref_t      *shards[VX_PK_SHARDS];
} vx_pk_index_t;

typedef struct {
    char             name[VX_COL_NAME_MAX];
    uint32_t         table_id;
    uint32_t         ncols;
    int32_t          pk_col; /* -1 if none */
    vx_col_schema_t  cols[VX_MAX_COLS];
    uint32_t         nchunks;
    uint32_t         chunk_cap;
    vx_chunk_ref_t **chunks;
    vx_delmap_t    **delmaps;
    vx_pk_index_t   *pk_index;
    uint32_t         nindexes;
    vx_index_def_t   indexes[VX_MAX_INDEXES];
    uint64_t         total_rows;
} vx_table_t;

typedef struct {
    uint32_t     ntables;
    uint32_t     next_table_id;
    vx_table_t **tables;
} vx_catalog_t;

typedef struct {
    char      table[VX_COL_NAME_MAX];
    uint32_t  ncols;
    bool      upsert;
    char      col_names[VX_MAX_COLS][VX_COL_NAME_MAX];
    vx_val_t  vals[VX_MAX_COLS];
    char     *str_buf;
} vx_pending_put_t;

typedef struct vx_pool_job vx_pool_job_t;
typedef void (*vx_pool_fn)(uint32_t worker_id, void *ctx);

typedef struct {
    uint32_t        nworkers;
    pthread_t       threads[VX_MAX_WORKERS];
    pthread_mutex_t run_mu;
    pthread_mutex_t dispatch_mu;
    pthread_mutex_t mu;
    pthread_cond_t  cv_work;
    pthread_cond_t  cv_done;
    vx_pool_fn      fn;
    void           *ctx;
    uint64_t        gen;
    uint32_t        done_count;
    bool            stop;
} vx_pool_t;

struct vx_db {
    vx_db_cfg_t          cfg;
    vx_wal_t            *wal;
    vx_ebr_t            *ebr;
    vx_pool_t            pool;

    pthread_mutex_t      write_mu;
    uint64_t             next_lsn;
    uint64_t             next_chunk_id;

    vx_wmesh_t          *wmesh;
    pthread_key_t        wmesh_tx_key;
    vx_wmesh_tx_t       *fallback_wtx;
    pthread_mutex_t      fallback_wtx_mu;

    vx_logger_t         *logger;
    vx_log_tx_t         *wmesh_log_tx;
    vx_log_tx_t         *write_log_tx;

    pthread_key_t        reader_key;

    atomic_uint_fast64_t last_wal_lsn;

    atomic_uint_fast64_t queries_executed;
    atomic_uint_fast64_t mutations_committed;
    atomic_uint_fast64_t chunks_pruned;

    char                *wal_path;
    uint64_t             replay_min_lsn;
    atomic_uint_fast64_t checkpoints;
    atomic_uint_fast64_t dead_rows;
};

/* Internal helpers shared between vx_db.c and vx_exec.c */
vx_chunk_ref_t *vx_chunk_ref_new(vx_chunk_t *ch);
vx_chunk_ref_t *vx_chunk_ref_inc(vx_chunk_ref_t *r);
void            vx_chunk_ref_dec(vx_chunk_ref_t *r);

vx_table_t     *vx_table_clone_shallow(const vx_table_t *src);
void            vx_table_free(vx_table_t *t);
vx_catalog_t   *vx_catalog_clone(const vx_catalog_t *src);
void            vx_catalog_free(vx_catalog_t *cat);
void            vx_catalog_ebr_free(void *ptr, void *user_ctx);

vx_table_t     *vx_catalog_find(const vx_catalog_t *cat, const char *name, size_t len);
int32_t         vx_table_find_col(const vx_table_t *t, const char *name, size_t len);
bool            vx_table_append_row_cow(vx_table_t *t, const vx_val_t *vals,
                                        uint32_t chunk_rows, uint64_t *next_chunk_id);

vx_ebr_reader_t *vx_db_get_reader(vx_db_t *db);
void             vx_db_flush_locked(vx_db_t *db);
bool             vx_db_wal_log(vx_db_t *db, uint16_t op_type, uint32_t table_id,
                               const uint8_t *payload, uint32_t len);

void             vx_pool_run(vx_pool_t *pool, vx_pool_fn fn, void *ctx);

/* Executor entrypoint (vx_exec.c) */
vx_result_t *vx_exec_program(vx_db_t *db, const vx_program_t *prog,
                             const char *raw_query, size_t raw_len,
                             const vx_param_t *params, size_t nparams,
                             bool sync, bool replaying);

vx_result_t *vx_result_new_err(const char *msg);
vx_result_t *vx_result_new_ok(uint64_t rows_affected);

bool vx_table_upsert_row_cow(vx_table_t *t, const vx_val_t *vals,
                             uint32_t chunk_rows, uint64_t *next_chunk_id);
bool vx_db_wal_log_query_params(vx_db_t *db, const char *query, size_t query_len,
                                const vx_param_t *params, size_t nparams);

vx_pk_index_t *vx_pk_index_create(void);
vx_pk_index_t *vx_pk_index_inc(vx_pk_index_t *ix);
void           vx_pk_index_dec(vx_pk_index_t *ix);
bool           vx_pk_index_get(const vx_pk_index_t *ix, uint64_t key, uint64_t *out_packed);
bool           vx_pk_index_put_cow(vx_pk_index_t **ix_ptr, uint64_t key, uint64_t packed);
bool           vx_pk_index_del_cow(vx_pk_index_t **ix_ptr, uint64_t key, uint64_t *out_packed);

bool vx_table_tombstone_row_cow(vx_table_t *t, uint32_t chunk_idx, uint32_t row_idx);
bool vx_val_to_pk_u64(const vx_val_t *v, vx_type_t pk_type, uint64_t *out_key);

bool vx_snap_write(const char *snap_path, const vx_catalog_t *cat, uint64_t checkpoint_lsn);
bool vx_snap_load(const char *snap_path, vx_catalog_t **out_cat, uint64_t *out_lsn,
                  uint32_t chunk_rows, uint64_t *next_chunk_id);

bool     vx_table_check_unique(const vx_table_t *t, const vx_val_t *row,
                               int64_t skip_ci, int64_t skip_ri);
uint64_t vx_catalog_dead_rows(const vx_catalog_t *cat);

#endif /* VX_INTERNAL_H */
