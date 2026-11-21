#include "vx_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================== IN-MEMORY WORKING TABLE (DATAFRAME) ==================== */

typedef struct {
    char       name[VX_COL_NAME_MAX];
    char       table[VX_COL_NAME_MAX]; /* optional qualifier for joins */
    vx_type_t  type;
    void      *data;
    uint64_t  *null_bits;
} vx_wcol_t;

typedef struct {
    uint32_t   ncols;
    uint32_t   nrows;
    uint32_t   cap;
    vx_wcol_t *cols;
    vx_arena_t str_arena;
} vx_wtable_t;

static size_t type_elem_size(vx_type_t t) {
    switch (t) {
        case VX_TYPE_I64:  return sizeof(int64_t);
        case VX_TYPE_U64:  return sizeof(uint64_t);
        case VX_TYPE_F64:  return sizeof(double);
        case VX_TYPE_BOOL: return sizeof(uint8_t);
        case VX_TYPE_STR:  return sizeof(vx_str_t);
        default:           return 8;
    }
}

static void *alloc_col_buf(size_t bytes) {
    if (bytes < 64U) bytes = 64U;
    void *p = NULL;
    if (posix_memalign(&p, 64U, bytes) != 0 || !p) return NULL;
    memset(p, 0, bytes);
    return p;
}

static vx_wtable_t *wtable_create(uint32_t ncols, uint32_t cap, size_t str_bytes) {
    vx_wtable_t *wt = (vx_wtable_t *)calloc(1, sizeof(vx_wtable_t));
    if (!wt) return NULL;
    wt->ncols = ncols;
    wt->cap = cap ? cap : 16U;
    if (!vx_arena_init(&wt->str_arena, str_bytes ? str_bytes : 65536U)) {
        free(wt);
        return NULL;
    }
    if (ncols > 0) {
        wt->cols = (vx_wcol_t *)calloc(ncols, sizeof(vx_wcol_t));
        if (!wt->cols) {
            vx_arena_destroy(&wt->str_arena);
            free(wt);
            return NULL;
        }
    }
    return wt;
}

static bool wtable_init_col(vx_wtable_t *wt, uint32_t idx, const char *tbl,
                            const char *name, vx_type_t type) {
    vx_wcol_t *c = &wt->cols[idx];
    snprintf(c->table, sizeof(c->table), "%s", tbl ? tbl : "");
    snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
    c->type = type;
    c->data = alloc_col_buf((size_t)wt->cap * type_elem_size(type));
    size_t nwords = ((size_t)wt->cap + 63U) / 64U;
    c->null_bits = (uint64_t *)alloc_col_buf(nwords * sizeof(uint64_t));
    return c->data != NULL && c->null_bits != NULL;
}

static void wtable_free(vx_wtable_t *wt) {
    if (!wt) return;
    if (wt->cols) {
        for (uint32_t c = 0; c < wt->ncols; c++) {
            free(wt->cols[c].data);
            free(wt->cols[c].null_bits);
        }
        free(wt->cols);
    }
    vx_arena_destroy(&wt->str_arena);
    free(wt);
}

static bool wtable_reserve(vx_wtable_t *wt, uint32_t need) {
    if (need <= wt->cap) return true;
    uint32_t ncap = wt->cap ? wt->cap : 16U;
    while (ncap < need) ncap *= 2U;

    size_t old_words = ((size_t)wt->cap + 63U) / 64U;
    size_t new_words = ((size_t)ncap + 63U) / 64U;

    for (uint32_t c = 0; c < wt->ncols; c++) {
        size_t esz = type_elem_size(wt->cols[c].type);
        void *nd = alloc_col_buf((size_t)ncap * esz);
        uint64_t *nb = (uint64_t *)alloc_col_buf(new_words * sizeof(uint64_t));
        if (!nd || !nb) {
            free(nd);
            free(nb);
            return false;
        }
        if (wt->nrows > 0) {
            memcpy(nd, wt->cols[c].data, (size_t)wt->nrows * esz);
            memcpy(nb, wt->cols[c].null_bits, old_words * sizeof(uint64_t));
        }
        free(wt->cols[c].data);
        free(wt->cols[c].null_bits);
        wt->cols[c].data = nd;
        wt->cols[c].null_bits = nb;
    }
    wt->cap = ncap;
    return true;
}

static inline bool wcol_is_null(const vx_wcol_t *c, uint32_t r) {
    return (c->null_bits[r >> 6] & (1ULL << (r & 63U))) != 0;
}

static inline void wcol_set_null(vx_wcol_t *c, uint32_t r, bool is_null) {
    if (is_null) c->null_bits[r >> 6] |= (1ULL << (r & 63U));
    else c->null_bits[r >> 6] &= ~(1ULL << (r & 63U));
}

static vx_val_t wtable_get_val(const vx_wtable_t *wt, uint32_t c, uint32_t r) {
    const vx_wcol_t *col = &wt->cols[c];
    vx_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = col->type;
    v.is_null = wcol_is_null(col, r);
    if (v.is_null) return v;
    switch (col->type) {
        case VX_TYPE_I64:  v.v.i64 = ((const int64_t *)col->data)[r]; break;
        case VX_TYPE_U64:  v.v.u64 = ((const uint64_t *)col->data)[r]; break;
        case VX_TYPE_F64:  v.v.f64 = ((const double *)col->data)[r]; break;
        case VX_TYPE_BOOL: v.v.b   = ((const uint8_t *)col->data)[r] != 0; break;
        case VX_TYPE_STR: {
            const vx_str_t *s = &((const vx_str_t *)col->data)[r];
            v.v.str.ptr = vx_str_data(s);
            v.v.str.len = s->len;
            break;
        }
    }
    return v;
}

static bool wtable_set_val(vx_wtable_t *wt, uint32_t c, uint32_t r, const vx_val_t *v) {
    vx_wcol_t *col = &wt->cols[c];
    if (v->is_null) {
        wcol_set_null(col, r, true);
        return true;
    }
    wcol_set_null(col, r, false);
    switch (col->type) {
        case VX_TYPE_I64:
            if (v->type == VX_TYPE_U64) ((int64_t *)col->data)[r] = (int64_t)v->v.u64;
            else if (v->type == VX_TYPE_F64) ((int64_t *)col->data)[r] = (int64_t)v->v.f64;
            else ((int64_t *)col->data)[r] = v->v.i64;
            break;
        case VX_TYPE_U64:
            if (v->type == VX_TYPE_I64) ((uint64_t *)col->data)[r] = (uint64_t)v->v.i64;
            else if (v->type == VX_TYPE_F64) ((uint64_t *)col->data)[r] = (uint64_t)v->v.f64;
            else ((uint64_t *)col->data)[r] = v->v.u64;
            break;
        case VX_TYPE_F64:
            if (v->type == VX_TYPE_I64) ((double *)col->data)[r] = (double)v->v.i64;
            else if (v->type == VX_TYPE_U64) ((double *)col->data)[r] = (double)v->v.u64;
            else ((double *)col->data)[r] = v->v.f64;
            break;
        case VX_TYPE_BOOL:
            ((uint8_t *)col->data)[r] = v->v.b ? 1U : 0U;
            break;
        case VX_TYPE_STR: {
            vx_str_t *dst = &((vx_str_t *)col->data)[r];
            return vx_str_make(&wt->str_arena, v->v.str.ptr, v->v.str.len, dst);
        }
    }
    return true;
}

static int32_t wtable_find_ident(const vx_wtable_t *wt, const vx_expr_t *ident) {
    if (!wt || !ident || ident->kind != VX_EXPR_IDENT) return -1;
    if (ident->u.ident.nparts == 1) {
        vx_span_t s = ident->u.ident.parts[0];
        for (uint32_t c = 0; c < wt->ncols; c++) {
            if (strlen(wt->cols[c].name) == s.len &&
                memcmp(wt->cols[c].name, s.ptr, s.len) == 0) {
                return (int32_t)c;
            }
        }
    } else if (ident->u.ident.nparts == 2) {
        vx_span_t t = ident->u.ident.parts[0];
        vx_span_t n = ident->u.ident.parts[1];
        for (uint32_t c = 0; c < wt->ncols; c++) {
            if (strlen(wt->cols[c].table) == t.len &&
                memcmp(wt->cols[c].table, t.ptr, t.len) == 0 &&
                strlen(wt->cols[c].name) == n.len &&
                memcmp(wt->cols[c].name, n.ptr, n.len) == 0) {
                return (int32_t)c;
            }
        }
    }
    return -1;
}

static int32_t wtable_find_span(const vx_wtable_t *wt, vx_span_t sp) {
    const char *dot = (const char *)memchr(sp.ptr, '.', sp.len);
    if (dot) {
        size_t tlen = (size_t)(dot - sp.ptr);
        size_t clen = sp.len - tlen - 1U;
        for (uint32_t c = 0; c < wt->ncols; c++) {
            if (strlen(wt->cols[c].table) == tlen &&
                memcmp(wt->cols[c].table, sp.ptr, tlen) == 0 &&
                strlen(wt->cols[c].name) == clen &&
                memcmp(wt->cols[c].name, dot + 1, clen) == 0) {
                return (int32_t)c;
            }
        }
        return -1;
    }
    for (uint32_t c = 0; c < wt->ncols; c++) {
        if (strlen(wt->cols[c].name) == sp.len &&
            memcmp(wt->cols[c].name, sp.ptr, sp.len) == 0) {
            return (int32_t)c;
        }
    }
    return -1;
}

static vx_result_t *wtable_to_result(vx_wtable_t *wt) {
    vx_result_t *res = (vx_result_t *)calloc(1, sizeof(vx_result_t));
    if (!res) {
        wtable_free(wt);
        return NULL;
    }
    res->ok = true;
    res->nrows = wt->nrows;
    res->ncols = wt->ncols;
    res->str_arena = wt->str_arena;
    memset(&wt->str_arena, 0, sizeof(wt->str_arena));

    if (wt->ncols > 0) {
        res->cols = (vx_result_col_t *)calloc(wt->ncols, sizeof(vx_result_col_t));
        for (uint32_t c = 0; c < wt->ncols; c++) {
            snprintf(res->cols[c].name, VX_COL_NAME_MAX, "%s", wt->cols[c].name);
            res->cols[c].type = wt->cols[c].type;
            res->cols[c].data = wt->cols[c].data;
            res->cols[c].null_bits = wt->cols[c].null_bits;
            wt->cols[c].data = NULL;
            wt->cols[c].null_bits = NULL;
        }
    }
    wtable_free(wt);
    return res;
}

/* ==================== EXPRESSION EVALUATOR ==================== */

typedef struct {
    const vx_param_t *params;
    size_t            nparams;
    char              err[192];
    bool              failed;
} eval_ctx_t;

static double val_as_f64(const vx_val_t *v) {
    if (v->type == VX_TYPE_F64) return v->v.f64;
    if (v->type == VX_TYPE_I64) return (double)v->v.i64;
    if (v->type == VX_TYPE_U64) return (double)v->v.u64;
    return 0.0;
}

static bool is_numeric(vx_type_t t) {
    return t == VX_TYPE_I64 || t == VX_TYPE_U64 || t == VX_TYPE_F64;
}

static int compare_vals(const vx_val_t *a, const vx_val_t *b) {
    if (a->is_null && b->is_null) return 0;
    if (a->is_null) return 1;
    if (b->is_null) return -1;

    if (a->type == VX_TYPE_STR && b->type == VX_TYPE_STR) {
        uint32_t ml = a->v.str.len < b->v.str.len ? a->v.str.len : b->v.str.len;
        int c = ml ? memcmp(a->v.str.ptr, b->v.str.ptr, ml) : 0;
        if (c != 0) return c;
        return a->v.str.len < b->v.str.len ? -1 : (a->v.str.len > b->v.str.len ? 1 : 0);
    }
    if (a->type == VX_TYPE_BOOL && b->type == VX_TYPE_BOOL) {
        return (int)a->v.b - (int)b->v.b;
    }
    if (a->type == VX_TYPE_I64 && b->type == VX_TYPE_I64) {
        return a->v.i64 < b->v.i64 ? -1 : (a->v.i64 > b->v.i64 ? 1 : 0);
    }
    if (a->type == VX_TYPE_U64 && b->type == VX_TYPE_U64) {
        return a->v.u64 < b->v.u64 ? -1 : (a->v.u64 > b->v.u64 ? 1 : 0);
    }
    double da = val_as_f64(a);
    double db = val_as_f64(b);
    return da < db ? -1 : (da > db ? 1 : 0);
}

static vx_val_t eval_expr(const vx_expr_t *e, const vx_wtable_t *wt,
                          uint32_t row, eval_ctx_t *ec) {
    vx_val_t out;
    memset(&out, 0, sizeof(out));
    out.type = VX_TYPE_I64;
    out.is_null = true;
    if (!e || ec->failed) return out;

    switch (e->kind) {
        case VX_EXPR_NULL:
            return out;
        case VX_EXPR_BOOL:
            out.type = VX_TYPE_BOOL; out.is_null = false; out.v.b = e->u.b;
            return out;
        case VX_EXPR_INT:
            out.type = VX_TYPE_I64; out.is_null = false; out.v.i64 = e->u.i64;
            return out;
        case VX_EXPR_FLOAT:
            out.type = VX_TYPE_F64; out.is_null = false; out.v.f64 = e->u.f64;
            return out;
        case VX_EXPR_STR:
            out.type = VX_TYPE_STR; out.is_null = false;
            out.v.str.ptr = e->u.str.ptr; out.v.str.len = e->u.str.len;
            return out;
        case VX_EXPR_PARAM:
            for (size_t i = 0; i < ec->nparams; i++) {
                if (vx_span_eq_cstr(e->u.param, ec->params[i].name)) {
                    return ec->params[i].val;
                }
            }
            ec->failed = true;
            snprintf(ec->err, sizeof(ec->err), "unbound parameter '$%.*s'",
                     (int)e->u.param.len, e->u.param.ptr);
            return out;
        case VX_EXPR_IDENT: {
            int32_t ci = wtable_find_ident(wt, e);
            if (ci < 0) {
                ec->failed = true;
                snprintf(ec->err, sizeof(ec->err), "unknown column '%.*s'",
                         (int)e->u.ident.parts[0].len, e->u.ident.parts[0].ptr);
                return out;
            }
            return wtable_get_val(wt, (uint32_t)ci, row);
        }
        case VX_EXPR_UNARY: {
            vx_val_t v = eval_expr(e->u.unary.operand, wt, row, ec);
            if (ec->failed || v.is_null) return out;
            if (e->u.unary.op == VX_UNOP_NOT) {
                out.type = VX_TYPE_BOOL; out.is_null = false; out.v.b = !v.v.b;
                return out;
            }
            if (v.type == VX_TYPE_F64) {
                out.type = VX_TYPE_F64; out.is_null = false; out.v.f64 = -v.v.f64;
            } else {
                out.type = VX_TYPE_I64; out.is_null = false; out.v.i64 = -v.v.i64;
            }
            return out;
        }
        case VX_EXPR_BINARY: {
            vx_binop_t op = e->u.binary.op;
            if (op == VX_BINOP_AND) {
                vx_val_t l = eval_expr(e->u.binary.lhs, wt, row, ec);
                if (ec->failed) return out;
                out.type = VX_TYPE_BOOL; out.is_null = false;
                if (l.is_null || !l.v.b) { out.v.b = false; return out; }
                vx_val_t r = eval_expr(e->u.binary.rhs, wt, row, ec);
                out.v.b = (!r.is_null && r.v.b);
                return out;
            }
            if (op == VX_BINOP_OR) {
                vx_val_t l = eval_expr(e->u.binary.lhs, wt, row, ec);
                if (ec->failed) return out;
                out.type = VX_TYPE_BOOL; out.is_null = false;
                if (!l.is_null && l.v.b) { out.v.b = true; return out; }
                vx_val_t r = eval_expr(e->u.binary.rhs, wt, row, ec);
                out.v.b = (!r.is_null && r.v.b);
                return out;
            }
            if (op == VX_BINOP_IN) {
                vx_val_t l = eval_expr(e->u.binary.lhs, wt, row, ec);
                if (ec->failed) return out;
                out.type = VX_TYPE_BOOL; out.is_null = false; out.v.b = false;
                if (l.is_null || e->u.binary.rhs->kind != VX_EXPR_LIST) return out;
                for (uint32_t i = 0; i < e->u.binary.rhs->u.list.nitems; i++) {
                    vx_val_t it = eval_expr(e->u.binary.rhs->u.list.items[i], wt, row, ec);
                    if (!it.is_null && compare_vals(&l, &it) == 0) {
                        out.v.b = true;
                        break;
                    }
                }
                return out;
            }

            vx_val_t l = eval_expr(e->u.binary.lhs, wt, row, ec);
            vx_val_t r = eval_expr(e->u.binary.rhs, wt, row, ec);
            if (ec->failed) return out;
            if (l.is_null || r.is_null) {
                if (op == VX_BINOP_EQ) {
                    out.type = VX_TYPE_BOOL; out.is_null = false;
                    out.v.b = (l.is_null && r.is_null);
                    return out;
                }
                if (op == VX_BINOP_NE) {
                    out.type = VX_TYPE_BOOL; out.is_null = false;
                    out.v.b = (l.is_null != r.is_null);
                    return out;
                }
                if (op >= VX_BINOP_LT && op <= VX_BINOP_GE) {
                    out.type = VX_TYPE_BOOL; out.is_null = false; out.v.b = false;
                }
                return out;
            }

            if (op >= VX_BINOP_EQ && op <= VX_BINOP_GE) {
                int c = compare_vals(&l, &r);
                out.type = VX_TYPE_BOOL; out.is_null = false;
                switch (op) {
                    case VX_BINOP_EQ: out.v.b = (c == 0); break;
                    case VX_BINOP_NE: out.v.b = (c != 0); break;
                    case VX_BINOP_LT: out.v.b = (c <  0); break;
                    case VX_BINOP_LE: out.v.b = (c <= 0); break;
                    case VX_BINOP_GT: out.v.b = (c >  0); break;
                    case VX_BINOP_GE: out.v.b = (c >= 0); break;
                    default: break;
                }
                return out;
            }

            if (is_numeric(l.type) && is_numeric(r.type)) {
                if (l.type == VX_TYPE_F64 || r.type == VX_TYPE_F64 || op == VX_BINOP_DIV) {
                    double a = val_as_f64(&l), b = val_as_f64(&r);
                    out.type = VX_TYPE_F64; out.is_null = false;
                    switch (op) {
                        case VX_BINOP_ADD: out.v.f64 = a + b; break;
                        case VX_BINOP_SUB: out.v.f64 = a - b; break;
                        case VX_BINOP_MUL: out.v.f64 = a * b; break;
                        case VX_BINOP_DIV: out.v.f64 = (b != 0.0) ? a / b : 0.0; break;
                        case VX_BINOP_MOD: out.v.f64 = (b != 0.0) ? fmod(a, b) : 0.0; break;
                        default: break;
                    }
                } else {
                    int64_t a = (l.type == VX_TYPE_U64) ? (int64_t)l.v.u64 : l.v.i64;
                    int64_t b = (r.type == VX_TYPE_U64) ? (int64_t)r.v.u64 : r.v.i64;
                    out.type = VX_TYPE_I64; out.is_null = false;
                    switch (op) {
                        case VX_BINOP_ADD: out.v.i64 = a + b; break;
                        case VX_BINOP_SUB: out.v.i64 = a - b; break;
                        case VX_BINOP_MUL: out.v.i64 = a * b; break;
                        case VX_BINOP_MOD: out.v.i64 = (b != 0) ? a % b : 0; break;
                        default: break;
                    }
                }
            }
            return out;
        }
        default:
            return out;
    }
}

/* ==================== MORSEL-DRIVEN FAST PATH (FILTER + HASH AGG) ==================== */

typedef struct {
    int64_t  key;
    uint64_t cnt;
    double   sum_f64;
    int64_t  sum_i64;
    bool     used;
} fast_agg_slot_t;

typedef struct {
    const vx_table_t     *table;
    atomic_uint_fast32_t  next_chunk;
    uint32_t              filt_col;
    vx_cmp_op_t           filt_op;
    double                filt_f64;
    int64_t               filt_i64;
    vx_type_t             filt_type;
    bool                  has_group;
    uint32_t              grp_col;
    uint32_t              agg_col;
    vx_type_t             agg_type;
    fast_agg_slot_t     **local_maps;
    uint32_t              map_cap;
    atomic_uint_fast64_t  pruned;
} morsel_ctx_t;

static void morsel_worker_fn(uint32_t wid, void *arg) {
    morsel_ctx_t *mc = (morsel_ctx_t *)arg;
    fast_agg_slot_t *map = mc->local_maps[wid];
    uint32_t mask = mc->map_cap - 1U;
    uint32_t *sel = NULL;
    uint32_t sel_cap = 0;

    for (;;) {
        uint32_t ci = atomic_fetch_add_explicit(&mc->next_chunk, 1, memory_order_relaxed);
        if (ci >= mc->table->nchunks) break;

        const vx_chunk_t *ch = mc->table->chunks[ci]->chunk;
        if (ch->nrows == 0) continue;
        const vx_delmap_t *dm = mc->table->delmaps ? mc->table->delmaps[ci] : NULL;
        if (dm && vx_delmap_count(dm) >= ch->nrows) continue;

        if (mc->filt_type == VX_TYPE_F64) {
            if (!vx_zone_may_match_f64(&ch->cols[mc->filt_col].zone, mc->filt_op, mc->filt_f64)) {
                atomic_fetch_add_explicit(&mc->pruned, 1, memory_order_relaxed);
                continue;
            }
        } else if (mc->filt_type == VX_TYPE_I64) {
            if (!vx_zone_may_match_i64(&ch->cols[mc->filt_col].zone, mc->filt_op, mc->filt_i64)) {
                atomic_fetch_add_explicit(&mc->pruned, 1, memory_order_relaxed);
                continue;
            }
        }

        if (ch->nrows > sel_cap) {
            uint32_t *ns = (uint32_t *)realloc(sel, ch->nrows * sizeof(uint32_t));
            if (!ns) break;
            sel = ns;
            sel_cap = ch->nrows;
        }

        uint32_t matched = 0;
        const uint64_t *nb = ch->cols[mc->filt_col].zone.null_count == 0
                                 ? NULL : ch->cols[mc->filt_col].null_bits;
        if (mc->filt_type == VX_TYPE_F64) {
            matched = vx_filter_f64((const double *)ch->cols[mc->filt_col].data,
                                    nb, ch->nrows, mc->filt_op, mc->filt_f64, sel);
        } else {
            matched = vx_filter_i64((const int64_t *)ch->cols[mc->filt_col].data,
                                    nb, ch->nrows, mc->filt_op, mc->filt_i64, sel);
        }

        if (matched > 0 && dm) {
            matched = vx_delmap_filter_sel(dm, sel, matched);
        }

        if (matched == 0) continue;

        if (!mc->has_group) {
            map[0].used = true;
            map[0].cnt += matched;
            if (mc->agg_type == VX_TYPE_F64) {
                const double *av = (const double *)ch->cols[mc->agg_col].data;
                double s = 0.0;
                for (uint32_t i = 0; i < matched; i++) s += av[sel[i]];
                map[0].sum_f64 += s;
            } else {
                const int64_t *av = (const int64_t *)ch->cols[mc->agg_col].data;
                int64_t s = 0;
                for (uint32_t i = 0; i < matched; i++) s += av[sel[i]];
                map[0].sum_i64 += s;
            }
        } else {
            const int64_t *gk = (const int64_t *)ch->cols[mc->grp_col].data;
            const double *av_f = (const double *)ch->cols[mc->agg_col].data;
            const int64_t *av_i = (const int64_t *)ch->cols[mc->agg_col].data;
            for (uint32_t i = 0; i < matched; i++) {
                uint32_t r = sel[i];
                int64_t k = gk[r];
                uint32_t slot = (uint32_t)(((uint64_t)k * 11400714819323198485ULL) >> 54) & mask;
                while (map[slot].used && map[slot].key != k) {
                    slot = (slot + 1U) & mask;
                }
                if (!map[slot].used) {
                    map[slot].used = true;
                    map[slot].key = k;
                }
                map[slot].cnt++;
                if (mc->agg_type == VX_TYPE_F64) map[slot].sum_f64 += av_f[r];
                else map[slot].sum_i64 += av_i[r];
            }
        }
    }
    free(sel);
}

static bool binop_to_cmp(vx_binop_t b, vx_cmp_op_t *out) {
    switch (b) {
        case VX_BINOP_EQ: *out = VX_CMP_EQ; return true;
        case VX_BINOP_NE: *out = VX_CMP_NE; return true;
        case VX_BINOP_LT: *out = VX_CMP_LT; return true;
        case VX_BINOP_LE: *out = VX_CMP_LE; return true;
        case VX_BINOP_GT: *out = VX_CMP_GT; return true;
        case VX_BINOP_GE: *out = VX_CMP_GE; return true;
        default: return false;
    }
}

/* Detects `table[col op lit] (by i64_col)? { count(), sum(col) }` for morsel SIMD fast path */
static vx_wtable_t *try_morsel_fast_path(vx_db_t *db, const vx_table_t *t,
                                         const vx_stage_t *stages, uint32_t nstages,
                                         uint32_t *consumed) {
    if (nstages < 2 || stages[0].kind != VX_STAGE_FILTER || stages[1].kind != VX_STAGE_GROUP) {
        return NULL;
    }
    const vx_expr_t *pred = stages[0].u.filter.pred;
    if (!pred || pred->kind != VX_EXPR_BINARY) return NULL;
    vx_cmp_op_t cmp;
    if (!binop_to_cmp(pred->u.binary.op, &cmp)) return NULL;
    if (pred->u.binary.lhs->kind != VX_EXPR_IDENT || pred->u.binary.lhs->u.ident.nparts != 1) {
        return NULL;
    }
    int32_t fcol = vx_table_find_col(t, pred->u.binary.lhs->u.ident.parts[0].ptr,
                                     pred->u.binary.lhs->u.ident.parts[0].len);
    if (fcol < 0) return NULL;
    vx_type_t ft = t->cols[fcol].type;
    if (ft != VX_TYPE_F64 && ft != VX_TYPE_I64) return NULL;

    double f64_v = 0.0;
    int64_t i64_v = 0;
    if (pred->u.binary.rhs->kind == VX_EXPR_FLOAT) {
        f64_v = pred->u.binary.rhs->u.f64;
        i64_v = (int64_t)f64_v;
    } else if (pred->u.binary.rhs->kind == VX_EXPR_INT) {
        i64_v = pred->u.binary.rhs->u.i64;
        f64_v = (double)i64_v;
    } else {
        return NULL;
    }

    const vx_stage_t *g = &stages[1];
    if (g->u.group.nkeys > 1 || g->u.group.naggs != 2) return NULL;

    int32_t gcol = -1;
    if (g->u.group.nkeys == 1) {
        const vx_expr_t *ke = g->u.group.keys[0];
        if (ke->kind != VX_EXPR_IDENT || ke->u.ident.nparts != 1) return NULL;
        gcol = vx_table_find_col(t, ke->u.ident.parts[0].ptr, ke->u.ident.parts[0].len);
        if (gcol < 0 || t->cols[gcol].type != VX_TYPE_I64) return NULL;
    }

    const vx_proj_item_t *a0 = &g->u.group.aggs[0];
    const vx_proj_item_t *a1 = &g->u.group.aggs[1];
    if (a0->expr->kind != VX_EXPR_CALL || !vx_span_eq_cstr(a0->expr->u.call.name, "count") ||
        a1->expr->kind != VX_EXPR_CALL || !vx_span_eq_cstr(a1->expr->u.call.name, "sum") ||
        a1->expr->u.call.nargs != 1 || a1->expr->u.call.args[0]->kind != VX_EXPR_IDENT) {
        return NULL;
    }
    int32_t acol = vx_table_find_col(t, a1->expr->u.call.args[0]->u.ident.parts[0].ptr,
                                     a1->expr->u.call.args[0]->u.ident.parts[0].len);
    if (acol < 0 || (t->cols[acol].type != VX_TYPE_F64 && t->cols[acol].type != VX_TYPE_I64)) {
        return NULL;
    }

    uint32_t nw = db->pool.nworkers;
    uint32_t map_cap = 1024U;
    fast_agg_slot_t **lmaps = (fast_agg_slot_t **)calloc(nw, sizeof(*lmaps));
    for (uint32_t w = 0; w < nw; w++) {
        lmaps[w] = (fast_agg_slot_t *)calloc(map_cap, sizeof(fast_agg_slot_t));
    }

    morsel_ctx_t mc = {
        .table = t,
        .filt_col = (uint32_t)fcol,
        .filt_op = cmp,
        .filt_f64 = f64_v,
        .filt_i64 = i64_v,
        .filt_type = ft,
        .has_group = (gcol >= 0),
        .grp_col = gcol >= 0 ? (uint32_t)gcol : 0,
        .agg_col = (uint32_t)acol,
        .agg_type = t->cols[acol].type,
        .local_maps = lmaps,
        .map_cap = map_cap
    };
    atomic_init(&mc.next_chunk, 0);
    atomic_init(&mc.pruned, 0);

    vx_pool_run(&db->pool, morsel_worker_fn, &mc);
    atomic_fetch_add_explicit(&db->chunks_pruned, atomic_load(&mc.pruned), memory_order_relaxed);

    /* Merge thread-local maps into worker 0's map */
    fast_agg_slot_t *merged = lmaps[0];
    uint32_t mask = map_cap - 1U;
    for (uint32_t w = 1; w < nw; w++) {
        for (uint32_t s = 0; s < map_cap; s++) {
            if (!lmaps[w][s].used) continue;
            if (!mc.has_group) {
                merged[0].used = true;
                merged[0].cnt += lmaps[w][s].cnt;
                merged[0].sum_f64 += lmaps[w][s].sum_f64;
                merged[0].sum_i64 += lmaps[w][s].sum_i64;
            } else {
                int64_t k = lmaps[w][s].key;
                uint32_t slot = (uint32_t)(((uint64_t)k * 11400714819323198485ULL) >> 54) & mask;
                while (merged[slot].used && merged[slot].key != k) {
                    slot = (slot + 1U) & mask;
                }
                if (!merged[slot].used) {
                    merged[slot].used = true;
                    merged[slot].key = k;
                }
                merged[slot].cnt += lmaps[w][s].cnt;
                merged[slot].sum_f64 += lmaps[w][s].sum_f64;
                merged[slot].sum_i64 += lmaps[w][s].sum_i64;
            }
        }
    }

    uint32_t out_ncols = mc.has_group ? 3U : 2U;
    vx_wtable_t *out = wtable_create(out_ncols, 64, 4096);
    uint32_t ci = 0;
    if (mc.has_group) {
        wtable_init_col(out, ci++, t->name, t->cols[gcol].name, VX_TYPE_I64);
    }
    char a0n[VX_COL_NAME_MAX], a1n[VX_COL_NAME_MAX];
    snprintf(a0n, sizeof(a0n), "%.*s", (int)(a0->alias.len ? a0->alias.len : 5),
             a0->alias.len ? a0->alias.ptr : "count");
    snprintf(a1n, sizeof(a1n), "%.*s", (int)(a1->alias.len ? a1->alias.len : 3),
             a1->alias.len ? a1->alias.ptr : "sum");
    wtable_init_col(out, ci++, "", a0n, VX_TYPE_U64);
    wtable_init_col(out, ci++, "", a1n, mc.agg_type);

    if (!mc.has_group) {
        out->nrows = 1;
        ((uint64_t *)out->cols[0].data)[0] = merged[0].cnt;
        if (mc.agg_type == VX_TYPE_F64) ((double *)out->cols[1].data)[0] = merged[0].sum_f64;
        else ((int64_t *)out->cols[1].data)[0] = merged[0].sum_i64;
    } else {
        for (uint32_t s = 0; s < map_cap; s++) {
            if (!merged[s].used) continue;
            wtable_reserve(out, out->nrows + 1);
            uint32_t r = out->nrows++;
            ((int64_t *)out->cols[0].data)[r] = merged[s].key;
            ((uint64_t *)out->cols[1].data)[r] = merged[s].cnt;
            if (mc.agg_type == VX_TYPE_F64) ((double *)out->cols[2].data)[r] = merged[s].sum_f64;
            else ((int64_t *)out->cols[2].data)[r] = merged[s].sum_i64;
        }
    }

    for (uint32_t w = 0; w < nw; w++) free(lmaps[w]);
    free(lmaps);
    *consumed = 2;
    return out;
}

/* ==================== GENERAL STAGE EXECUTORS ==================== */

static vx_wtable_t *materialize_table(const vx_table_t *t) {
    uint32_t cap = t->total_rows > 0 ? (uint32_t)t->total_rows : 16U;
    size_t str_need = 65536U;
    for (uint32_t ci = 0; ci < t->nchunks; ci++) {
        str_need += t->chunks[ci]->chunk->str_arena.offset;
    }
    vx_wtable_t *wt = wtable_create(t->ncols, cap, str_need);
    if (!wt) return NULL;
    for (uint32_t c = 0; c < t->ncols; c++) {
        wtable_init_col(wt, c, t->name, t->cols[c].name, t->cols[c].type);
    }
    for (uint32_t ch_i = 0; ch_i < t->nchunks; ch_i++) {
        const vx_chunk_t *ch = t->chunks[ch_i]->chunk;
        const vx_delmap_t *dm = t->delmaps ? t->delmaps[ch_i] : NULL;
        if (dm && vx_delmap_count(dm) >= ch->nrows) continue;

        for (uint32_t r = 0; r < ch->nrows; r++) {
            if (vx_delmap_is_set(dm, r)) continue;
            wtable_reserve(wt, wt->nrows + 1);
            uint32_t dr = wt->nrows++;
            for (uint32_t c = 0; c < t->ncols; c++) {
                const vx_col_t *sc = &ch->cols[c];
                vx_val_t v;
                memset(&v, 0, sizeof(v));
                v.type = sc->type;
                v.is_null = vx_col_is_null(sc, r);
                if (!v.is_null) {
                    switch (sc->type) {
                        case VX_TYPE_I64:  v.v.i64 = ((const int64_t *)sc->data)[r]; break;
                        case VX_TYPE_U64:  v.v.u64 = ((const uint64_t *)sc->data)[r]; break;
                        case VX_TYPE_F64:  v.v.f64 = ((const double *)sc->data)[r]; break;
                        case VX_TYPE_BOOL: v.v.b   = ((const uint8_t *)sc->data)[r] != 0; break;
                        case VX_TYPE_STR: {
                            const vx_str_t *s = &((const vx_str_t *)sc->data)[r];
                            v.v.str.ptr = vx_str_data(s);
                            v.v.str.len = s->len;
                            break;
                        }
                    }
                }
                wtable_set_val(wt, c, dr, &v);
            }
        }
    }
    return wt;
}

static vx_wtable_t *materialize_single_row(const vx_table_t *t, uint32_t ci, uint32_t ri) {
    const vx_chunk_t *ch = t->chunks[ci]->chunk;
    vx_wtable_t *wt = wtable_create(t->ncols, 1, 4096U);
    if (!wt) return NULL;
    for (uint32_t c = 0; c < t->ncols; c++) {
        wtable_init_col(wt, c, t->name, t->cols[c].name, t->cols[c].type);
    }
    wt->nrows = 1;
    for (uint32_t c = 0; c < t->ncols; c++) {
        const vx_col_t *sc = &ch->cols[c];
        vx_val_t v;
        memset(&v, 0, sizeof(v));
        v.type = sc->type;
        v.is_null = vx_col_is_null(sc, ri);
        if (!v.is_null) {
            switch (sc->type) {
                case VX_TYPE_I64:  v.v.i64 = ((const int64_t *)sc->data)[ri]; break;
                case VX_TYPE_U64:  v.v.u64 = ((const uint64_t *)sc->data)[ri]; break;
                case VX_TYPE_F64:  v.v.f64 = ((const double *)sc->data)[ri]; break;
                case VX_TYPE_BOOL: v.v.b   = ((const uint8_t *)sc->data)[ri] != 0; break;
                case VX_TYPE_STR: {
                    const vx_str_t *s = &((const vx_str_t *)sc->data)[ri];
                    v.v.str.ptr = vx_str_data(s);
                    v.v.str.len = s->len;
                    break;
                }
            }
        }
        wtable_set_val(wt, c, 0, &v);
    }
    return wt;
}

static vx_wtable_t *try_pk_fast_path(const vx_table_t *t, const vx_stage_t *stages,
                                     uint32_t nstages, eval_ctx_t *ec, uint32_t *consumed) {
    if (nstages < 1 || stages[0].kind != VX_STAGE_PK || t->pk_col < 0 || !t->pk_index) {
        return NULL;
    }
    vx_val_t kv = eval_expr(stages[0].u.pk.key, NULL, 0, ec);
    if (ec->failed) return NULL;

    uint64_t pk_key = 0;
    if (!vx_val_to_pk_u64(&kv, t->cols[t->pk_col].type, &pk_key)) {
        return NULL;
    }

    *consumed = 1;
    uint64_t packed = 0;
    if (vx_pk_index_get(t->pk_index, pk_key, &packed)) {
        uint32_t ci = vx_pk_chunk(packed);
        uint32_t ri = vx_pk_row(packed);
        if (ci < t->nchunks && !vx_delmap_is_set(t->delmaps[ci], ri)) {
            return materialize_single_row(t, ci, ri);
        }
    }

    vx_wtable_t *empty = wtable_create(t->ncols, 1, 64U);
    if (empty) {
        for (uint32_t c = 0; c < t->ncols; c++) {
            wtable_init_col(empty, c, t->name, t->cols[c].name, t->cols[c].type);
        }
    }
    return empty;
}

static vx_wtable_t *stage_filter_or_pk(vx_wtable_t *in, const vx_stage_t *st,
                                       int32_t pk_col, eval_ctx_t *ec) {
    vx_wtable_t *out = wtable_create(in->ncols, in->nrows ? in->nrows : 16U,
                                     in->str_arena.cap);
    for (uint32_t c = 0; c < in->ncols; c++) {
        wtable_init_col(out, c, in->cols[c].table, in->cols[c].name, in->cols[c].type);
    }

    vx_val_t pk_target = {0};
    if (st->kind == VX_STAGE_PK) {
        if (pk_col < 0) pk_col = 0;
        pk_target = eval_expr(st->u.pk.key, in, 0, ec);
        if (ec->failed) { wtable_free(out); return NULL; }
    } else if (in->nrows == 0) {
        eval_expr(st->u.filter.pred, in, 0, ec);
        if (ec->failed) { wtable_free(out); return NULL; }
    }

    for (uint32_t r = 0; r < in->nrows; r++) {
        bool keep = false;
        if (st->kind == VX_STAGE_PK) {
            vx_val_t rv = wtable_get_val(in, (uint32_t)pk_col, r);
            keep = !rv.is_null && compare_vals(&rv, &pk_target) == 0;
        } else {
            vx_val_t v = eval_expr(st->u.filter.pred, in, r, ec);
            if (ec->failed) { wtable_free(out); return NULL; }
            keep = (!v.is_null && v.type == VX_TYPE_BOOL && v.v.b);
        }
        if (keep) {
            uint32_t dr = out->nrows++;
            for (uint32_t c = 0; c < in->ncols; c++) {
                vx_val_t cv = wtable_get_val(in, c, r);
                wtable_set_val(out, c, dr, &cv);
            }
        }
    }
    return out;
}

static vx_wtable_t *stage_slice(vx_wtable_t *in, const vx_stage_t *st) {
    int64_t start = st->u.slice.has_start ? st->u.slice.start : 0;
    int64_t stop  = st->u.slice.has_stop  ? st->u.slice.stop  : (int64_t)in->nrows;
    if (start < 0) start = 0;
    if (stop > (int64_t)in->nrows) stop = (int64_t)in->nrows;
    uint32_t count = stop > start ? (uint32_t)(stop - start) : 0;

    vx_wtable_t *out = wtable_create(in->ncols, count ? count : 16U, in->str_arena.cap);
    for (uint32_t c = 0; c < in->ncols; c++) {
        wtable_init_col(out, c, in->cols[c].table, in->cols[c].name, in->cols[c].type);
    }
    for (uint32_t r = (uint32_t)start; r < (uint32_t)stop; r++) {
        uint32_t dr = out->nrows++;
        for (uint32_t c = 0; c < in->ncols; c++) {
            vx_val_t cv = wtable_get_val(in, c, r);
            wtable_set_val(out, c, dr, &cv);
        }
    }
    return out;
}

static vx_type_t infer_expr_type(const vx_expr_t *e, const vx_wtable_t *wt, eval_ctx_t *ec) {
    if (!e) return VX_TYPE_I64;
    switch (e->kind) {
        case VX_EXPR_BOOL:  return VX_TYPE_BOOL;
        case VX_EXPR_INT:   return VX_TYPE_I64;
        case VX_EXPR_FLOAT: return VX_TYPE_F64;
        case VX_EXPR_STR:   return VX_TYPE_STR;
        case VX_EXPR_PARAM: {
            for (size_t i = 0; i < ec->nparams; i++) {
                if (vx_span_eq_cstr(e->u.param, ec->params[i].name)) {
                    return ec->params[i].val.type;
                }
            }
            ec->failed = true;
            snprintf(ec->err, sizeof(ec->err), "unbound parameter '$%.*s'",
                     (int)e->u.param.len, e->u.param.ptr);
            return VX_TYPE_I64;
        }
        case VX_EXPR_IDENT: {
            int32_t ci = wtable_find_ident(wt, e);
            if (ci < 0) {
                ec->failed = true;
                snprintf(ec->err, sizeof(ec->err), "unknown column '%.*s'",
                         (int)e->u.ident.parts[0].len, e->u.ident.parts[0].ptr);
                return VX_TYPE_I64;
            }
            return wt->cols[ci].type;
        }
        case VX_EXPR_BINARY: {
            vx_type_t lt = infer_expr_type(e->u.binary.lhs, wt, ec);
            vx_type_t rt = infer_expr_type(e->u.binary.rhs, wt, ec);
            if (e->u.binary.op >= VX_BINOP_EQ && e->u.binary.op <= VX_BINOP_IN) return VX_TYPE_BOOL;
            if (lt == VX_TYPE_F64 || rt == VX_TYPE_F64 || e->u.binary.op == VX_BINOP_DIV) return VX_TYPE_F64;
            return VX_TYPE_I64;
        }
        default: return VX_TYPE_I64;
    }
}

static vx_wtable_t *stage_project(vx_wtable_t *in, const vx_stage_t *st, eval_ctx_t *ec) {
    uint32_t nc = st->u.project.nitems;
    vx_wtable_t *out = wtable_create(nc, in->nrows ? in->nrows : 16U, in->str_arena.cap);

    for (uint32_t c = 0; c < nc; c++) {
        const vx_proj_item_t *it = &st->u.project.items[c];
        vx_type_t tp = infer_expr_type(it->expr, in, ec);
        if (ec->failed) { wtable_free(out); return NULL; }

        char cname[VX_COL_NAME_MAX] = {0};
        char tname[VX_COL_NAME_MAX] = {0};
        if (it->alias.len > 0) {
            snprintf(cname, sizeof(cname), "%.*s", (int)it->alias.len, it->alias.ptr);
        } else if (it->expr->kind == VX_EXPR_IDENT) {
            uint32_t np = it->expr->u.ident.nparts;
            vx_span_t last = it->expr->u.ident.parts[np - 1];
            snprintf(cname, sizeof(cname), "%.*s", (int)last.len, last.ptr);
            if (np == 2) {
                snprintf(tname, sizeof(tname), "%.*s",
                         (int)it->expr->u.ident.parts[0].len, it->expr->u.ident.parts[0].ptr);
            } else {
                int32_t ci = wtable_find_ident(in, it->expr);
                if (ci >= 0) snprintf(tname, sizeof(tname), "%s", in->cols[ci].table);
            }
        } else {
            snprintf(cname, sizeof(cname), "col_%u", c);
        }
        wtable_init_col(out, c, tname, cname, tp);
    }

    for (uint32_t r = 0; r < in->nrows; r++) {
        uint32_t dr = out->nrows++;
        for (uint32_t c = 0; c < nc; c++) {
            vx_val_t v = eval_expr(st->u.project.items[c].expr, in, r, ec);
            if (ec->failed) { wtable_free(out); return NULL; }
            wtable_set_val(out, c, dr, &v);
        }
    }
    return out;
}

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t hash_val_for_table(const vx_val_t *v, bool force_f64_num) {
    if (v->is_null) return 0x9e3779b97f4a7c15ULL;
    if (force_f64_num && is_numeric(v->type)) {
        double d = val_as_f64(v);
        if (d == 0.0) d = 0.0; /* normalize -0.0 */
        uint64_t bits = 0;
        memcpy(&bits, &d, sizeof(bits));
        return mix64(bits);
    }
    switch (v->type) {
        case VX_TYPE_I64:  return mix64((uint64_t)v->v.i64);
        case VX_TYPE_U64:  return mix64(v->v.u64);
        case VX_TYPE_BOOL: return v->v.b ? 0x517cc1b727220a95ULL : 0x2545f4914f6cdd1dULL;
        case VX_TYPE_F64: {
            double d = v->v.f64 == 0.0 ? 0.0 : v->v.f64;
            uint64_t bits = 0;
            memcpy(&bits, &d, sizeof(bits));
            return mix64(bits);
        }
        case VX_TYPE_STR: {
            uint64_t h = 14695981039346656037ULL ^ (uint64_t)v->v.str.len;
            const uint8_t *p = (const uint8_t *)v->v.str.ptr;
            for (uint32_t i = 0; i < v->v.str.len; i++) {
                h ^= p[i];
                h *= 1099511628211ULL;
            }
            return mix64(h);
        }
        default: return 0;
    }
}

/* ==================== LOCK-FREE STABLE MERGE SORT ==================== */

typedef struct {
    const vx_wtable_t *wt;
    const vx_stage_t  *st;
    eval_ctx_t        *ec;
    int32_t            col_idx[16];
    uint32_t           nitems;
} lf_sort_ctx_t;

static inline int lf_sort_cmp(const lf_sort_ctx_t *sc, uint32_t ra, uint32_t rb) {
    for (uint32_t i = 0; i < sc->nitems; i++) {
        vx_val_t va = (i < 16U && sc->col_idx[i] >= 0)
            ? wtable_get_val(sc->wt, (uint32_t)sc->col_idx[i], ra)
            : eval_expr(sc->st->u.sort.items[i].expr, sc->wt, ra, sc->ec);
        vx_val_t vb = (i < 16U && sc->col_idx[i] >= 0)
            ? wtable_get_val(sc->wt, (uint32_t)sc->col_idx[i], rb)
            : eval_expr(sc->st->u.sort.items[i].expr, sc->wt, rb, sc->ec);
        int c = compare_vals(&va, &vb);
        if (c != 0) return sc->st->u.sort.items[i].desc ? -c : c;
    }
    return 0;
}

static void lf_mergesort(uint32_t *a, uint32_t *tmp, uint32_t lo, uint32_t hi,
                         const lf_sort_ctx_t *sc) {
    if (hi - lo <= 16U) {
        for (uint32_t i = lo + 1U; i < hi; i++) {
            uint32_t key = a[i];
            uint32_t j = i;
            while (j > lo && lf_sort_cmp(sc, a[j - 1U], key) > 0) {
                a[j] = a[j - 1U];
                j--;
            }
            a[j] = key;
        }
        return;
    }
    uint32_t mid = lo + (hi - lo) / 2U;
    lf_mergesort(a, tmp, lo, mid, sc);
    lf_mergesort(a, tmp, mid, hi, sc);
    if (lf_sort_cmp(sc, a[mid - 1U], a[mid]) <= 0) return;

    uint32_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (lf_sort_cmp(sc, a[i], a[j]) <= 0) tmp[k++] = a[i++];
        else tmp[k++] = a[j++];
    }
    while (i < mid) tmp[k++] = a[i++];
    while (j < hi)  tmp[k++] = a[j++];
    memcpy(a + lo, tmp + lo, (hi - lo) * sizeof(uint32_t));
}

static vx_wtable_t *stage_sort(vx_wtable_t *in, const vx_stage_t *st, eval_ctx_t *ec) {
    if (in->nrows <= 1) return in;

    lf_sort_ctx_t sc = { .wt = in, .st = st, .ec = ec, .nitems = st->u.sort.nitems };
    for (uint32_t i = 0; i < sc.nitems && i < 16U; i++) {
        sc.col_idx[i] = -1;
        if (st->u.sort.items[i].expr->kind == VX_EXPR_IDENT) {
            int32_t ci = wtable_find_ident(in, st->u.sort.items[i].expr);
            if (ci < 0) {
                ec->failed = true;
                snprintf(ec->err, sizeof(ec->err), "unknown sort column");
                wtable_free(in);
                return NULL;
            }
            sc.col_idx[i] = ci;
        }
    }

    uint32_t *order = (uint32_t *)malloc(in->nrows * sizeof(uint32_t));
    uint32_t *tmp   = (uint32_t *)malloc(in->nrows * sizeof(uint32_t));
    if (!order || !tmp) {
        free(order); free(tmp); wtable_free(in); return NULL;
    }
    for (uint32_t i = 0; i < in->nrows; i++) order[i] = i;

    lf_mergesort(order, tmp, 0, in->nrows, &sc);
    free(tmp);

    vx_wtable_t *out = wtable_create(in->ncols, in->nrows, in->str_arena.cap);
    for (uint32_t c = 0; c < in->ncols; c++) {
        wtable_init_col(out, c, in->cols[c].table, in->cols[c].name, in->cols[c].type);
    }
    for (uint32_t i = 0; i < in->nrows; i++) {
        uint32_t dr = out->nrows++;
        for (uint32_t c = 0; c < in->ncols; c++) {
            vx_val_t v = wtable_get_val(in, c, order[i]);
            wtable_set_val(out, c, dr, &v);
        }
    }
    free(order);
    wtable_free(in);
    return out;
}

/* ==================== O(N) HASH GROUP-BY ==================== */

typedef enum { AGG_COUNT = 0, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX } agg_kind_t;

static vx_wtable_t *stage_group(vx_wtable_t *in, const vx_stage_t *st, eval_ctx_t *ec) {
    uint32_t nk = st->u.group.nkeys;
    uint32_t na = st->u.group.naggs;
    vx_wtable_t *out = wtable_create(nk + na, 64, in->str_arena.cap);
    if (!out) return NULL;

    int32_t key_cols[16];
    for (uint32_t k = 0; k < nk; k++) {
        key_cols[k] = (k < 16U && st->u.group.keys[k]->kind == VX_EXPR_IDENT)
            ? wtable_find_ident(in, st->u.group.keys[k]) : -1;
        vx_type_t tp = infer_expr_type(st->u.group.keys[k], in, ec);
        if (ec->failed) { wtable_free(out); return NULL; }
        char cname[VX_COL_NAME_MAX] = {0}, tname[VX_COL_NAME_MAX] = {0};
        if (st->u.group.keys[k]->kind == VX_EXPR_IDENT) {
            uint32_t np = st->u.group.keys[k]->u.ident.nparts;
            vx_span_t last = st->u.group.keys[k]->u.ident.parts[np - 1];
            snprintf(cname, sizeof(cname), "%.*s", (int)last.len, last.ptr);
            if (np == 2) {
                snprintf(tname, sizeof(tname), "%.*s",
                         (int)st->u.group.keys[k]->u.ident.parts[0].len,
                         st->u.group.keys[k]->u.ident.parts[0].ptr);
            }
        }
        wtable_init_col(out, k, tname, cname, tp);
    }

    agg_kind_t akind[32];
    int32_t    acol[32];
    for (uint32_t a = 0; a < na; a++) {
        const vx_proj_item_t *it = &st->u.group.aggs[a];
        vx_type_t tp = VX_TYPE_F64;
        akind[a] = AGG_COUNT;
        acol[a] = -1;
        if (it->expr->kind == VX_EXPR_CALL) {
            if (vx_span_eq_cstr(it->expr->u.call.name, "count")) {
                akind[a] = AGG_COUNT; tp = VX_TYPE_U64;
            } else if (vx_span_eq_cstr(it->expr->u.call.name, "sum")) {
                akind[a] = AGG_SUM;
                if (it->expr->u.call.nargs == 1) tp = infer_expr_type(it->expr->u.call.args[0], in, ec);
            } else if (vx_span_eq_cstr(it->expr->u.call.name, "avg")) {
                akind[a] = AGG_AVG; tp = VX_TYPE_F64;
            } else if (vx_span_eq_cstr(it->expr->u.call.name, "min")) {
                akind[a] = AGG_MIN;
                if (it->expr->u.call.nargs == 1) tp = infer_expr_type(it->expr->u.call.args[0], in, ec);
            } else if (vx_span_eq_cstr(it->expr->u.call.name, "max")) {
                akind[a] = AGG_MAX;
                if (it->expr->u.call.nargs == 1) tp = infer_expr_type(it->expr->u.call.args[0], in, ec);
            }
            if (it->expr->u.call.nargs == 1 && it->expr->u.call.args[0]->kind == VX_EXPR_IDENT) {
                acol[a] = wtable_find_ident(in, it->expr->u.call.args[0]);
            }
        }
        if (ec->failed) { wtable_free(out); return NULL; }
        char cname[VX_COL_NAME_MAX] = {0};
        if (it->alias.len > 0) snprintf(cname, sizeof(cname), "%.*s", (int)it->alias.len, it->alias.ptr);
        else snprintf(cname, sizeof(cname), "agg_%u", a);
        wtable_init_col(out, nk + a, "", cname, tp);
    }

    uint32_t gcap = 64U, ng = 0;
    uint32_t na_stride = na ? na : 1U;
    uint32_t nk_stride = nk ? nk : 1U;

    vx_val_t *g_keys  = (vx_val_t *)calloc((size_t)gcap * nk_stride, sizeof(vx_val_t));
    uint64_t *g_hash  = (uint64_t *)calloc(gcap, sizeof(uint64_t));
    uint64_t *g_cnt   = (uint64_t *)calloc((size_t)gcap * na_stride, sizeof(uint64_t));
    double   *g_sumf  = (double *)calloc((size_t)gcap * na_stride, sizeof(double));
    int64_t  *g_sumi  = (int64_t *)calloc((size_t)gcap * na_stride, sizeof(int64_t));
    uint64_t *g_sumu  = (uint64_t *)calloc((size_t)gcap * na_stride, sizeof(uint64_t));
    vx_val_t *g_minv  = (vx_val_t *)calloc((size_t)gcap * na_stride, sizeof(vx_val_t));
    vx_val_t *g_maxv  = (vx_val_t *)calloc((size_t)gcap * na_stride, sizeof(vx_val_t));

    size_t ht_cap = 128U;
    int32_t *ht = (int32_t *)malloc(ht_cap * sizeof(int32_t));
    memset(ht, 0xFF, ht_cap * sizeof(int32_t)); /* -1 = empty */

    if (nk == 0 && in->nrows == 0) {
        for (uint32_t a = 0; a < na; a++) {
            g_minv[a].is_null = true;
            g_maxv[a].is_null = true;
        }
        ng = 1;
    }

    vx_val_t cur_k[16];
    for (uint32_t r = 0; r < in->nrows; r++) {
        uint32_t gi = 0;
        if (nk == 0) {
            if (ng == 0) {
                for (uint32_t a = 0; a < na; a++) {
                    g_minv[a].is_null = true;
                    g_maxv[a].is_null = true;
                }
                ng = 1;
            }
            gi = 0;
        } else {
            uint64_t h = 0x517cc1b727220a95ULL;
            for (uint32_t k = 0; k < nk; k++) {
                cur_k[k] = (k < 16U && key_cols[k] >= 0)
                    ? wtable_get_val(in, (uint32_t)key_cols[k], r)
                    : eval_expr(st->u.group.keys[k], in, r, ec);
                if (ec->failed) goto group_fail;
                h = mix64(h ^ hash_val_for_table(&cur_k[k], false));
            }

            size_t mask = ht_cap - 1U;
            size_t slot = (size_t)h & mask;
            bool found = false;
            while (ht[slot] != -1) {
                uint32_t cand = (uint32_t)ht[slot];
                if (g_hash[cand] == h) {
                    bool match = true;
                    const vx_val_t *ck = &g_keys[(size_t)cand * nk_stride];
                    for (uint32_t k = 0; k < nk; k++) {
                        if (compare_vals(&ck[k], &cur_k[k]) != 0) { match = false; break; }
                    }
                    if (match) { gi = cand; found = true; break; }
                }
                slot = (slot + 1U) & mask;
            }

            if (!found) {
                if (ng == gcap) {
                    uint32_t ncap = gcap * 2U;
                    g_keys = (vx_val_t *)realloc(g_keys, (size_t)ncap * nk_stride * sizeof(vx_val_t));
                    g_hash = (uint64_t *)realloc(g_hash, (size_t)ncap * sizeof(uint64_t));
                    g_cnt  = (uint64_t *)realloc(g_cnt,  (size_t)ncap * na_stride * sizeof(uint64_t));
                    g_sumf = (double *)  realloc(g_sumf, (size_t)ncap * na_stride * sizeof(double));
                    g_sumi = (int64_t *) realloc(g_sumi, (size_t)ncap * na_stride * sizeof(int64_t));
                    g_sumu = (uint64_t *)realloc(g_sumu, (size_t)ncap * na_stride * sizeof(uint64_t));
                    g_minv = (vx_val_t *)realloc(g_minv, (size_t)ncap * na_stride * sizeof(vx_val_t));
                    g_maxv = (vx_val_t *)realloc(g_maxv, (size_t)ncap * na_stride * sizeof(vx_val_t));
                    gcap = ncap;
                }
                gi = ng++;
                g_hash[gi] = h;
                for (uint32_t k = 0; k < nk; k++) g_keys[(size_t)gi * nk_stride + k] = cur_k[k];
                for (uint32_t a = 0; a < na; a++) {
                    size_t idx = (size_t)gi * na_stride + a;
                    g_cnt[idx] = 0; g_sumf[idx] = 0.0; g_sumi[idx] = 0; g_sumu[idx] = 0;
                    g_minv[idx].is_null = true; g_maxv[idx].is_null = true;
                }
                ht[slot] = (int32_t)gi;

                if ((size_t)ng * 10U >= ht_cap * 7U) {
                    ht_cap *= 2U;
                    free(ht);
                    ht = (int32_t *)malloc(ht_cap * sizeof(int32_t));
                    memset(ht, 0xFF, ht_cap * sizeof(int32_t));
                    size_t nmask = ht_cap - 1U;
                    for (uint32_t i = 0; i < ng; i++) {
                        size_t s = (size_t)g_hash[i] & nmask;
                        while (ht[s] != -1) s = (s + 1U) & nmask;
                        ht[s] = (int32_t)i;
                    }
                }
            }
        }

        for (uint32_t a = 0; a < na; a++) {
            size_t idx = (size_t)gi * na_stride + a;
            if (akind[a] == AGG_COUNT) {
                g_cnt[idx]++;
                continue;
            }
            const vx_expr_t *ce = st->u.group.aggs[a].expr;
            if (ce->kind != VX_EXPR_CALL || ce->u.call.nargs != 1) continue;
            vx_val_t v = (acol[a] >= 0)
                ? wtable_get_val(in, (uint32_t)acol[a], r)
                : eval_expr(ce->u.call.args[0], in, r, ec);
            if (ec->failed) goto group_fail;
            if (v.is_null) continue;
            g_cnt[idx]++;
            g_sumf[idx] += val_as_f64(&v);
            if (v.type == VX_TYPE_I64) g_sumi[idx] += v.v.i64;
            else if (v.type == VX_TYPE_U64) g_sumu[idx] += v.v.u64;
            if (g_minv[idx].is_null || compare_vals(&v, &g_minv[idx]) < 0) g_minv[idx] = v;
            if (g_maxv[idx].is_null || compare_vals(&v, &g_maxv[idx]) > 0) g_maxv[idx] = v;
        }
    }

    wtable_reserve(out, ng);
    for (uint32_t i = 0; i < ng; i++) {
        uint32_t dr = out->nrows++;
        for (uint32_t k = 0; k < nk; k++) {
            wtable_set_val(out, k, dr, &g_keys[(size_t)i * nk_stride + k]);
        }
        for (uint32_t a = 0; a < na; a++) {
            size_t idx = (size_t)i * na_stride + a;
            vx_val_t res_v = { .type = out->cols[nk + a].type, .is_null = false };
            switch (akind[a]) {
                case AGG_COUNT: res_v.v.u64 = g_cnt[idx]; break;
                case AGG_SUM:
                    if (res_v.type == VX_TYPE_I64) res_v.v.i64 = g_sumi[idx];
                    else if (res_v.type == VX_TYPE_U64) res_v.v.u64 = g_sumu[idx];
                    else res_v.v.f64 = g_sumf[idx];
                    break;
                case AGG_AVG:
                    res_v.v.f64 = g_cnt[idx] ? (g_sumf[idx] / (double)g_cnt[idx]) : 0.0;
                    break;
                case AGG_MIN: res_v = g_minv[idx]; break;
                case AGG_MAX: res_v = g_maxv[idx]; break;
            }
            wtable_set_val(out, nk + a, dr, &res_v);
        }
    }

group_fail:
    free(ht);
    free(g_keys); free(g_hash); free(g_cnt);
    free(g_sumf); free(g_sumi); free(g_sumu); free(g_minv); free(g_maxv);
    if (ec->failed) { wtable_free(out); return NULL; }
    return out;
}

/* ==================== O(N + M) HASH JOIN (+, ++, &) ==================== */

static vx_wtable_t *stage_join(vx_wtable_t *left, const vx_catalog_t *cat,
                               const vx_table_t *left_schema, const vx_stage_t *st,
                               eval_ctx_t *ec) {
    vx_table_t *rtbl = vx_catalog_find(cat, st->u.join.right_table.ptr, st->u.join.right_table.len);
    if (!rtbl) {
        ec->failed = true;
        snprintf(ec->err, sizeof(ec->err), "unknown join table '%.*s'",
                 (int)st->u.join.right_table.len, st->u.join.right_table.ptr);
        return NULL;
    }

    int32_t lcol = -1, rcol = -1;
    if (st->u.join.has_on) {
        lcol = wtable_find_span(left, st->u.join.left_col);
        rcol = vx_table_find_col(rtbl, st->u.join.right_col.ptr, st->u.join.right_col.len);
    } else if (left_schema) {
        for (uint32_t c = 0; c < left_schema->ncols; c++) {
            if (left_schema->cols[c].has_fk &&
                vx_span_eq_cstr(st->u.join.right_table, left_schema->cols[c].fk_table)) {
                vx_span_t ln = { left_schema->cols[c].name, (uint32_t)strlen(left_schema->cols[c].name) };
                lcol = wtable_find_span(left, ln);
                rcol = vx_table_find_col(rtbl, left_schema->cols[c].fk_col,
                                         strlen(left_schema->cols[c].fk_col));
                break;
            }
        }
    }

    if (lcol < 0 || rcol < 0) {
        ec->failed = true;
        snprintf(ec->err, sizeof(ec->err),
                 "join requires explicit '(lcol == rcol)' or a declared foreign key");
        return NULL;
    }

    vx_wtable_t *right = materialize_table(rtbl);
    if (!right) return NULL;

    bool force_f64 = (left->cols[lcol].type != right->cols[rcol].type);

    /* Build chained hash index on right side (reverse insert preserves row order) */
    size_t nbuckets = 64U;
    while (nbuckets < (size_t)right->nrows * 2U) nbuckets *= 2U;
    size_t bmask = nbuckets - 1U;

    int32_t *heads = (int32_t *)malloc(nbuckets * sizeof(int32_t));
    int32_t *next  = (int32_t *)malloc((right->nrows ? right->nrows : 1U) * sizeof(int32_t));
    bool    *rmat  = (bool *)calloc(right->nrows ? right->nrows : 1U, sizeof(bool));
    memset(heads, 0xFF, nbuckets * sizeof(int32_t));

    for (uint32_t i = right->nrows; i > 0; i--) {
        uint32_t rr = i - 1U;
        vx_val_t rv = wtable_get_val(right, (uint32_t)rcol, rr);
        if (rv.is_null) {
            next[rr] = -1;
            continue;
        }
        size_t b = (size_t)hash_val_for_table(&rv, force_f64) & bmask;
        next[rr] = heads[b];
        heads[b] = (int32_t)rr;
    }

    /* Probe phase: collect (lr, rr) index pairs (-1 denotes NULL side) */
    size_t pcap = left->nrows + right->nrows + 16U;
    size_t plen = 0;
    int32_t *pl = (int32_t *)malloc(pcap * sizeof(int32_t));
    int32_t *pr = (int32_t *)malloc(pcap * sizeof(int32_t));

    for (uint32_t lr = 0; lr < left->nrows; lr++) {
        vx_val_t lv = wtable_get_val(left, (uint32_t)lcol, lr);
        bool any = false;
        if (!lv.is_null) {
            size_t b = (size_t)hash_val_for_table(&lv, force_f64) & bmask;
            for (int32_t rr = heads[b]; rr != -1; rr = next[rr]) {
                vx_val_t rv = wtable_get_val(right, (uint32_t)rcol, (uint32_t)rr);
                if (compare_vals(&lv, &rv) == 0) {
                    any = true;
                    rmat[rr] = true;
                    if (plen == pcap) {
                        pcap *= 2U;
                        pl = (int32_t *)realloc(pl, pcap * sizeof(int32_t));
                        pr = (int32_t *)realloc(pr, pcap * sizeof(int32_t));
                    }
                    pl[plen] = (int32_t)lr;
                    pr[plen] = rr;
                    plen++;
                }
            }
        }
        if (!any && (st->u.join.kind == VX_JOIN_LEFT || st->u.join.kind == VX_JOIN_FULL)) {
            if (plen == pcap) {
                pcap *= 2U;
                pl = (int32_t *)realloc(pl, pcap * sizeof(int32_t));
                pr = (int32_t *)realloc(pr, pcap * sizeof(int32_t));
            }
            pl[plen] = (int32_t)lr;
            pr[plen] = -1;
            plen++;
        }
    }

    if (st->u.join.kind == VX_JOIN_FULL) {
        for (uint32_t rr = 0; rr < right->nrows; rr++) {
            if (rmat[rr]) continue;
            if (plen == pcap) {
                pcap *= 2U;
                pl = (int32_t *)realloc(pl, pcap * sizeof(int32_t));
                pr = (int32_t *)realloc(pr, pcap * sizeof(int32_t));
            }
            pl[plen] = -1;
            pr[plen] = (int32_t)rr;
            plen++;
        }
    }

    /* Column-at-a-time materialization for L1 cache locality */
    uint32_t out_nc = left->ncols + right->ncols;
    vx_wtable_t *out = wtable_create(out_nc, plen ? (uint32_t)plen : 16U,
                                     left->str_arena.cap + right->str_arena.cap);
    for (uint32_t c = 0; c < left->ncols; c++) {
        wtable_init_col(out, c, left->cols[c].table, left->cols[c].name, left->cols[c].type);
    }
    for (uint32_t c = 0; c < right->ncols; c++) {
        wtable_init_col(out, left->ncols + c, right->cols[c].table,
                        right->cols[c].name, right->cols[c].type);
    }
    out->nrows = (uint32_t)plen;
    vx_val_t null_v = { .is_null = true };

    for (uint32_t c = 0; c < left->ncols; c++) {
        for (size_t i = 0; i < plen; i++) {
            if (pl[i] < 0) wtable_set_val(out, c, (uint32_t)i, &null_v);
            else {
                vx_val_t cv = wtable_get_val(left, c, (uint32_t)pl[i]);
                wtable_set_val(out, c, (uint32_t)i, &cv);
            }
        }
    }
    for (uint32_t c = 0; c < right->ncols; c++) {
        uint32_t dc = left->ncols + c;
        for (size_t i = 0; i < plen; i++) {
            if (pr[i] < 0) wtable_set_val(out, dc, (uint32_t)i, &null_v);
            else {
                vx_val_t cv = wtable_get_val(right, c, (uint32_t)pr[i]);
                wtable_set_val(out, dc, (uint32_t)i, &cv);
            }
        }
    }

    free(pl); free(pr); free(heads); free(next); free(rmat);
    wtable_free(right);
    return out;
}

/* ==================== DDL & MUTATION EXECUTOR ==================== */

static bool parse_type_name(vx_span_t s, vx_type_t *out) {
    if (vx_span_eq_cstr(s, "i64") || vx_span_eq_cstr(s, "i32") || vx_span_eq_cstr(s, "int")) {
        *out = VX_TYPE_I64; return true;
    }
    if (vx_span_eq_cstr(s, "u64") || vx_span_eq_cstr(s, "u32") || vx_span_eq_cstr(s, "u16")) {
        *out = VX_TYPE_U64; return true;
    }
    if (vx_span_eq_cstr(s, "f64") || vx_span_eq_cstr(s, "f32") || vx_span_eq_cstr(s, "float")) {
        *out = VX_TYPE_F64; return true;
    }
    if (vx_span_eq_cstr(s, "bool")) { *out = VX_TYPE_BOOL; return true; }
    if (vx_span_eq_cstr(s, "str"))  { *out = VX_TYPE_STR;  return true; }
    return false;
}

static bool rebuild_table_from_wtable(vx_table_t *t, const vx_wtable_t *wt,
                                      uint32_t chunk_rows, uint64_t *next_chunk_id) {
    for (uint32_t i = 0; i < t->nchunks; i++) {
        vx_chunk_ref_dec(t->chunks[i]);
        if (t->delmaps) {
            vx_delmap_dec(t->delmaps[i]);
            t->delmaps[i] = NULL;
        }
    }
    t->nchunks = 0;
    t->total_rows = 0;
    if (t->pk_index) {
        vx_pk_index_dec(t->pk_index);
        t->pk_index = vx_pk_index_create();
    }
    vx_val_t row[VX_MAX_COLS];
    for (uint32_t r = 0; r < wt->nrows; r++) {
        for (uint32_t c = 0; c < t->ncols; c++) row[c] = wtable_get_val(wt, c, r);
        if (!vx_table_append_row_cow(t, row, chunk_rows, next_chunk_id)) return false;
    }
    return true;
}

static vx_result_t *exec_write_stmt(vx_db_t *db, vx_catalog_t *cat,
                                    const vx_stmt_t *stmt, eval_ctx_t *ec) {
    switch (stmt->kind) {
        case VX_STMT_CREATE_TABLE: {
            if (vx_catalog_find(cat, stmt->u.create_table.table.ptr, stmt->u.create_table.table.len)) {
                return vx_result_new_err("table already exists");
            }
            if (cat->ntables >= VX_MAX_TABLES || stmt->u.create_table.ncols > VX_MAX_COLS) {
                return vx_result_new_err("table or column limit exceeded");
            }
            vx_table_t *t = (vx_table_t *)calloc(1, sizeof(vx_table_t));
            snprintf(t->name, sizeof(t->name), "%.*s",
                     (int)stmt->u.create_table.table.len, stmt->u.create_table.table.ptr);
            t->table_id = ++cat->next_table_id;
            t->ncols = stmt->u.create_table.ncols;
            t->pk_col = -1;
            for (uint32_t c = 0; c < t->ncols; c++) {
                const vx_col_def_t *cd = &stmt->u.create_table.cols[c];
                snprintf(t->cols[c].name, VX_COL_NAME_MAX, "%.*s", (int)cd->name.len, cd->name.ptr);
                if (!parse_type_name(cd->type_name, &t->cols[c].type)) {
                    vx_table_free(t);
                    return vx_result_new_err("unknown column type");
                }
                t->cols[c].is_key = cd->is_key;
                if (cd->is_key) t->pk_col = (int32_t)c;
                t->cols[c].has_fk = cd->has_fk;
                if (cd->has_fk) {
                    snprintf(t->cols[c].fk_table, VX_COL_NAME_MAX, "%.*s",
                             (int)cd->fk_table.len, cd->fk_table.ptr);
                    snprintf(t->cols[c].fk_col, VX_COL_NAME_MAX, "%.*s",
                             (int)cd->fk_col.len, cd->fk_col.ptr);
                }
            }
            if (t->pk_col >= 0 &&
                (t->cols[t->pk_col].type == VX_TYPE_U64 || t->cols[t->pk_col].type == VX_TYPE_I64)) {
                t->pk_index = vx_pk_index_create();
            }
            cat->tables = (vx_table_t **)realloc(cat->tables, (cat->ntables + 1) * sizeof(*cat->tables));
            cat->tables[cat->ntables++] = t;
            return vx_result_new_ok(0);
        }

        case VX_STMT_DROP_TABLE: {
            for (uint32_t i = 0; i < cat->ntables; i++) {
                if (vx_span_eq_cstr(stmt->u.drop_table.table, cat->tables[i]->name)) {
                    vx_table_free(cat->tables[i]);
                    memmove(&cat->tables[i], &cat->tables[i + 1],
                            (cat->ntables - i - 1) * sizeof(*cat->tables));
                    cat->ntables--;
                    return vx_result_new_ok(0);
                }
            }
            return vx_result_new_err("table not found");
        }

        case VX_STMT_CREATE_INDEX: {
            vx_table_t *t = vx_catalog_find(cat, stmt->u.create_index.table.ptr,
                                            stmt->u.create_index.table.len);
            if (!t) return vx_result_new_err("table not found");
            if (t->nindexes >= VX_MAX_INDEXES) return vx_result_new_err("index limit exceeded");

            vx_index_def_t def;
            memset(&def, 0, sizeof(def));
            def.is_unique = stmt->u.create_index.is_unique;
            def.ncols = stmt->u.create_index.ncols;
            for (uint32_t k = 0; k < def.ncols; k++) {
                int32_t ci = vx_table_find_col(t, stmt->u.create_index.cols[k].ptr,
                                               stmt->u.create_index.cols[k].len);
                if (ci < 0) return vx_result_new_err("unknown column in index");
                def.col_idxs[k] = (uint32_t)ci;
            }
            t->indexes[t->nindexes++] = def;

            /* Back-verify existing live rows when adding a unique constraint */
            if (def.is_unique) {
                vx_val_t row[VX_MAX_COLS];
                for (uint32_t ci = 0; ci < t->nchunks; ci++) {
                    const vx_chunk_t *ch = t->chunks[ci]->chunk;
                    const vx_delmap_t *dm = t->delmaps ? t->delmaps[ci] : NULL;
                    for (uint32_t r = 0; r < ch->nrows; r++) {
                        if (vx_delmap_is_set(dm, r)) continue;
                        for (uint32_t k = 0; k < def.ncols; k++) {
                            uint32_t c = def.col_idxs[k];
                            const vx_col_t *sc = &ch->cols[c];
                            memset(&row[c], 0, sizeof(row[c]));
                            row[c].type = sc->type;
                            row[c].is_null = vx_col_is_null(sc, r);
                            if (row[c].is_null) continue;
                            switch (sc->type) {
                                case VX_TYPE_I64:  row[c].v.i64 = ((const int64_t *)sc->data)[r]; break;
                                case VX_TYPE_U64:  row[c].v.u64 = ((const uint64_t *)sc->data)[r]; break;
                                case VX_TYPE_F64:  row[c].v.f64 = ((const double *)sc->data)[r]; break;
                                case VX_TYPE_BOOL: row[c].v.b   = ((const uint8_t *)sc->data)[r] != 0; break;
                                case VX_TYPE_STR: {
                                    const vx_str_t *s = &((const vx_str_t *)sc->data)[r];
                                    row[c].v.str.ptr = vx_str_data(s);
                                    row[c].v.str.len = s->len;
                                    break;
                                }
                            }
                        }
                        if (!vx_table_check_unique(t, row, (int64_t)ci, (int64_t)r)) {
                            t->nindexes--;
                            return vx_result_new_err("unique constraint violation on existing data");
                        }
                    }
                }
            }
            return vx_result_new_ok(0);
        }

        case VX_STMT_INSERT: {
            vx_table_t *t = vx_catalog_find(cat, stmt->u.insert.table.ptr, stmt->u.insert.table.len);
            if (!t) return vx_result_new_err("table not found");

            uint64_t affected = 0;

            for (uint32_t r = 0; r < stmt->u.insert.nrecords; r++) {
                const vx_record_t *rec = &stmt->u.insert.records[r];
                vx_val_t row[VX_MAX_COLS];
                for (uint32_t c = 0; c < t->ncols; c++) {
                    row[c].type = t->cols[c].type;
                    row[c].is_null = true;
                }
                for (uint32_t f = 0; f < rec->nfields; f++) {
                    int32_t ci = vx_table_find_col(t, rec->fields[f].field.ptr, rec->fields[f].field.len);
                    if (ci < 0) {
                        return vx_result_new_err("unknown column in insert");
                    }
                    vx_val_t v = eval_expr(rec->fields[f].value, NULL, 0, ec);
                    if (ec->failed) { return vx_result_new_err(ec->err); }
                    if (t->cols[ci].type == VX_TYPE_U64 && v.type == VX_TYPE_I64 && !v.is_null) {
                        v.type = VX_TYPE_U64; v.v.u64 = (uint64_t)v.v.i64;
                    } else if (t->cols[ci].type == VX_TYPE_F64 && v.type == VX_TYPE_I64 && !v.is_null) {
                        v.type = VX_TYPE_F64; v.v.f64 = (double)v.v.i64;
                    }
                    row[ci] = v;
                    row[ci].type = t->cols[ci].type;
                }

                bool ok_ins = stmt->u.insert.is_upsert
                    ? vx_table_upsert_row_cow(t, row, db->cfg.chunk_rows, &db->next_chunk_id)
                    : vx_table_append_row_cow(t, row, db->cfg.chunk_rows, &db->next_chunk_id);
                if (!ok_ins) return vx_result_new_err("insert failed");
                affected++;
            }
            return vx_result_new_ok(affected);
        }

        case VX_STMT_UPDATE: {
            vx_table_t *t = vx_catalog_find(cat, stmt->u.update.table.ptr, stmt->u.update.table.len);
            if (!t) return vx_result_new_err("table not found");

            if (stmt->u.update.target.kind == VX_STAGE_PK && t->pk_col >= 0 && t->pk_index) {
                vx_val_t pk_v = eval_expr(stmt->u.update.target.u.pk.key, NULL, 0, ec);
                if (ec->failed) return vx_result_new_err(ec->err);
                uint64_t pk_key = 0;
                if (vx_val_to_pk_u64(&pk_v, t->cols[t->pk_col].type, &pk_key)) {
                    uint64_t packed = 0;
                    if (!vx_pk_index_get(t->pk_index, pk_key, &packed)) {
                        return vx_result_new_ok(0);
                    }
                    uint32_t ci = vx_pk_chunk(packed), ri = vx_pk_row(packed);
                    if (vx_delmap_is_set(t->delmaps[ci], ri)) return vx_result_new_ok(0);

                    vx_wtable_t *one = materialize_single_row(t, ci, ri);
                    if (!one) return vx_result_new_err("out of memory");
                    vx_val_t new_row[VX_MAX_COLS];
                    for (uint32_t c = 0; c < t->ncols; c++) new_row[c] = wtable_get_val(one, c, 0);

                    for (uint32_t f = 0; f < stmt->u.update.patch.nfields; f++) {
                        int32_t col_i = vx_table_find_col(t, stmt->u.update.patch.fields[f].field.ptr,
                                                          stmt->u.update.patch.fields[f].field.len);
                        if (col_i < 0) { wtable_free(one); return vx_result_new_err("unknown column in update"); }
                        vx_val_t nv = eval_expr(stmt->u.update.patch.fields[f].value, one, 0, ec);
                        if (ec->failed) { wtable_free(one); return vx_result_new_err(ec->err); }
                        if (!nv.is_null) {
                            if (t->cols[col_i].type == VX_TYPE_U64 && nv.type == VX_TYPE_I64) {
                                nv.type = VX_TYPE_U64; nv.v.u64 = (uint64_t)nv.v.i64;
                            } else if (t->cols[col_i].type == VX_TYPE_F64 && nv.type == VX_TYPE_I64) {
                                nv.type = VX_TYPE_F64; nv.v.f64 = (double)nv.v.i64;
                            }
                        }
                        nv.type = t->cols[col_i].type;
                        new_row[col_i] = nv;
                    }

                    if (!vx_table_check_unique(t, new_row, (int64_t)ci, (int64_t)ri)) {
                        wtable_free(one);
                        return vx_result_new_err("unique constraint violation");
                    }
                    uint64_t new_pk = 0;
                    bool pk_changed = !vx_val_to_pk_u64(&new_row[t->pk_col],
                                                        t->cols[t->pk_col].type, &new_pk) ||
                                      new_pk != pk_key;
                    if (pk_changed) {
                        uint64_t dup_packed = 0;
                        if (vx_pk_index_get(t->pk_index, new_pk, &dup_packed) &&
                            !vx_delmap_is_set(t->delmaps[vx_pk_chunk(dup_packed)],
                                              vx_pk_row(dup_packed))) {
                            wtable_free(one);
                            return vx_result_new_err("duplicate primary key");
                        }
                    }

                    vx_table_tombstone_row_cow(t, ci, ri);
                    if (pk_changed) {
                        vx_pk_index_del_cow(&t->pk_index, pk_key, NULL);
                    }
                    bool ok_up = vx_table_append_row_cow(t, new_row, db->cfg.chunk_rows, &db->next_chunk_id);
                    wtable_free(one);
                    return ok_up ? vx_result_new_ok(1) : vx_result_new_err("update append failed");
                }
            }

            vx_wtable_t *wt = materialize_table(t);

            vx_val_t pk_target = {0};
            if (stmt->u.update.target.kind == VX_STAGE_PK) {
                pk_target = eval_expr(stmt->u.update.target.u.pk.key, wt, 0, ec);
                if (ec->failed) { wtable_free(wt); return vx_result_new_err(ec->err); }
            }

            int64_t sl_start = 0, sl_stop = (int64_t)wt->nrows;
            if (stmt->u.update.target.kind == VX_STAGE_SLICE) {
                if (stmt->u.update.target.u.slice.has_start) sl_start = stmt->u.update.target.u.slice.start;
                if (stmt->u.update.target.u.slice.has_stop)  sl_stop  = stmt->u.update.target.u.slice.stop;
                if (sl_start < 0) sl_start = 0;
                if (sl_stop > (int64_t)wt->nrows) sl_stop = (int64_t)wt->nrows;
            }

            uint64_t affected = 0;
            for (uint32_t r = 0; r < wt->nrows; r++) {
                bool match = false;
                if (stmt->u.update.target.kind == VX_STAGE_SLICE) {
                    match = ((int64_t)r >= sl_start && (int64_t)r < sl_stop);
                } else if (stmt->u.update.target.kind == VX_STAGE_PK) {
                    int32_t pkc = t->pk_col >= 0 ? t->pk_col : 0;
                    vx_val_t rv = wtable_get_val(wt, (uint32_t)pkc, r);
                    match = !rv.is_null && compare_vals(&rv, &pk_target) == 0;
                } else {
                    vx_val_t v = eval_expr(stmt->u.update.target.u.filter.pred, wt, r, ec);
                    if (ec->failed) { wtable_free(wt); return vx_result_new_err(ec->err); }
                    match = !v.is_null && v.type == VX_TYPE_BOOL && v.v.b;
                }
                if (match) {
                    for (uint32_t f = 0; f < stmt->u.update.patch.nfields; f++) {
                        int32_t ci = vx_table_find_col(t, stmt->u.update.patch.fields[f].field.ptr,
                                                       stmt->u.update.patch.fields[f].field.len);
                        if (ci < 0) { wtable_free(wt); return vx_result_new_err("unknown column in update"); }
                        vx_val_t nv = eval_expr(stmt->u.update.patch.fields[f].value, wt, r, ec);
                        if (ec->failed) { wtable_free(wt); return vx_result_new_err(ec->err); }
                        wtable_set_val(wt, (uint32_t)ci, r, &nv);
                    }
                    affected++;
                }
            }
            if (!rebuild_table_from_wtable(t, wt, db->cfg.chunk_rows, &db->next_chunk_id)) {
                wtable_free(wt);
                return vx_result_new_err("constraint violation in update");
            }
            wtable_free(wt);
            return vx_result_new_ok(affected);
        }

        case VX_STMT_DELETE: {
            vx_table_t *t = vx_catalog_find(cat, stmt->u.del.table.ptr, stmt->u.del.table.len);
            if (!t) return vx_result_new_err("table not found");

            if (stmt->u.del.target.kind == VX_STAGE_PK && t->pk_col >= 0 && t->pk_index) {
                vx_val_t pk_v = eval_expr(stmt->u.del.target.u.pk.key, NULL, 0, ec);
                if (ec->failed) return vx_result_new_err(ec->err);
                uint64_t pk_key = 0;
                if (vx_val_to_pk_u64(&pk_v, t->cols[t->pk_col].type, &pk_key)) {
                    uint64_t packed = 0;
                    if (!vx_pk_index_del_cow(&t->pk_index, pk_key, &packed)) {
                        return vx_result_new_ok(0);
                    }
                    vx_table_tombstone_row_cow(t, vx_pk_chunk(packed), vx_pk_row(packed));
                    return vx_result_new_ok(1);
                }
            }

            vx_wtable_t *in = materialize_table(t);
            vx_wtable_t *keep = wtable_create(in->ncols, in->nrows ? in->nrows : 16U, in->str_arena.cap);
            for (uint32_t c = 0; c < in->ncols; c++) {
                wtable_init_col(keep, c, in->cols[c].table, in->cols[c].name, in->cols[c].type);
            }

            vx_val_t pk_target = {0};
            if (stmt->u.del.target.kind == VX_STAGE_PK) {
                pk_target = eval_expr(stmt->u.del.target.u.pk.key, in, 0, ec);
                if (ec->failed) { wtable_free(in); wtable_free(keep); return vx_result_new_err(ec->err); }
            }

            int64_t sl_start = 0, sl_stop = (int64_t)in->nrows;
            if (stmt->u.del.target.kind == VX_STAGE_SLICE) {
                if (stmt->u.del.target.u.slice.has_start) sl_start = stmt->u.del.target.u.slice.start;
                if (stmt->u.del.target.u.slice.has_stop)  sl_stop  = stmt->u.del.target.u.slice.stop;
                if (sl_start < 0) sl_start = 0;
                if (sl_stop > (int64_t)in->nrows) sl_stop = (int64_t)in->nrows;
            }

            uint64_t deleted = 0;
            for (uint32_t r = 0; r < in->nrows; r++) {
                bool del_row = false;
                if (stmt->u.del.target.kind == VX_STAGE_SLICE) {
                    del_row = ((int64_t)r >= sl_start && (int64_t)r < sl_stop);
                } else if (stmt->u.del.target.kind == VX_STAGE_PK) {
                    int32_t pkc = t->pk_col >= 0 ? t->pk_col : 0;
                    vx_val_t rv = wtable_get_val(in, (uint32_t)pkc, r);
                    del_row = !rv.is_null && compare_vals(&rv, &pk_target) == 0;
                } else {
                    vx_val_t v = eval_expr(stmt->u.del.target.u.filter.pred, in, r, ec);
                    if (ec->failed) { wtable_free(in); wtable_free(keep); return vx_result_new_err(ec->err); }
                    del_row = !v.is_null && v.type == VX_TYPE_BOOL && v.v.b;
                }
                if (del_row) {
                    deleted++;
                } else {
                    uint32_t dr = keep->nrows++;
                    for (uint32_t c = 0; c < in->ncols; c++) {
                        vx_val_t cv = wtable_get_val(in, c, r);
                        wtable_set_val(keep, c, dr, &cv);
                    }
                }
            }
            rebuild_table_from_wtable(t, keep, db->cfg.chunk_rows, &db->next_chunk_id);
            wtable_free(in);
            wtable_free(keep);
            return vx_result_new_ok(deleted);
        }

        default:
            return vx_result_new_err("unsupported write statement");
    }
}

/* ==================== PROGRAM ENTRYPOINT ==================== */

vx_result_t *vx_exec_program(vx_db_t *db, const vx_program_t *prog,
                             const char *raw_query, size_t raw_len,
                             const vx_param_t *params, size_t nparams,
                             bool sync, bool replaying) {
    (void)sync;
    if (!prog || prog->nstmts == 0) {
        return vx_result_new_ok(0);
    }

    eval_ctx_t ec = { .params = params, .nparams = nparams, .failed = false };

    /* Check if any statement is a write/DDL */
    bool has_write = false;
    for (uint32_t i = 0; i < prog->nstmts; i++) {
        if (prog->stmts[i]->kind != VX_STMT_QUERY) {
            has_write = true;
            break;
        }
    }

    if (has_write) {
        /* Flush SPSC write mesh BEFORE acquiring write_mu (preserves lock hierarchy) */
        if (!replaying && db->wmesh) {
            vx_wmesh_flush(db->wmesh);
        }

        pthread_mutex_lock(&db->write_mu);

        const vx_catalog_t *cur = (const vx_catalog_t *)vx_ebr_current_root(db->ebr);
        vx_catalog_t *next = vx_catalog_clone(cur);
        vx_result_t *last_res = NULL;

        for (uint32_t i = 0; i < prog->nstmts; i++) {
            vx_result_free(last_res);
            last_res = exec_write_stmt(db, next, prog->stmts[i], &ec);
            if (!last_res || !last_res->ok) {
                vx_catalog_free(next);
                pthread_mutex_unlock(&db->write_mu);
                return last_res ? last_res : vx_result_new_err("write failed");
            }
        }

        if (!replaying && db->wal && raw_query && raw_len > 0) {
            bool logged = (nparams > 0)
                ? vx_db_wal_log_query_params(db, raw_query, raw_len, params, nparams)
                : vx_db_wal_log(db, VX_WAL_OP_QUERY, 0,
                                (const uint8_t *)raw_query, (uint32_t)raw_len);
            if (!logged) {
                vx_catalog_free(next);
                vx_result_free(last_res);
                pthread_mutex_unlock(&db->write_mu);
                return vx_result_new_err("WAL append failed");
            }
        }

        void *old = vx_ebr_publish(db->ebr, next);
        vx_ebr_retire(db->ebr, old, vx_catalog_ebr_free, NULL);
        vx_ebr_reclaim(db->ebr);
        atomic_store_explicit(&db->dead_rows, vx_catalog_dead_rows(next), memory_order_release);
        atomic_fetch_add_explicit(&db->mutations_committed, 1, memory_order_relaxed);

        if (!replaying && db->write_log_tx) {
            vx_log_emit(db->write_log_tx, VX_LOG_INFO, VX_LOG_EV_WRITE_STMT,
                        "write_stmt", prog->nstmts, db->next_lsn, 0, 0);
        }

        pthread_mutex_unlock(&db->write_mu);
        return last_res;
    }

    vx_ebr_reader_t *reader = vx_db_get_reader(db);
    if (!reader) {
        return vx_result_new_err("max_readers limit exceeded");
    }
    const vx_catalog_t *cat = (const vx_catalog_t *)vx_ebr_enter(reader);

    typedef struct {
        vx_span_t    name;
        vx_wtable_t *wt;
    } binding_t;
    binding_t bindings[16];
    uint32_t nbind = 0;
    vx_wtable_t *last_wt = NULL;
    bool explain = false;

    for (uint32_t si = 0; si < prog->nstmts; si++) {
        const vx_stmt_t *st = prog->stmts[si];
        explain = st->u.query.explain;

        vx_wtable_t *cur = NULL;
        const vx_table_t *src_schema = NULL;
        uint32_t start_stage = 0;

        /* Check bound variables first */
        for (uint32_t b = 0; b < nbind; b++) {
            if (bindings[b].name.len == st->u.query.source_table.len &&
                memcmp(bindings[b].name.ptr, st->u.query.source_table.ptr, bindings[b].name.len) == 0) {
                cur = bindings[b].wt;
                bindings[b].wt = NULL; /* move ownership */
                break;
            }
        }

        if (!cur) {
            src_schema = vx_catalog_find(cat, st->u.query.source_table.ptr,
                                         st->u.query.source_table.len);
            if (!src_schema) {
                vx_ebr_exit(reader);
                for (uint32_t b = 0; b < nbind; b++) wtable_free(bindings[b].wt);
                wtable_free(last_wt);
                return vx_result_new_err("table not found");
            }

            cur = try_pk_fast_path(src_schema, st->u.query.stages,
                                   st->u.query.nstages, &ec, &start_stage);
            if (ec.failed) {
                vx_ebr_exit(reader);
                for (uint32_t b = 0; b < nbind; b++) wtable_free(bindings[b].wt);
                wtable_free(last_wt);
                return vx_result_new_err(ec.err);
            }
            if (!cur) {
                cur = try_morsel_fast_path(db, src_schema, st->u.query.stages,
                                           st->u.query.nstages, &start_stage);
            }
            if (!cur) {
                cur = materialize_table(src_schema);
            }
        }

        for (uint32_t j = start_stage; j < st->u.query.nstages; j++) {
            const vx_stage_t *stg = &st->u.query.stages[j];
            vx_wtable_t *nxt = NULL;
            switch (stg->kind) {
                case VX_STAGE_PK:
                case VX_STAGE_FILTER:
                    nxt = stage_filter_or_pk(cur, stg, src_schema ? src_schema->pk_col : -1, &ec);
                    wtable_free(cur);
                    cur = nxt;
                    break;
                case VX_STAGE_SLICE:
                    nxt = stage_slice(cur, stg);
                    wtable_free(cur);
                    cur = nxt;
                    break;
                case VX_STAGE_PROJECT:
                    nxt = stage_project(cur, stg, &ec);
                    wtable_free(cur);
                    cur = nxt;
                    break;
                case VX_STAGE_SORT:
                    cur = stage_sort(cur, stg, &ec);
                    break;
                case VX_STAGE_GROUP:
                    nxt = stage_group(cur, stg, &ec);
                    wtable_free(cur);
                    cur = nxt;
                    break;
                case VX_STAGE_JOIN:
                    nxt = stage_join(cur, cat, src_schema, stg, &ec);
                    wtable_free(cur);
                    cur = nxt;
                    src_schema = NULL;
                    break;
            }
            if (ec.failed || !cur) {
                vx_ebr_exit(reader);
                for (uint32_t b = 0; b < nbind; b++) wtable_free(bindings[b].wt);
                wtable_free(last_wt);
                wtable_free(cur);
                return vx_result_new_err(ec.err[0] ? ec.err : "execution error");
            }
        }

        if (st->u.query.bind_name.len > 0 && nbind < 16U) {
            bindings[nbind++] = (binding_t){ .name = st->u.query.bind_name, .wt = cur };
        } else {
            wtable_free(last_wt);
            last_wt = cur;
        }
    }

    vx_ebr_exit(reader);
    for (uint32_t b = 0; b < nbind; b++) wtable_free(bindings[b].wt);
    atomic_fetch_add_explicit(&db->queries_executed, 1, memory_order_relaxed);

    vx_result_t *res = last_wt ? wtable_to_result(last_wt) : vx_result_new_ok(0);
    if (res && explain) {
        res->explain_text = (char *)malloc(256);
        if (res->explain_text) {
            snprintf(res->explain_text, 256,
                     "PLAN: morsel_workers=%u chunk_rows=%u rows_out=%u cols=%u",
                     db->pool.nworkers, db->cfg.chunk_rows, res->nrows, res->ncols);
        }
    }
    return res;
}
