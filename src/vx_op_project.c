#include <string.h>
#include "vx_op_project.h"

extern vx_status_t vx_vec_compact(const vx_vec_t *in, const vx_sel_t *sel,
                                  vx_arena_t *dst_arena, vx_vec_t *out);

static vx_status_t vx_proj_init_global(const vx_op_t *op, vx_exec_ctx_t *ctx, void **gstate) {
    (void)op; (void)ctx; *gstate = NULL; return VX_OK;
}

static vx_status_t vx_proj_init_local(const vx_op_t *op, vx_exec_ctx_t *ctx,
                                      void *gstate, void **lstate) {
    (void)op; (void)ctx; (void)gstate; *lstate = NULL; return VX_OK;
}

static vx_op_res_t vx_proj_execute(const vx_op_t *op, vx_exec_ctx_t *ctx,
                                   void *gstate, void *lstate,
                                   vx_batch_t *in, vx_batch_t *out) {
    (void)gstate; (void)lstate;
    const vx_project_cfg_t *cfg = (const vx_project_cfg_t *)op->plan_cfg;

    vx_batch_reset(out);
    if (in->sel.count == 0) {
        return VX_OP_NEED_MORE_INPUT;
    }

    /* When computed built-ins run on a sparse sel, built-ins output dense [0..n-1].
     * Normalize pass-through columns to dense [0..n-1] so all output columns share one sel. */
    const bool normalize_dense = cfg->has_computed && !in->sel.is_identity;

    out->ncols = cfg->n_items;
    if (normalize_dense) {
        out->sel.count       = in->sel.count;
        out->sel.is_identity = true;
    } else {
        out->sel = in->sel;
    }

    vx_status_t st = VX_OK;
    for (uint32_t k = 0; k < cfg->n_items; ++k) {
        const vx_proj_item_t *it = &cfg->items[k];

        switch (it->kind) {
        case VX_PROJ_COL:
            /* Late materialization: fault in deferred column only for surviving rows */
            st = vx_batch_materialize_col(in, it->batch_col_idx, it->table_col_idx, ctx->scratch);
            if (st != VX_OK) goto fail;

            if (normalize_dense && in->cols[it->batch_col_idx].kind != VX_VEC_CONST) {
                st = vx_vec_compact(&in->cols[it->batch_col_idx], &in->sel,
                                    ctx->scratch, &out->cols[k]);
                if (st != VX_OK) goto fail;
            } else {
                /* Zero-copy pass-through */
                out->cols[k]       = in->cols[it->batch_col_idx];
                out->cols[k].owner = vx_owner_retain(out->cols[k].owner);
            }
            break;

        case VX_PROJ_LITERAL:
            out->cols[k]       = it->lit_vec;
            out->cols[k].owner = vx_owner_retain(it->lit_vec.owner);
            break;

        case VX_PROJ_BUILTIN: {
            vx_vec_t args[4];
            for (uint32_t a = 0; a < it->nargs; ++a) {
                if (it->args[a].is_col) {
                    st = vx_batch_materialize_col(in, it->args[a].batch_col_idx,
                                                  it->args[a].table_col_idx, ctx->scratch);
                    if (st != VX_OK) goto fail;
                    args[a] = in->cols[it->args[a].batch_col_idx];
                } else {
                    args[a] = it->args[a].lit_vec;
                }
            }
            st = vx_builtin_eval(it->builtin_id, args, it->nargs,
                                 &in->sel, ctx->scratch, &out->cols[k]);
            if (st != VX_OK) goto fail;
            break;
        }
        }
    }

    return VX_OP_NEED_MORE_INPUT;

fail:
    vx_batch_reset(out);
    if (ctx->err) ctx->err->code = st;
    return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
}

static void vx_proj_destroy_local(void *lstate)  { (void)lstate; }
static void vx_proj_destroy_global(void *gstate) { (void)gstate; }

const vx_op_vtbl_t VX_OP_PROJECT = {
    .name           = "Project",
    .init_global    = vx_proj_init_global,
    .init_local     = vx_proj_init_local,
    .execute        = vx_proj_execute,
    .destroy_local  = vx_proj_destroy_local,
    .destroy_global = vx_proj_destroy_global
};
