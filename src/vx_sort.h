#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vx_op.h"

typedef struct vx_sort_key {
    uint32_t  out_col_idx;   /* index into cfg->col_types[] / packed row */
    vx_type_t type;
    bool      desc;
    bool      nulls_first;
} vx_sort_key_t;

typedef struct vx_sort_cfg {
    uint32_t             ncols;
    const vx_type_t     *col_types;
    const uint32_t      *batch_col_ids; /* input batch col index for each output col */
    const uint32_t      *table_col_ids; /* table col index for late materialization */
    const vx_sort_key_t *keys;
    uint32_t             nkeys;

    uint64_t             offset;        /* slice start (0 if none) */
    uint64_t             limit;         /* slice length (0 => unbounded for full sort) */
    const char          *spill_dir;     /* NULL => "/tmp" */
} vx_sort_cfg_t;

typedef union vx_sort_cell {
    int64_t  i64;
    uint64_t u64;
    double   f64;
    uint8_t  b;
    struct {
        int32_t  len;
        uint32_t off; /* byte offset from start of row's trailing str_data[] */
    } str;
} vx_sort_cell_t;

_Static_assert(sizeof(vx_sort_cell_t) == 8, "vx_sort_cell_t must be 8 bytes");

/* Self-contained, 8B-aligned variable-length row record:
 *   [0..7]                         vx_sort_row_t header
 *   [8 .. 8 + nbm_bytes)           uint64_t validity bitmap (1 = valid, 0 = null)
 *   [cells_off .. str_base_off)    vx_sort_cell_t cells[ncols]
 *   [str_base_off .. total_bytes)  concatenated UTF-8 string bytes, padded to 8B */
typedef struct vx_sort_row {
    uint32_t total_bytes;
    uint32_t str_bytes;
} vx_sort_row_t;

_Static_assert(sizeof(vx_sort_row_t) == 8, "vx_sort_row_t header must be 8 bytes");

static inline uint32_t vx_sr_nbm_words(uint32_t ncols) {
    return (ncols + 63u) >> 6;
}
static inline size_t vx_sr_cells_off(uint32_t ncols) {
    return sizeof(vx_sort_row_t) + (size_t)vx_sr_nbm_words(ncols) * sizeof(uint64_t);
}
static inline size_t vx_sr_str_off(uint32_t ncols) {
    return vx_sr_cells_off(ncols) + (size_t)ncols * sizeof(vx_sort_cell_t);
}
static inline uint64_t *vx_sr_val_mut(vx_sort_row_t *r) {
    return (uint64_t *)((uint8_t *)r + sizeof(vx_sort_row_t));
}
static inline const uint64_t *vx_sr_val(const vx_sort_row_t *r) {
    return (const uint64_t *)((const uint8_t *)r + sizeof(vx_sort_row_t));
}
static inline vx_sort_cell_t *vx_sr_cells_mut(vx_sort_row_t *r, uint32_t ncols) {
    return (vx_sort_cell_t *)((uint8_t *)r + vx_sr_cells_off(ncols));
}
static inline const vx_sort_cell_t *vx_sr_cells(const vx_sort_row_t *r, uint32_t ncols) {
    return (const vx_sort_cell_t *)((const uint8_t *)r + vx_sr_cells_off(ncols));
}
static inline uint8_t *vx_sr_strdata_mut(vx_sort_row_t *r, uint32_t ncols) {
    return (uint8_t *)r + vx_sr_str_off(ncols);
}
static inline const uint8_t *vx_sr_strdata(const vx_sort_row_t *r, uint32_t ncols) {
    return (const uint8_t *)r + vx_sr_str_off(ncols);
}
static inline bool vx_sr_is_valid(const vx_sort_row_t *r, uint32_t col) {
    return ((vx_sr_val(r)[col >> 6] >> (col & 63u)) & 1ULL) != 0;
}

static inline bool vx_svec_valid(const vx_vec_t *v, uint32_t r) {
    if (!v->has_nulls || !v->validity) return true;
    uint32_t idx = (v->kind == VX_VEC_CONST) ? 0u : r;
    return ((v->validity[idx >> 6] >> (idx & 63u)) & 1ULL) != 0;
}
static inline uint32_t vx_svec_vidx(const vx_vec_t *v, uint32_t r) {
    if (v->kind == VX_VEC_CONST) return 0u;
    if (v->kind == VX_VEC_DICT)  return ((const uint32_t *)v->data)[r];
    return r;
}
static inline const void *vx_svec_base(const vx_vec_t *v) {
    return (v->kind == VX_VEC_DICT) ? v->dict_vals : v->data;
}

/* Exact total-order comparisons (NaN > +Inf; -0.0 == +0.0) */
static inline int vx_cmp_f64_total(double a, double b) {
    bool na = isnan(a), nb = isnan(b);
    if (na || nb) return (na == nb) ? 0 : (na ? 1 : -1);
    return (a < b) ? -1 : (a > b ? 1 : 0);
}
static inline int vx_cmp_bytes_lex(const uint8_t *a, int32_t la, const uint8_t *b, int32_t lb) {
    int32_t m = (la < lb) ? la : lb;
    if (m > 0) {
        int c = memcmp(a, b, (size_t)m);
        if (c != 0) return c;
    }
    return (la < lb) ? -1 : (la > lb ? 1 : 0);
}

/* Returns <0 if a precedes b in the requested sort order, 0 if equal, >0 otherwise. */
static inline int vx_sort_row_cmp(const vx_sort_cfg_t *cfg,
                                  const vx_sort_row_t *a, const vx_sort_row_t *b) {
    const uint32_t        nc = cfg->ncols;
    const vx_sort_cell_t *ca = vx_sr_cells(a, nc);
    const vx_sort_cell_t *cb = vx_sr_cells(b, nc);
    const uint8_t        *sa = vx_sr_strdata(a, nc);
    const uint8_t        *sb = vx_sr_strdata(b, nc);

    for (uint32_t k = 0; k < cfg->nkeys; ++k) {
        const vx_sort_key_t *ks = &cfg->keys[k];
        const uint32_t       c  = ks->out_col_idx;
        const bool           va = vx_sr_is_valid(a, c);
        const bool           vb = vx_sr_is_valid(b, c);
        if (!va || !vb) {
            if (va == vb) continue;
            return (!va) ? (ks->nulls_first ? -1 : 1) : (ks->nulls_first ? 1 : -1);
        }

        int cmp = 0;
        switch (ks->type) {
        case VX_TYPE_BOOL:
            cmp = (ca[c].b < cb[c].b) ? -1 : (ca[c].b > cb[c].b ? 1 : 0);
            break;
        case VX_TYPE_I64:
            cmp = (ca[c].i64 < cb[c].i64) ? -1 : (ca[c].i64 > cb[c].i64 ? 1 : 0);
            break;
        case VX_TYPE_U64:
            cmp = (ca[c].u64 < cb[c].u64) ? -1 : (ca[c].u64 > cb[c].u64 ? 1 : 0);
            break;
        case VX_TYPE_F64:
            cmp = vx_cmp_f64_total(ca[c].f64, cb[c].f64);
            break;
        case VX_TYPE_STR:
            cmp = vx_cmp_bytes_lex(sa + ca[c].str.off, ca[c].str.len,
                                   sb + cb[c].str.off, cb[c].str.len);
            break;
        }
        if (cmp != 0) return ks->desc ? -cmp : cmp;
    }
    return 0;
}

/* Computes exact 8B-aligned packed byte size for row `phys_row` in `batch`. */
static inline size_t vx_sort_row_measure(const vx_sort_cfg_t *cfg, const vx_batch_t *in,
                                         uint32_t phys_row, uint32_t *out_str_bytes) {
    size_t sb = 0;
    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        if (cfg->col_types[c] != VX_TYPE_STR) continue;
        const vx_vec_t *v = &in->cols[cfg->batch_col_ids[c]];
        if (!vx_svec_valid(v, phys_row)) continue;
        const vx_strview_t *svs = (const vx_strview_t *)vx_svec_base(v);
        sb += (size_t)svs[vx_svec_vidx(v, phys_row)].inl.len;
    }
    *out_str_bytes = (uint32_t)sb;
    return (vx_sr_str_off(cfg->ncols) + sb + 7u) & ~(size_t)7u;
}

/* Packs row `phys_row` into caller-provided 8B-aligned buffer `dst` of size `total_bytes`. */
static inline void vx_sort_row_pack(const vx_sort_cfg_t *cfg, const vx_batch_t *in,
                                    uint32_t phys_row, size_t total_bytes,
                                    uint32_t str_bytes, vx_sort_row_t *dst) {
    const uint32_t  nc    = cfg->ncols;
    const uint32_t  nbmw  = vx_sr_nbm_words(nc);
    uint64_t       *val   = vx_sr_val_mut(dst);
    vx_sort_cell_t *cells = vx_sr_cells_mut(dst, nc);
    uint8_t        *sdata = vx_sr_strdata_mut(dst, nc);

    dst->total_bytes = (uint32_t)total_bytes;
    dst->str_bytes   = str_bytes;
    memset(val, 0, (size_t)nbmw * sizeof(uint64_t));

    uint32_t soff = 0;
    for (uint32_t c = 0; c < nc; ++c) {
        const vx_vec_t *v = &in->cols[cfg->batch_col_ids[c]];
        if (!vx_svec_valid(v, phys_row)) {
            cells[c].u64 = 0;
            continue;
        }
        val[c >> 6] |= (1ULL << (c & 63u));
        uint32_t    vi   = vx_svec_vidx(v, phys_row);
        const void *base = vx_svec_base(v);
        switch (cfg->col_types[c]) {
        case VX_TYPE_BOOL: cells[c].u64 = 0; cells[c].b = ((const uint8_t *)base)[vi]; break;
        case VX_TYPE_I64:  cells[c].i64 = ((const int64_t *)base)[vi]; break;
        case VX_TYPE_U64:  cells[c].u64 = ((const uint64_t *)base)[vi]; break;
        case VX_TYPE_F64:  cells[c].f64 = ((const double *)base)[vi]; break;
        case VX_TYPE_STR: {
            const vx_strview_t *sv = &((const vx_strview_t *)base)[vi];
            int32_t len = sv->inl.len;
            cells[c].str.len = len;
            cells[c].str.off = soff;
            if (len > 0) {
                memcpy(sdata + soff, vx_strview_ptr(sv, v->strpool), (size_t)len);
                soff += (uint32_t)len;
            }
            break;
        }
        }
    }
    /* Zero trailing alignment padding for deterministic disk writes */
    size_t used = vx_sr_str_off(nc) + soff;
    if (used < total_bytes) memset((uint8_t *)dst + used, 0, total_bytes - used);
}

/* Portable bottom-up stable merge sort over row pointer array `rows[0..n-1]`. */
static inline vx_status_t vx_sort_rows_stable(const vx_sort_cfg_t *cfg,
                                              vx_sort_row_t **rows, size_t n) {
    if (n <= 1u) return VX_OK;
    vx_sort_row_t **tmp = (vx_sort_row_t **)malloc(n * sizeof(*tmp));
    if (!tmp) return VX_ERR_OOM;

    vx_sort_row_t **src = rows;
    vx_sort_row_t **dst = tmp;
    for (size_t width = 1u; width < n; width <<= 1) {
        for (size_t left = 0; left < n; left += width * 2u) {
            size_t mid   = (left + width < n) ? (left + width) : n;
            size_t right = (left + width * 2u < n) ? (left + width * 2u) : n;
            size_t i = left, j = mid, k = left;
            while (i < mid && j < right) {
                dst[k++] = (vx_sort_row_cmp(cfg, src[i], src[j]) <= 0) ? src[i++] : src[j++];
            }
            while (i < mid)   dst[k++] = src[i++];
            while (j < right) dst[k++] = src[j++];
        }
        vx_sort_row_t **sw = src; src = dst; dst = sw;
    }
    if (src != rows) memcpy(rows, src, n * sizeof(*rows));
    free(tmp);
    return VX_OK;
}

/* Unpacks `nrows` (<= VX_BATCH_CAP) packed rows into a dense columnar vx_batch_t in `scratch`. */
vx_status_t vx_sort_unpack_batch(const vx_sort_cfg_t *cfg, const vx_sort_row_t *const *rows,
                                 uint32_t nrows, vx_arena_t *scratch, vx_batch_t *out);

extern const vx_sink_vtbl_t   VX_SINK_TOPK;
extern const vx_source_vtbl_t VX_SOURCE_TOPK_SCAN;
extern const vx_sink_vtbl_t   VX_SINK_EXT_SORT;
extern const vx_source_vtbl_t VX_SOURCE_EXT_SORT_SCAN;

