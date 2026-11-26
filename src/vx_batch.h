#pragma once
#include "vx_arena.h"
#include "vx_chunk.h"

typedef struct vx_sel {
    alignas(64) uint32_t idx[VX_BATCH_CAP];
    uint32_t             count;
    bool                 is_identity; /* true => logical rows are [0 .. count-1] densely */
} vx_sel_t;

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_vec_kind : uint8_t {
    VX_VEC_DEFERRED = 0, /* not yet loaded; call vx_batch_materialize_col() */
    VX_VEC_FLAT     = 1, /* dense typed array[0..cap-1] indexed by batch->sel */
    VX_VEC_CONST    = 2, /* single scalar at data[0], validity bit 0 = is_valid */
    VX_VEC_DICT     = 3  /* M15: uint32_t codes[] indexing dict_vals[] */
} vx_vec_kind_t;

typedef struct vx_vec {
    vx_type_t           type;
    vx_vec_kind_t       kind;
    uint8_t             has_nulls;
    uint8_t             _pad[5];
    uint32_t            phys_len;    /* physical element count in data[] */
    uint32_t            dict_size;   /* valid when kind == VX_VEC_DICT */
    const void         *data;        /* FLAT/CONST values, or uint32_t codes[] for DICT */
    const void         *dict_vals;   /* typed dictionary values when kind == VX_VEC_DICT */
    const uint64_t     *validity;    /* NULL => all valid; else 1 = valid, 0 = null */
    const vx_strpool_t *strpool;     /* non-NULL iff type == VX_TYPE_STR */
    vx_owner_t         *owner;       /* pins backing chunk, segment, or scratch arena */
} vx_vec_t;

/* Callback for late materialization from Hot RAM chunk or Cold mmap segment */
typedef struct vx_batch vx_batch_t;
typedef vx_status_t (*vx_fetch_col_fn)(void *src_ctx, uint32_t table_col_idx,
                                       const vx_sel_t *sel, vx_arena_t *scratch,
                                       vx_vec_t *out_vec);

typedef struct vx_row_src {
    void            *src_ctx;        /* e.g. const vx_chunk_t* or M13 segment slice cursor */
    vx_owner_t      *src_owner;      /* retained for lifetime of batch */
    vx_fetch_col_fn  fetch_col;
    uint32_t         row_offset;     /* offset within segment/chunk */
    uint32_t         _pad;
} vx_row_src_t;

struct vx_batch {
    vx_vec_t      *cols;             /* array[ncols] */
    uint32_t       ncols;
    uint32_t       cap_cols;
    vx_sel_t       sel;              /* active row count is always batch->sel.count */
    vx_row_src_t   row_src;          /* non-null fetch_col enables late materialization */
};

/* Lifecycle & materialization */
vx_status_t vx_batch_init(vx_batch_t *b, uint32_t ncols, const vx_type_t *types);
void        vx_batch_destroy(vx_batch_t *b);
void        vx_batch_reset(vx_batch_t *b); /* releases column/source owners, resets sel */

/* Materializes a VX_VEC_DEFERRED column using batch->sel (no-op if already loaded) */
vx_status_t vx_batch_materialize_col(vx_batch_t *b, uint32_t batch_col_idx,
                                     uint32_t table_col_idx, vx_arena_t *scratch);

/* Compacts sparse sel into dense flat vectors allocated from dst_arena (for Result / Sort / Spill) */
vx_status_t vx_batch_compact(const vx_batch_t *in, vx_batch_t *out, vx_arena_t *dst_arena);
vx_status_t vx_vec_compact(const vx_vec_t *in, const vx_sel_t *sel,
                           vx_arena_t *dst_arena, vx_vec_t *out);
void        vx_batch_forward_shallow(const vx_batch_t *in, vx_batch_t *out);

