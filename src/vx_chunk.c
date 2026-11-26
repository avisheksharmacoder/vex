#include "../include/vx_chunk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../include/vx_simd.h"

#define VX_COL_ALIGN 64U

static size_t vx_type_size(vx_type_t t) {
    switch (t) {
        case VX_TYPE_I64:  return sizeof(int64_t);
        case VX_TYPE_U64:  return sizeof(uint64_t);
        case VX_TYPE_F64:  return sizeof(double);
        case VX_TYPE_BOOL: return sizeof(uint8_t);
        case VX_TYPE_STR:  return sizeof(vx_str_t);
        default:           return 0;
    }
}

static void *alloc_aligned_zero(size_t bytes) {
    if (bytes < VX_COL_ALIGN) {
        bytes = VX_COL_ALIGN;
    }
    void *p = NULL;
    if (posix_memalign(&p, VX_COL_ALIGN, bytes) != 0 || !p) {
        return NULL;
    }
    memset(p, 0, bytes);
    return p;
}

vx_chunk_t *vx_chunk_create(uint64_t chunk_id, uint32_t ncols,
                            const vx_type_t *types, uint32_t cap,
                            size_t str_arena_bytes) {
    if (ncols == 0 || !types || cap == 0) {
        return NULL;
    }
    for (uint32_t c = 0; c < ncols; c++) {
        if (vx_type_size(types[c]) == 0) {
            return NULL;
        }
    }

    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(vx_chunk_t));
    if (!ch) {
        return NULL;
    }
    ch->chunk_id = chunk_id;
    ch->ncols = ncols;
    ch->nrows = 0;
    ch->cap = cap;

    size_t arena_cap = str_arena_bytes ? str_arena_bytes : 64U;
    if (!vx_arena_init(&ch->str_arena, arena_cap)) {
        free(ch);
        return NULL;
    }

    ch->cols = (vx_col_t *)calloc(ncols, sizeof(vx_col_t));
    if (!ch->cols) {
        vx_arena_destroy(&ch->str_arena);
        free(ch);
        return NULL;
    }

    size_t null_words = ((size_t)cap + 63U) / 64U;
    size_t null_bytes = null_words * sizeof(uint64_t);

    for (uint32_t c = 0; c < ncols; c++) {
        size_t esz = vx_type_size(types[c]);
        if ((size_t)cap > SIZE_MAX / esz) {
            vx_chunk_destroy(ch);
            return NULL;
        }
        ch->cols[c].type = types[c];
        ch->cols[c].data = alloc_aligned_zero((size_t)cap * esz);
        ch->cols[c].null_bits = (uint64_t *)alloc_aligned_zero(null_bytes);
        if (!ch->cols[c].data || !ch->cols[c].null_bits) {
            vx_chunk_destroy(ch);
            return NULL;
        }
    }

    return ch;
}

void vx_chunk_destroy(vx_chunk_t *ch) {
    if (!ch) {
        return;
    }
    if (ch->cols) {
        for (uint32_t c = 0; c < ch->ncols; c++) {
            free(ch->cols[c].data);
            free(ch->cols[c].null_bits);
        }
        free(ch->cols);
    }
    vx_arena_destroy(&ch->str_arena);
    free(ch);
}

bool vx_chunk_push_row(vx_chunk_t *ch, const vx_val_t *vals) {
    if (!ch || !vals || ch->nrows >= ch->cap) {
        return false;
    }

    for (uint32_t c = 0; c < ch->ncols; c++) {
        if (vals[c].type != ch->cols[c].type) {
            return false;
        }
    }

    uint32_t r = ch->nrows;
    size_t saved_arena_off = ch->str_arena.offset;

    /* Pre-materialize string columns so arena OOM leaves chunk untouched */
    for (uint32_t c = 0; c < ch->ncols; c++) {
        if (ch->cols[c].type == VX_TYPE_STR && !vals[c].is_null) {
            vx_str_t *dst = &((vx_str_t *)ch->cols[c].data)[r];
            if (!vx_str_make(&ch->str_arena, vals[c].v.str.ptr, vals[c].v.str.len, dst)) {
                ch->str_arena.offset = saved_arena_off;
                return false;
            }
        }
    }

    for (uint32_t c = 0; c < ch->ncols; c++) {
        vx_col_t *col = &ch->cols[c];
        if (vals[c].is_null) {
            col->null_bits[r >> 6] |= (1ULL << (r & 63U));
            col->zone.null_count++;
            continue;
        }

        col->null_bits[r >> 6] &= ~(1ULL << (r & 63U));
        switch (col->type) {
            case VX_TYPE_I64: {
                int64_t v = vals[c].v.i64;
                ((int64_t *)col->data)[r] = v;
                if (!col->zone.has_bounds) {
                    col->zone.has_bounds = true;
                    col->zone.b.i64.min = v;
                    col->zone.b.i64.max = v;
                } else {
                    if (v < col->zone.b.i64.min) col->zone.b.i64.min = v;
                    if (v > col->zone.b.i64.max) col->zone.b.i64.max = v;
                }
                break;
            }
            case VX_TYPE_U64: {
                uint64_t v = vals[c].v.u64;
                ((uint64_t *)col->data)[r] = v;
                if (!col->zone.has_bounds) {
                    col->zone.has_bounds = true;
                    col->zone.b.u64.min = v;
                    col->zone.b.u64.max = v;
                } else {
                    if (v < col->zone.b.u64.min) col->zone.b.u64.min = v;
                    if (v > col->zone.b.u64.max) col->zone.b.u64.max = v;
                }
                break;
            }
            case VX_TYPE_F64: {
                double v = vals[c].v.f64;
                ((double *)col->data)[r] = v;
                if (!isnan(v)) {
                    if (!col->zone.has_bounds) {
                        col->zone.has_bounds = true;
                        col->zone.b.f64.min = v;
                        col->zone.b.f64.max = v;
                    } else {
                        if (v < col->zone.b.f64.min) col->zone.b.f64.min = v;
                        if (v > col->zone.b.f64.max) col->zone.b.f64.max = v;
                    }
                }
                break;
            }
            case VX_TYPE_BOOL:
                ((uint8_t *)col->data)[r] = vals[c].v.b ? 1U : 0U;
                break;
            case VX_TYPE_STR:
                break;
        }
    }

    ch->nrows++;
    return true;
}

bool vx_zone_may_match_i64(const vx_zone_t *z, vx_cmp_op_t op, int64_t val) {
    if (!z || !z->has_bounds) {
        return false;
    }
    int64_t lo = z->b.i64.min;
    int64_t hi = z->b.i64.max;
    switch (op) {
        case VX_CMP_EQ: return val >= lo && val <= hi;
        case VX_CMP_NE: return !(lo == val && hi == val);
        case VX_CMP_LT: return lo < val;
        case VX_CMP_LE: return lo <= val;
        case VX_CMP_GT: return hi > val;
        case VX_CMP_GE: return hi >= val;
        default:        return false;
    }
}

bool vx_zone_may_match_u64(const vx_zone_t *z, vx_cmp_op_t op, uint64_t val) {
    if (!z || !z->has_bounds) {
        return false;
    }
    uint64_t lo = z->b.u64.min;
    uint64_t hi = z->b.u64.max;
    switch (op) {
        case VX_CMP_EQ: return val >= lo && val <= hi;
        case VX_CMP_NE: return !(lo == val && hi == val);
        case VX_CMP_LT: return lo < val;
        case VX_CMP_LE: return lo <= val;
        case VX_CMP_GT: return hi > val;
        case VX_CMP_GE: return hi >= val;
        default:        return false;
    }
}

bool vx_zone_may_match_f64(const vx_zone_t *z, vx_cmp_op_t op, double val) {
    if (!z || !z->has_bounds) {
        return false;
    }
    if (isnan(val)) {
        return op == VX_CMP_NE;
    }
    double lo = z->b.f64.min;
    double hi = z->b.f64.max;
    switch (op) {
        case VX_CMP_EQ: return val >= lo && val <= hi;
        case VX_CMP_NE: return !(lo == val && hi == val);
        case VX_CMP_LT: return lo < val;
        case VX_CMP_LE: return lo <= val;
        case VX_CMP_GT: return hi > val;
        case VX_CMP_GE: return hi >= val;
        default:        return false;
    }
}

#define VX_DEFINE_SCALAR_LOOP(NAME, T, EXPR)                                    \
    static uint32_t NAME(const T *restrict col, const uint64_t *restrict nb,    \
                         uint32_t n, T val, uint32_t *restrict sel_out) {       \
        uint32_t out_n = 0;                                                     \
        if (!nb) {                                                              \
            for (uint32_t i = 0; i < n; i++) {                                  \
                T x = col[i];                                                   \
                sel_out[out_n] = i;                                             \
                out_n += (uint32_t)(EXPR);                                      \
            }                                                                   \
            return out_n;                                                       \
        }                                                                       \
        uint32_t base = 0;                                                      \
        while (base < n) {                                                      \
            uint64_t mask = nb[base >> 6];                                      \
            uint32_t end = base + 64U;                                          \
            if (end > n) end = n;                                               \
            if (mask == 0) {                                                    \
                for (uint32_t i = base; i < end; i++) {                         \
                    T x = col[i];                                               \
                    sel_out[out_n] = i;                                         \
                    out_n += (uint32_t)(EXPR);                                  \
                }                                                               \
            } else if (mask != ~0ULL) {                                         \
                for (uint32_t i = base; i < end; i++) {                         \
                    T x = col[i];                                               \
                    uint32_t not_null = (uint32_t)(((mask >> (i & 63U)) & 1U) == 0U); \
                    sel_out[out_n] = i;                                         \
                    out_n += (uint32_t)((EXPR) && not_null);                    \
                }                                                               \
            }                                                                   \
            base = end;                                                         \
        }                                                                       \
        return out_n;                                                           \
    }

VX_DEFINE_SCALAR_LOOP(filter_i64_eq, int64_t, x == val)
VX_DEFINE_SCALAR_LOOP(filter_i64_ne, int64_t, x != val)
VX_DEFINE_SCALAR_LOOP(filter_i64_lt, int64_t, x <  val)
VX_DEFINE_SCALAR_LOOP(filter_i64_le, int64_t, x <= val)
VX_DEFINE_SCALAR_LOOP(filter_i64_gt, int64_t, x >  val)
VX_DEFINE_SCALAR_LOOP(filter_i64_ge, int64_t, x >= val)

VX_DEFINE_SCALAR_LOOP(filter_u64_eq, uint64_t, x == val)
VX_DEFINE_SCALAR_LOOP(filter_u64_ne, uint64_t, x != val)
VX_DEFINE_SCALAR_LOOP(filter_u64_lt, uint64_t, x <  val)
VX_DEFINE_SCALAR_LOOP(filter_u64_le, uint64_t, x <= val)
VX_DEFINE_SCALAR_LOOP(filter_u64_gt, uint64_t, x >  val)
VX_DEFINE_SCALAR_LOOP(filter_u64_ge, uint64_t, x >= val)

VX_DEFINE_SCALAR_LOOP(filter_f64_eq_s, double, x == val)
VX_DEFINE_SCALAR_LOOP(filter_f64_ne_s, double, x != val)
VX_DEFINE_SCALAR_LOOP(filter_f64_lt_s, double, x <  val)
VX_DEFINE_SCALAR_LOOP(filter_f64_le_s, double, x <= val)
VX_DEFINE_SCALAR_LOOP(filter_f64_gt_s, double, x >  val)
VX_DEFINE_SCALAR_LOOP(filter_f64_ge_s, double, x >= val)

uint32_t vx_filter_i64(const int64_t *col, const uint64_t *null_bits, uint32_t n,
                       vx_cmp_op_t op, int64_t val, uint32_t *sel_out) {
    if (!col || !sel_out || n == 0) {
        return 0;
    }
    switch (op) {
        case VX_CMP_EQ: return filter_i64_eq(col, null_bits, n, val, sel_out);
        case VX_CMP_NE: return filter_i64_ne(col, null_bits, n, val, sel_out);
        case VX_CMP_LT: return filter_i64_lt(col, null_bits, n, val, sel_out);
        case VX_CMP_LE: return filter_i64_le(col, null_bits, n, val, sel_out);
        case VX_CMP_GT: return filter_i64_gt(col, null_bits, n, val, sel_out);
        case VX_CMP_GE: return filter_i64_ge(col, null_bits, n, val, sel_out);
        default:        return 0;
    }
}

uint32_t vx_filter_u64(const uint64_t *col, const uint64_t *null_bits, uint32_t n,
                       vx_cmp_op_t op, uint64_t val, uint32_t *sel_out) {
    if (!col || !sel_out || n == 0) {
        return 0;
    }
    switch (op) {
        case VX_CMP_EQ: return filter_u64_eq(col, null_bits, n, val, sel_out);
        case VX_CMP_NE: return filter_u64_ne(col, null_bits, n, val, sel_out);
        case VX_CMP_LT: return filter_u64_lt(col, null_bits, n, val, sel_out);
        case VX_CMP_LE: return filter_u64_le(col, null_bits, n, val, sel_out);
        case VX_CMP_GT: return filter_u64_gt(col, null_bits, n, val, sel_out);
        case VX_CMP_GE: return filter_u64_ge(col, null_bits, n, val, sel_out);
        default:        return 0;
    }
}

uint32_t vx_filter_f64(const double *col, const uint64_t *null_bits, uint32_t n,
                       vx_cmp_op_t op, double val, uint32_t *sel_out) {
    if (!col || !sel_out || n == 0) {
        return 0;
    }
    if (!null_bits && vx_simd_active() == VX_SIMD_AVX2) {
        return vx_filter_f64_avx2_dispatch(col, n, op, val, sel_out);
    }
    switch (op) {
        case VX_CMP_EQ: return filter_f64_eq_s(col, null_bits, n, val, sel_out);
        case VX_CMP_NE: return filter_f64_ne_s(col, null_bits, n, val, sel_out);
        case VX_CMP_LT: return filter_f64_lt_s(col, null_bits, n, val, sel_out);
        case VX_CMP_LE: return filter_f64_le_s(col, null_bits, n, val, sel_out);
        case VX_CMP_GT: return filter_f64_gt_s(col, null_bits, n, val, sel_out);
        case VX_CMP_GE: return filter_f64_ge_s(col, null_bits, n, val, sel_out);
        default:        return 0;
    }
}

uint32_t vx_filter_str_eq(const vx_str_t *col, const uint64_t *null_bits, uint32_t n,
                          const vx_str_t *needle, uint32_t *sel_out) {
    if (!col || !needle || !sel_out || n == 0) {
        return 0;
    }

    uint64_t key_head = 0;
    memcpy(&key_head, needle, sizeof(uint64_t));
    uint32_t out_n = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (null_bits && ((null_bits[i >> 6] >> (i & 63U)) & 1ULL) != 0ULL) {
            continue;
        }
        uint64_t cur_head = 0;
        memcpy(&cur_head, &col[i], sizeof(uint64_t));
        if (cur_head != key_head) {
            continue;
        }
        if (needle->len <= 4U) {
            sel_out[out_n++] = i;
        } else if (needle->len <= VX_STR_INLINE_MAX) {
            if (memcmp(col[i].u.tail, needle->u.tail, needle->len - 4U) == 0) {
                sel_out[out_n++] = i;
            }
        } else {
            if (memcmp(col[i].u.ptr + 4, needle->u.ptr + 4, needle->len - 4U) == 0) {
                sel_out[out_n++] = i;
            }
        }
    }
    return out_n;
}

uint32_t vx_chunk_filter_f64(const vx_chunk_t *ch, uint32_t col_idx,
                             vx_cmp_op_t op, double val, uint32_t *sel_out) {
    if (!ch || !sel_out || col_idx >= ch->ncols || ch->nrows == 0) {
        return 0;
    }
    const vx_col_t *col = &ch->cols[col_idx];
    if (col->type != VX_TYPE_F64) {
        return 0;
    }
    if (!vx_zone_may_match_f64(&col->zone, op, val)) {
        return 0;
    }
    const uint64_t *nb = col->zone.null_count == 0 ? NULL : col->null_bits;
    return vx_filter_f64((const double *)col->data, nb, ch->nrows, op, val, sel_out);
}

