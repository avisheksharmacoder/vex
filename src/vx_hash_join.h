#pragma once
#include "vx_op.h"

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_join_type : uint8_t {
    VX_JOIN_INNER = 0, /* Vex `+`  */
    VX_JOIN_LEFT  = 1, /* Vex `++` (probe = left, build = right) */
    VX_JOIN_SEMI  = 2, /* Vex `&`  (probe rows with >= 1 build match, zero-copy) */
    VX_JOIN_ANTI  = 3  /* probe rows with 0 build matches, zero-copy */
} vx_join_type_t;

typedef struct vx_join_key {
    vx_type_t type;
    uint32_t  batch_col_idx;
    uint32_t  table_col_idx;
} vx_join_key_t;

/* --- Build Sink Configuration --- */
typedef struct vx_hj_build_cfg {
    const vx_join_key_t *keys;
    uint32_t             nkeys;
    const uint32_t      *payload_batch_cols; /* build columns to retain (include keys if projected) */
    const uint32_t      *payload_table_cols;
    const vx_type_t     *payload_types;
    uint32_t             n_payload_cols;
} vx_hj_build_cfg_t;

/* --- Immutable Finalized Build Hash Table (read lock-free by all Probe workers) --- */
typedef struct vx_hj_col {
    vx_type_t  type;
    uint8_t    has_nulls;
    uint8_t    _pad[6];
    void      *data;     /* dense array[nrows] */
    uint64_t  *validity; /* NULL => all valid; else 1 = valid, 0 = null */
} vx_hj_col_t;

typedef struct vx_hj_table {
    vx_owner_t     hdr;
    uint32_t       nrows;
    uint32_t       nkeys;
    uint32_t       n_payload_cols;
    uint32_t       bucket_mask; /* (bucket_cap - 1), bucket_cap = power of 2 */

    uint32_t      *head;        /* [bucket_cap]: 0 = empty, else build_row + 1 */
    uint32_t      *next;        /* [nrows]:      0 = end,   else next_build_row + 1 */
    uint64_t      *hashes;      /* [nrows]:      full 64-bit mixed key hash */

    vx_hj_col_t   *key_cols;    /* [nkeys]: non-null join keys (for exact collision check) */
    vx_hj_col_t   *pay_cols;    /* [n_payload_cols]: build payload columns */
    vx_strpool_t   strpool;     /* owns out-of-line string buffers shared by key_cols & pay_cols */
    size_t         bytes_used;  /* M15 Grace Hash Join work_mem accounting */
} vx_hj_table_t;

/* --- Probe Operator Configuration --- */
typedef struct vx_hj_out_col {
    bool      from_build;       /* false = probe input col; true = build pay_cols[src_col_idx] */
    uint32_t  src_col_idx;      /* batch_col_idx on probe OR payload idx on build */
    uint32_t  table_col_idx;    /* used for late materialization when !from_build */
} vx_hj_out_col_t;

typedef struct vx_hj_probe_cfg {
    vx_join_type_t         join_type;
    vx_hj_table_t         *ht;          /* retained by Probe init_global */
    const vx_join_key_t   *probe_keys;
    uint32_t               nkeys;
    const vx_hj_out_col_t *out_cols;
    uint32_t               n_out_cols;
} vx_hj_probe_cfg_t;

/* Transfers ownership of finalized vx_hj_table_t* out of HashJoinBuild gstate */
vx_hj_table_t *vx_hj_build_take(void *sink_gstate);

extern const vx_sink_vtbl_t VX_SINK_HASH_JOIN_BUILD;
extern const vx_op_vtbl_t   VX_OP_HASH_JOIN_PROBE;

