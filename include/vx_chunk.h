#ifndef VX_CHUNK_H
#define VX_CHUNK_H

#include "vx_arena.h"
#include "vx_str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VX_TYPE_I64  = 1,
    VX_TYPE_U64  = 2,
    VX_TYPE_F64  = 3,
    VX_TYPE_BOOL = 4,
    VX_TYPE_STR  = 5
} vx_type_t;

typedef enum {
    VX_CMP_EQ = 0,
    VX_CMP_NE = 1,
    VX_CMP_LT = 2,
    VX_CMP_LE = 3,
    VX_CMP_GT = 4,
    VX_CMP_GE = 5
} vx_cmp_op_t;

typedef struct {
    vx_type_t type;
    bool      is_null;
    union {
        int64_t  i64;
        uint64_t u64;
        double   f64;
        bool     b;
        struct {
            const char *ptr;
            uint32_t    len;
        } str;
    } v;
} vx_val_t;

typedef struct {
    bool     has_bounds;
    uint32_t null_count;
    union {
        struct { int64_t  min; int64_t  max; } i64;
        struct { uint64_t min; uint64_t max; } u64;
        struct { double   min; double   max; } f64;
    } b;
} vx_zone_t;

typedef struct {
    vx_type_t  type;
    void      *data;       /* 64-byte aligned contiguous column array */
    uint64_t  *null_bits;  /* 64-byte aligned; bit == 1 means NULL */
    vx_zone_t  zone;
} vx_col_t;

typedef struct {
    uint64_t   chunk_id;
    uint32_t   ncols;
    uint32_t   nrows;
    uint32_t   cap;
    vx_col_t  *cols;
    vx_arena_t str_arena;
} vx_chunk_t;

vx_chunk_t *vx_chunk_create(uint64_t chunk_id, uint32_t ncols,
                            const vx_type_t *types, uint32_t cap,
                            size_t str_arena_bytes);
void        vx_chunk_destroy(vx_chunk_t *ch);

/* Appends one typed row and maintains per-column zone maps + null bitmaps */
bool        vx_chunk_push_row(vx_chunk_t *ch, const vx_val_t *vals);

static inline bool vx_col_is_null(const vx_col_t *col, uint32_t row) {
    return (col->null_bits[row >> 6] & (1ULL << (row & 63U))) != 0;
}

/* Zone map pruning: returns false when the entire chunk can be skipped in O(1) */
bool vx_zone_may_match_i64(const vx_zone_t *z, vx_cmp_op_t op, int64_t val);
bool vx_zone_may_match_u64(const vx_zone_t *z, vx_cmp_op_t op, uint64_t val);
bool vx_zone_may_match_f64(const vx_zone_t *z, vx_cmp_op_t op, double val);

/* Branchless SIMD-friendly column filters (NULL rows never match).
 * If null_bits == NULL or has no nulls, takes the pure unmasked SIMD fast path. */
uint32_t vx_filter_i64(const int64_t *col, const uint64_t *null_bits, uint32_t n,
                       vx_cmp_op_t op, int64_t val, uint32_t *sel_out);
uint32_t vx_filter_u64(const uint64_t *col, const uint64_t *null_bits, uint32_t n,
                       vx_cmp_op_t op, uint64_t val, uint32_t *sel_out);
uint32_t vx_filter_f64(const double *col, const uint64_t *null_bits, uint32_t n,
                       vx_cmp_op_t op, double val, uint32_t *sel_out);
uint32_t vx_filter_str_eq(const vx_str_t *col, const uint64_t *null_bits, uint32_t n,
                          const vx_str_t *needle, uint32_t *sel_out);

/* Zone-map-aware chunk filter shortcut */
uint32_t vx_chunk_filter_f64(const vx_chunk_t *ch, uint32_t col_idx,
                             vx_cmp_op_t op, double val, uint32_t *sel_out);

#endif /* VX_CHUNK_H */

