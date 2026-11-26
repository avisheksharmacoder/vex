#include <math.h>
#include <string.h>
#include "vx_op_filter.h"

extern void vx_batch_forward_shallow(const vx_batch_t *in, vx_batch_t *out);

/* Evaluates a scalar comparison against a VX_VEC_CONST column in O(1). */
static bool vx_eval_const_pred(const vx_vec_t *v, const vx_filter_pred_t *p) {
    bool is_null = (v->has_nulls && v->validity && ((v->validity[0] & 1ULL) == 0));
    if (p->kind == VX_PRED_IS_NULL)     return is_null;
    if (p->kind == VX_PRED_IS_NOT_NULL) return !is_null;
    if (is_null) return false;

    switch (p->type) {
    case VX_TYPE_I64: {
        int64_t a = *(const int64_t *)v->data, b = p->lit.i64;
        switch (p->cmp) {
        case VX_CMP_EQ: return a == b; case VX_CMP_NE: return a != b;
        case VX_CMP_LT: return a <  b; case VX_CMP_LE: return a <= b;
        case VX_CMP_GT: return a >  b; case VX_CMP_GE: return a >= b;
        }
        break;
    }
    case VX_TYPE_U64: {
        uint64_t a = *(const uint64_t *)v->data, b = p->lit.u64;
        switch (p->cmp) {
        case VX_CMP_EQ: return a == b; case VX_CMP_NE: return a != b;
        case VX_CMP_LT: return a <  b; case VX_CMP_LE: return a <= b;
        case VX_CMP_GT: return a >  b; case VX_CMP_GE: return a >= b;
        }
        break;
    }
    case VX_TYPE_F64: {
        double a = *(const double *)v->data, b = p->lit.f64;
        switch (p->cmp) {
        case VX_CMP_EQ: return a == b; case VX_CMP_NE: return a != b;
        case VX_CMP_LT: return a <  b; case VX_CMP_LE: return a <= b;
        case VX_CMP_GT: return a >  b; case VX_CMP_GE: return a >= b;
        }
        break;
    }
    case VX_TYPE_BOOL: {
        uint8_t a = *(const uint8_t *)v->data, b = p->lit.b;
        return (p->cmp == VX_CMP_NE) ? (a != b) : (a == b);
    }
    case VX_TYPE_STR: {
        const vx_strview_t *sv = (const vx_strview_t *)v->data;
        bool eq = (sv->inl.len == p->lit.str.len) &&
                  (sv->inl.len == 0 ||
                   memcmp(vx_strview_ptr(sv, v->strpool), p->lit.str.ptr, (size_t)sv->inl.len) == 0);
        return p->negate ? !eq : eq;
    }
    }
    return false;
}

/* Filters in->sel using a dense VX_TYPE_BOOL vector produced by vx_builtin_eval(). */
static void vx_filter_by_dense_bool(const vx_vec_t *bool_vec, const vx_simd_ops_t *simd,
                                     bool want_true, vx_sel_t *sel) {
    vx_sel_t dense_in = { .count = sel->count, .is_identity = true };
    vx_sel_t dense_hit;

    uint32_t nhit = simd->filter_bool((const uint8_t *)bool_vec->data,
                                      bool_vec->has_nulls ? bool_vec->validity : NULL,
                                      &dense_in, &dense_hit,
                                      VX_CMP_EQ, want_true ? 1u : 0u);
    if (sel->is_identity) {
        memcpy(sel->idx, dense_hit.idx, (size_t)nhit * sizeof(uint32_t));
        sel->is_identity = (nhit == sel->count);
    } else {
        /* dense_hit.idx[k] >= k strictly, so remapping sel->idx in place is alias-safe. */
        for (uint32_t k = 0; k < nhit; ++k) {
            sel->idx[k] = sel->idx[dense_hit.idx[k]];
        }
        sel->is_identity = false;
    }
    sel->count = nhit;
}

static vx_status_t vx_filter_init_global(const vx_op_t *op, vx_exec_ctx_t *ctx, void **gstate) {
    (void)op; (void)ctx; *gstate = NULL; return VX_OK;
}

static vx_status_t vx_filter_init_local(const vx_op_t *op, vx_exec_ctx_t *ctx,
                                         void *gstate, void **lstate) {
    (void)op; (void)ctx; (void)gstate; *lstate = NULL; return VX_OK;
}

static vx_op_res_t vx_filter_execute(const vx_op_t *op, vx_exec_ctx_t *ctx,
                                      void *gstate, void *lstate,
                                      vx_batch_t *in, vx_batch_t *out) {
    (void)gstate; (void)lstate;
    const vx_filter_cfg_t *cfg  = (const vx_filter_cfg_t *)op->plan_cfg;
    const vx_simd_ops_t   *simd = ctx->simd;

    if (in->sel.count == 0) {
        vx_batch_reset(out);
        return VX_OP_NEED_MORE_INPUT;
    }

    for (uint32_t p = 0; p < cfg->npreds && in->sel.count > 0; ++p) {
        const vx_filter_pred_t *pr = &cfg->preds[p];

        if (pr->kind == VX_PRED_BUILTIN_BOOL) {
            vx_vec_t args[4];
            for (uint32_t a = 0; a < pr->nargs; ++a) {
                if (pr->args[a].is_col) {
                    vx_status_t st = vx_batch_materialize_col(in, pr->args[a].batch_col_idx,
                                                              pr->args[a].table_col_idx,
                                                              ctx->scratch);
                    if (st != VX_OK) {
                        if (ctx->err) ctx->err->code = st;
                        return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
                    }
                    args[a] = in->cols[pr->args[a].batch_col_idx];
                } else {
                    args[a] = pr->args[a].lit_vec;
                }
            }
            vx_vec_t res_bool;
            memset(&res_bool, 0, sizeof(res_bool));
            vx_status_t st = vx_builtin_eval(pr->builtin_id, args, pr->nargs,
                                             &in->sel, ctx->scratch, &res_bool);
            if (st != VX_OK) {
                vx_owner_release(res_bool.owner);
                if (ctx->err) ctx->err->code = st;
                return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
            }
            vx_filter_by_dense_bool(&res_bool, simd, !pr->negate, &in->sel);
            vx_owner_release(res_bool.owner);
            continue;
        }

        /* Staged late materialization: touch deferred predicate columns only if rows remain */
        vx_status_t st = vx_batch_materialize_col(in, pr->batch_col_idx,
                                                  pr->table_col_idx, ctx->scratch);
        if (st != VX_OK) {
            if (ctx->err) ctx->err->code = st;
            return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
        }

        const vx_vec_t *col = &in->cols[pr->batch_col_idx];
        if (col->kind == VX_VEC_CONST) {
            if (!vx_eval_const_pred(col, pr)) in->sel.count = 0;
            continue;
        }

        const uint64_t *val = col->has_nulls ? col->validity : NULL;

        if (pr->kind == VX_PRED_IS_NULL || pr->kind == VX_PRED_IS_NOT_NULL) {
            bool want_null = (pr->kind == VX_PRED_IS_NULL);
            if (!val) {
                if (want_null) in->sel.count = 0;
            } else {
                simd->filter_null(val, &in->sel, &in->sel, want_null);
            }
            continue;
        }

        /* VX_PRED_COL_LIT: in-place SIMD filter on &in->sel */
        switch (pr->type) {
        case VX_TYPE_I64:
            simd->filter_i64((const int64_t *)col->data, val,
                             &in->sel, &in->sel, pr->cmp, pr->lit.i64);
            break;
        case VX_TYPE_U64:
            simd->filter_u64((const uint64_t *)col->data, val,
                             &in->sel, &in->sel, pr->cmp, pr->lit.u64);
            break;
        case VX_TYPE_F64:
            simd->filter_f64((const double *)col->data, val,
                             &in->sel, &in->sel, pr->cmp, pr->lit.f64);
            break;
        case VX_TYPE_BOOL:
            simd->filter_bool((const uint8_t *)col->data, val,
                              &in->sel, &in->sel, pr->cmp, pr->lit.b);
            break;
        case VX_TYPE_STR:
            if (col->kind == VX_VEC_DICT) {
                /* Dictionary predicate pushdown: evaluate once over dict_vals[0..dict_size-1] */
                uint64_t match_bits[64];
                memset(match_bits, 0, sizeof(match_bits));
                const vx_strview_t *dv = (const vx_strview_t *)col->dict_vals;
                for (uint32_t d = 0; d < col->dict_size && d < 4096u; ++d) {
                    bool eq = (dv[d].inl.len == pr->lit.str.len) &&
                              (dv[d].inl.len == 0 ||
                               memcmp(vx_strview_ptr(&dv[d], col->strpool),
                                      pr->lit.str.ptr, (size_t)dv[d].inl.len) == 0);
                    if (pr->negate ? !eq : eq) {
                        match_bits[d >> 6] |= (1ULL << (d & 63u));
                    }
                }
                const uint32_t *codes = (const uint32_t *)col->data;
                uint32_t nhit = 0;
                for (uint32_t i = 0; i < in->sel.count; ++i) {
                    uint32_t r = in->sel.is_identity ? i : in->sel.idx[i];
                    if (val && ((val[r >> 6] >> (r & 63u)) & 1ULL) == 0) continue;
                    uint32_t code = codes[r];
                    if (code < 4096u && ((match_bits[code >> 6] >> (code & 63u)) & 1ULL)) {
                        in->sel.idx[nhit++] = r;
                    }
                }
                in->sel.count = nhit;
                in->sel.is_identity = false;
            } else {
                simd->filter_str_eq((const vx_strview_t *)col->data, col->strpool, val,
                                    &in->sel, &in->sel, pr->lit.str.ptr, pr->lit.str.len,
                                    pr->negate);
            }
            break;
        }
    }

    if (in->sel.count == 0) {
        vx_batch_reset(out);
        return VX_OP_NEED_MORE_INPUT;
    }

    vx_batch_forward_shallow(in, out);
    return VX_OP_NEED_MORE_INPUT;
}

static void vx_filter_destroy_local(void *lstate)  { (void)lstate; }
static void vx_filter_destroy_global(void *gstate) { (void)gstate; }

const vx_op_vtbl_t VX_OP_FILTER = {
    .name           = "Filter",
    .init_global    = vx_filter_init_global,
    .init_local     = vx_filter_init_local,
    .execute        = vx_filter_execute,
    .destroy_local  = vx_filter_destroy_local,
    .destroy_global = vx_filter_destroy_global
};
