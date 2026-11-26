#pragma once
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vx_op.h"

#define VX_AGG_PARTITIONS 64u
#define VX_AGG_PART_SHIFT 58u /* top 6 bits of 64-bit mixed hash */

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_agg_kind : uint8_t {
    VX_AGG_COUNT_STAR = 0, /* count() — touches no input column */
    VX_AGG_COUNT      = 1, /* count(col) — skips NULL */
    VX_AGG_SUM        = 2, /* exact i64 / u64 / Neumaier f64 */
    VX_AGG_MIN        = 3,
    VX_AGG_MAX        = 4
} vx_agg_kind_t;

typedef struct vx_key_spec {
    vx_type_t type;
    uint32_t  batch_col_idx;
    uint32_t  table_col_idx;
} vx_key_spec_t;

typedef struct vx_agg_spec {
    vx_agg_kind_t kind;
    vx_type_t     type;          /* input column type (unused for COUNT_STAR) */
    uint32_t      batch_col_idx;
    uint32_t      table_col_idx;
} vx_agg_spec_t;

typedef struct vx_hash_agg_cfg {
    const vx_key_spec_t *keys;
    uint32_t             nkeys;  /* 0 => global ungrouped aggregation */
    const vx_agg_spec_t *aggs;
    uint32_t             naggs;
} vx_hash_agg_cfg_t;

typedef struct vx_key_val {
    uint8_t is_null;
    uint8_t _pad[7];
    union {
        int64_t      i64;
        uint64_t     u64;
        double       f64;
        uint8_t      b;
        vx_strview_t str;        /* backed by owning vx_agg_ht_t.strpool */
    } v;
} vx_key_val_t;

typedef struct vx_agg_state {
    uint64_t count;              /* valid non-null count, or total rows for COUNT_STAR */
    union {
        int64_t      i64;
        uint64_t     u64;
        double       f64;
        struct { double sum; double comp; } f64_sum;
        vx_strview_t str;
    } v;
} vx_agg_state_t;

typedef struct vx_agg_bucket {
    uint64_t hash;
    uint32_t gid_plus1;          /* 0 = empty; else group index + 1 */
    uint32_t _pad;
} vx_agg_bucket_t;

typedef struct vx_agg_ht {
    vx_agg_bucket_t *buckets;
    uint32_t         cap;        /* power of 2 */
    uint32_t         ngroups;
    uint32_t         cap_groups;
    uint32_t         nkeys;
    uint32_t         naggs;

    uint64_t        *hashes;     /* [ngroups] */
    vx_key_val_t    *keys;       /* [ngroups * nkeys] */
    vx_agg_state_t  *aggs;       /* [ngroups * naggs] */
    vx_strpool_t     strpool;    /* owns out-of-line key & min/max string bytes */
} vx_agg_ht_t;

typedef struct vx_agg_part {
    pthread_mutex_t mu;
    vx_agg_ht_t     ht;
} vx_agg_part_t;

typedef struct vx_agg_gstate {
    const vx_hash_agg_cfg_t *cfg;
    vx_agg_part_t            parts[VX_AGG_PARTITIONS];
} vx_agg_gstate_t;

typedef struct vx_agg_lstate {
    const vx_hash_agg_cfg_t *cfg;
    vx_agg_ht_t              ht;
    vx_key_val_t            *tmp_key; /* scratch key buffer of length nkeys */
} vx_agg_lstate_t;

extern const vx_sink_vtbl_t VX_SINK_HASH_AGG;

