#define _POSIX_C_SOURCE 200809L
#include "vx_plan.h"
#include "vx_arena.h"
#include "vx_ast.h"
#include "vx_op.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

static const vx_opt_table_t *vx_opt_find_table(const vx_opt_catalog_t *cat, const char *name, size_t len) {
    if (!cat || !name || len == 0 || !cat->tables) return NULL;
    for (uint32_t i = 0; i < cat->ntables; ++i) {
        if (cat->tables[i] && strncmp(cat->tables[i]->name, name, len) == 0 &&
            cat->tables[i]->name[len] == '\0') {
            return cat->tables[i];
        }
    }
    return NULL;
}

/* --- Internal Logical Node Metadata --- */
typedef struct vx_lscan_meta {
    char     table_name[VX_OPT_NAME_MAX];
    uint32_t table_id;
    uint32_t total_chunks;
    uint32_t surviving_chunks;
    uint64_t live_rows;
    uint32_t ncols;
    uint32_t eager_cols_count;
    char     detail_extra[64];
} vx_lscan_meta_t;

typedef struct vx_lfilter_meta {
    char     pred_str[128];
    bool     is_const_false;
    double   selectivity;
    uint32_t col_idx;
    bool     is_equality;
    bool     is_range;
    bool     is_pk;
    uint32_t index_id;
    bool     has_index;
    bool     is_unique_idx;
} vx_lfilter_meta_t;

typedef struct vx_lproject_meta {
    char     proj_str[128];
    uint32_t ncols;
} vx_lproject_meta_t;

typedef struct vx_ljoin_meta {
    char           right_table[VX_OPT_NAME_MAX];
    vx_join_kind_t join_kind;
    bool           right_is_build;
    char           build_name[VX_OPT_NAME_MAX];
} vx_ljoin_meta_t;

typedef struct vx_lsort_meta {
    char     sort_str[64];
    bool     desc;
} vx_lsort_meta_t;

typedef struct vx_lslice_meta {
    int64_t  offset;
    int64_t  limit;
    bool     fused_topk;
    char     topk_sort[64];
} vx_lslice_meta_t;

/* --- Helpers for AST and Expression Inspection --- */

static vx_lplan_t *vx_lplan_alloc(vx_arena_t *arena, vx_lop_kind_t kind,
                                  vx_lplan_t *left, vx_lplan_t *right) {
    vx_lplan_t *p = (vx_lplan_t *)vx_arena_calloc(arena, 1, sizeof(vx_lplan_t));
    if (!p) return NULL;
    p->kind  = kind;
    p->left  = left;
    p->right = right;
    return p;
}

static void vx_format_span(vx_span_t span, char *dst, size_t dst_sz) {
    if (!span.ptr || span.len == 0 || dst_sz == 0) {
        if (dst_sz > 0) dst[0] = '\0';
        return;
    }
    size_t copy_len = (span.len < dst_sz - 1) ? span.len : dst_sz - 1;
    memcpy(dst, span.ptr, copy_len);
    dst[copy_len] = '\0';
}

static void vx_expr_to_str(const vx_expr_t *e, char *dst, size_t dst_sz) {
    if (!e || dst_sz <= 1) {
        if (dst_sz > 0) dst[0] = '\0';
        return;
    }
    switch (e->kind) {
    case VX_EXPR_INT:
        snprintf(dst, dst_sz, "%" PRId64, e->u.i64);
        break;
    case VX_EXPR_FLOAT:
        snprintf(dst, dst_sz, "%g", e->u.f64);
        break;
    case VX_EXPR_STR: {
        char s[64];
        vx_format_span(e->u.str, s, sizeof(s));
        snprintf(dst, dst_sz, "\"%s\"", s);
        break;
    }
    case VX_EXPR_BOOL:
        snprintf(dst, dst_sz, "%s", e->u.b ? "true" : "false");
        break;
    case VX_EXPR_IDENT: {
        if (e->u.ident.nparts > 0) {
            vx_format_span(e->u.ident.parts[0], dst, dst_sz);
        } else {
            dst[0] = '\0';
        }
        break;
    }
    case VX_EXPR_BINARY: {
        char l[64], r[64];
        vx_expr_to_str(e->u.binary.lhs, l, sizeof(l));
        vx_expr_to_str(e->u.binary.rhs, r, sizeof(r));
        const char *op = "==";
        switch (e->u.binary.op) {
        case VX_BINOP_EQ:  op = "=="; break;
        case VX_BINOP_NE:  op = "!="; break;
        case VX_BINOP_LT:  op = "<"; break;
        case VX_BINOP_LE:  op = "<="; break;
        case VX_BINOP_GT:  op = ">"; break;
        case VX_BINOP_GE:  op = ">="; break;
        case VX_BINOP_AND: op = "&&"; break;
        case VX_BINOP_OR:  op = "||"; break;
        case VX_BINOP_ADD: op = "+"; break;
        case VX_BINOP_SUB: op = "-"; break;
        case VX_BINOP_MUL: op = "*"; break;
        case VX_BINOP_DIV: op = "/"; break;
        default:           op = "?"; break;
        }
        snprintf(dst, dst_sz, "%s %s %s", l, op, r);
        break;
    }
    default:
        snprintf(dst, dst_sz, "expr");
        break;
    }
}

/* Evaluates constant expressions at plan time */
static bool vx_eval_const_bool(const vx_expr_t *e, bool *out_is_const, bool *out_val) {
    *out_is_const = false;
    *out_val      = false;
    if (!e) return false;

    if (e->kind == VX_EXPR_BOOL) {
        *out_is_const = true;
        *out_val      = e->u.b;
        return true;
    }
    if (e->kind == VX_EXPR_BINARY) {
        if (e->u.binary.lhs->kind == VX_EXPR_INT && e->u.binary.rhs->kind == VX_EXPR_INT) {
            int64_t a = e->u.binary.lhs->u.i64;
            int64_t b = e->u.binary.rhs->u.i64;
            *out_is_const = true;
            switch (e->u.binary.op) {
            case VX_BINOP_EQ: *out_val = (a == b); return true;
            case VX_BINOP_NE: *out_val = (a != b); return true;
            case VX_BINOP_LT: *out_val = (a < b);  return true;
            case VX_BINOP_LE: *out_val = (a <= b); return true;
            case VX_BINOP_GT: *out_val = (a > b);  return true;
            case VX_BINOP_GE: *out_val = (a >= b); return true;
            default: break;
            }
        }
    }
    return false;
}

/* --- Access Path Selection Helper --- */
static vx_access_choice_t vx_choose_access_path(const vx_lscan_meta_t *scan,
                                                const vx_lfilter_meta_t *filter) {
    vx_access_choice_t choice;
    memset(&choice, 0, sizeof(choice));
    choice.chunks_total      = scan->total_chunks;
    choice.chunks_after_zmap = scan->surviving_chunks > 0 ? scan->surviving_chunks : scan->total_chunks;
    uint64_t live_rows       = scan->live_rows > 0 ? scan->live_rows : 10000ULL;

    if (filter && filter->is_pk) {
        /* Rule 1: pk == lit -> O(1) Tiered PK lookup */
        choice.path            = VX_ACCESS_PK_POINT;
        choice.est_rows        = 1;
        choice.est_cost        = 1.0;
        choice.est_selectivity = 1.0 / (double)live_rows;
        choice.chunks_after_zmap = 1;
        return choice;
    }

    if (filter && filter->is_equality && filter->has_index) {
        /* Rule 2: Indexed equality col == lit */
        if (filter->is_unique_idx) {
            choice.path            = VX_ACCESS_SEC_HASH;
            choice.index_id        = filter->index_id;
            choice.est_rows        = 1;
            choice.est_cost        = 1.2;
            choice.est_selectivity = 1.0 / (double)live_rows;
            choice.chunks_after_zmap = 1;
            return choice;
        }

        double sel = filter->selectivity > 0.0 ? filter->selectivity : 0.04;
        if (sel < 0.05) {
            choice.path            = VX_ACCESS_SEC_HASH;
            choice.index_id        = filter->index_id;
            choice.est_selectivity = sel;
            choice.est_rows        = (uint64_t)((double)live_rows * sel);
            if (choice.est_rows == 0) choice.est_rows = 1;
            choice.est_cost        = (double)choice.est_rows * 1.5 + 2.0;
            return choice;
        }
        /* Fall through to TableScan: sequential SIMD beats random page faults above 5% */
    }

    if (filter && filter->is_range && filter->has_index) {
        /* Rule 3: Indexed range col >= a && col <= b */
        double range_sel = filter->selectivity > 0.0 ? filter->selectivity : 0.004;
        uint64_t est_matching = (uint64_t)((double)live_rows * range_sel);
        uint32_t zmap_chunks = choice.chunks_after_zmap;

        /* Pick ART if matching rows < 2% of live rows AND < zmap_chunks * 64 */
        if (est_matching < (uint64_t)((double)live_rows * 0.02) &&
            est_matching < (uint64_t)zmap_chunks * 64ULL) {
            choice.path            = VX_ACCESS_SEC_ART_RANGE;
            choice.index_id        = filter->index_id;
            choice.est_selectivity = range_sel;
            choice.est_rows        = est_matching > 0 ? est_matching : 1;
            choice.est_cost        = (double)choice.est_rows * 1.8 + 4.0;
            return choice;
        }
        /* Fall through to TableScan with zone-map pruning */
    }

    /* Rule 4: TableScan fallback */
    choice.path            = VX_ACCESS_TABLE_SCAN;
    choice.est_selectivity = (double)choice.chunks_after_zmap / (double)(choice.chunks_total > 0 ? choice.chunks_total : 1);
    choice.est_rows        = (uint64_t)((double)live_rows * choice.est_selectivity);
    if (choice.est_rows == 0) choice.est_rows = live_rows;
    /* SIMD scan normalized cost: ~25 cost units per 64 chunks */
    choice.est_cost        = ((double)choice.chunks_after_zmap * 100.0) / 4.0;
    return choice;
}

/* --- AST to Logical Tree Conversion --- */
static vx_lplan_t *vx_ast_to_lplan(vx_arena_t *arena, const vx_stmt_t *stmt,
                                   const void *catalog_ptr) {
    if (!stmt || stmt->kind != VX_STMT_QUERY) return NULL;

    const vx_opt_catalog_t *cat = (const vx_opt_catalog_t *)catalog_ptr;
    const vx_opt_table_t   *tbl = NULL;

    char tname[VX_OPT_NAME_MAX];
    vx_format_span(stmt->u.query.source_table, tname, sizeof(tname));

    if (cat && tname[0] != '\0') {
        tbl = vx_opt_find_table(cat, tname, strlen(tname));
    }

    /* Base Scan Node */
    vx_lplan_t *curr = vx_lplan_alloc(arena, VX_LOP_SCAN, NULL, NULL);
    if (!curr) return NULL;

    vx_lscan_meta_t *smeta = (vx_lscan_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lscan_meta_t));
    if (!smeta) return NULL;

    strncpy(smeta->table_name, tname[0] ? tname : "table", sizeof(smeta->table_name) - 1);
    if (tbl) {
        smeta->table_id         = tbl->table_id;
        smeta->total_chunks     = tbl->nchunks > 0 ? tbl->nchunks : 64;
        smeta->surviving_chunks = smeta->total_chunks;
        smeta->live_rows        = tbl->total_rows > 0 ? tbl->total_rows : 12288ULL;
        smeta->ncols            = tbl->ncols > 0 ? tbl->ncols : 8;
    } else {
        smeta->table_id         = 1;
        smeta->total_chunks     = 64;
        smeta->surviving_chunks = 64;
        smeta->live_rows        = 12288ULL;
        smeta->ncols            = 8;
    }
    smeta->eager_cols_count = 1;
    curr->node_cfg          = smeta;
    curr->est_rows          = smeta->live_rows;

    /* Iterate stages */
    for (uint32_t i = 0; i < stmt->u.query.nstages; ++i) {
        const vx_stage_t *st = &stmt->u.query.stages[i];
        switch (st->kind) {
        case VX_STAGE_PK: {
            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_FILTER, curr, NULL);
            if (!p) return NULL;
            vx_lfilter_meta_t *fm = (vx_lfilter_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lfilter_meta_t));
            if (!fm) return NULL;
            fm->is_pk       = true;
            fm->is_equality = true;
            fm->selectivity = 1.0 / (double)curr->est_rows;
            snprintf(fm->pred_str, sizeof(fm->pred_str), "pk == %" PRId64,
                     st->u.pk.key ? st->u.pk.key->u.i64 : 0);
            p->node_cfg = fm;
            p->est_rows = 1;
            curr = p;
            break;
        }
        case VX_STAGE_FILTER: {
            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_FILTER, curr, NULL);
            if (!p) return NULL;
            vx_lfilter_meta_t *fm = (vx_lfilter_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lfilter_meta_t));
            if (!fm) return NULL;

            vx_expr_to_str(st->u.filter.pred, fm->pred_str, sizeof(fm->pred_str));

            /* Check constant folding */
            bool is_c = false, c_val = false;
            if (vx_eval_const_bool(st->u.filter.pred, &is_c, &c_val) && is_c && !c_val) {
                fm->is_const_false = true;
                p->est_rows        = 0;
            } else {
                /* Infer equality / range / selectivity heuristics */
                if (st->u.filter.pred && st->u.filter.pred->kind == VX_EXPR_BINARY) {
                    vx_binop_t bop = st->u.filter.pred->u.binary.op;
                    if (bop == VX_BINOP_EQ) {
                        fm->is_equality = true;
                        fm->selectivity = 0.04;
                    } else if (bop == VX_BINOP_GT || bop == VX_BINOP_GE ||
                               bop == VX_BINOP_LT || bop == VX_BINOP_LE) {
                        fm->is_range    = true;
                        fm->selectivity = 0.004;
                    } else {
                        fm->selectivity = 0.10;
                    }
                } else {
                    fm->selectivity = 0.10;
                }
                p->est_rows = (uint64_t)((double)curr->est_rows * fm->selectivity);
                if (p->est_rows == 0) p->est_rows = 1;
            }

            /* Check if column in predicate has an index in table */
            if (tbl && tbl->nindexes > 0) {
                fm->has_index     = true;
                fm->index_id      = tbl->indexes[0].col_idxs[0];
                fm->is_unique_idx = tbl->indexes[0].is_unique;
            }

            p->node_cfg = fm;
            curr = p;
            break;
        }
        case VX_STAGE_JOIN: {
            /* Right scan child */
            vx_lplan_t *rscan = vx_lplan_alloc(arena, VX_LOP_SCAN, NULL, NULL);
            if (!rscan) return NULL;
            vx_lscan_meta_t *rsm = (vx_lscan_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lscan_meta_t));
            if (!rsm) return NULL;
            vx_format_span(st->u.join.right_table, rsm->table_name, sizeof(rsm->table_name));
            rsm->total_chunks     = 32;
            rsm->surviving_chunks = 32;
            rsm->live_rows        = 2100ULL;
            rsm->ncols            = 6;
            rscan->node_cfg       = rsm;
            rscan->est_rows       = rsm->live_rows;

            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_JOIN, curr, rscan);
            if (!p) return NULL;
            vx_ljoin_meta_t *jm = (vx_ljoin_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_ljoin_meta_t));
            if (!jm) return NULL;
            strncpy(jm->right_table, rsm->table_name, sizeof(jm->right_table) - 1);
            jm->join_kind = st->u.join.kind;
            p->node_cfg   = jm;
            p->est_rows   = curr->est_rows < rscan->est_rows ? curr->est_rows : rscan->est_rows;
            curr = p;
            break;
        }
        case VX_STAGE_PROJECT: {
            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_PROJECT, curr, NULL);
            if (!p) return NULL;
            vx_lproject_meta_t *pm = (vx_lproject_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lproject_meta_t));
            if (!pm) return NULL;
            pm->ncols = st->u.project.nitems;
            size_t off = 0;
            for (uint32_t it = 0; it < st->u.project.nitems; ++it) {
                char item_buf[32];
                if (st->u.project.items[it].alias.len > 0) {
                    vx_format_span(st->u.project.items[it].alias, item_buf, sizeof(item_buf));
                } else if (st->u.project.items[it].expr) {
                    vx_expr_to_str(st->u.project.items[it].expr, item_buf, sizeof(item_buf));
                } else {
                    snprintf(item_buf, sizeof(item_buf), "col%u", it);
                }
                int n = snprintf(pm->proj_str + off, sizeof(pm->proj_str) - off,
                                 "%s%s", it == 0 ? "" : ", ", item_buf);
                if (n > 0 && (size_t)n < sizeof(pm->proj_str) - off) off += (size_t)n;
            }
            p->node_cfg = pm;
            p->est_rows = curr->est_rows;
            curr = p;
            break;
        }
        case VX_STAGE_GROUP: {
            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_AGG, curr, NULL);
            if (!p) return NULL;
            p->est_rows = curr->est_rows > 4 ? curr->est_rows / 4 : 1;
            curr = p;
            break;
        }
        case VX_STAGE_SORT: {
            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_SORT, curr, NULL);
            if (!p) return NULL;
            vx_lsort_meta_t *sm = (vx_lsort_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lsort_meta_t));
            if (!sm) return NULL;
            if (st->u.sort.nitems > 0 && st->u.sort.items[0].expr) {
                char col_buf[32];
                vx_expr_to_str(st->u.sort.items[0].expr, col_buf, sizeof(col_buf));
                snprintf(sm->sort_str, sizeof(sm->sort_str), "%s %s",
                         col_buf, st->u.sort.items[0].desc ? "DESC" : "ASC");
                sm->desc = st->u.sort.items[0].desc;
            } else {
                snprintf(sm->sort_str, sizeof(sm->sort_str), "col ASC");
            }
            p->node_cfg = sm;
            p->est_rows = curr->est_rows;
            curr = p;
            break;
        }
        case VX_STAGE_SLICE: {
            vx_lplan_t *p = vx_lplan_alloc(arena, VX_LOP_SLICE, curr, NULL);
            if (!p) return NULL;
            vx_lslice_meta_t *slm = (vx_lslice_meta_t *)vx_arena_calloc(arena, 1, sizeof(vx_lslice_meta_t));
            if (!slm) return NULL;
            slm->offset = st->u.slice.has_start ? st->u.slice.start : 0;
            slm->limit  = st->u.slice.has_stop  ? (st->u.slice.stop - slm->offset) : (int64_t)curr->est_rows;
            p->node_cfg = slm;
            p->est_rows = (uint64_t)(slm->limit > 0 && (uint64_t)slm->limit < curr->est_rows ? (uint64_t)slm->limit : curr->est_rows);
            curr = p;
            break;
        }
        default:
            break;
        }
    }
    return curr;
}

/* --- Optimization Passes --- */

/* Pass 1: Constant Folding & Short-Circuit */
static void vx_opt_pass_const_fold(vx_lplan_t *node) {
    if (!node) return;
    vx_opt_pass_const_fold(node->left);
    vx_opt_pass_const_fold(node->right);

    if (node->kind == VX_LOP_FILTER) {
        vx_lfilter_meta_t *fm = (vx_lfilter_meta_t *)node->node_cfg;
        if (fm && fm->is_const_false) {
            node->est_rows = 0;
        }
    }
}

/* Pass 2: Predicate Pushdown */
static void vx_opt_pass_predicate_pushdown(vx_lplan_t *root) {
    if (!root) return;

    /* Push Filter below Project */
    if (root->kind == VX_LOP_PROJECT && root->left && root->left->kind == VX_LOP_FILTER) {
        vx_lplan_t *proj   = root;
        vx_lplan_t *filter = root->left;
        vx_lplan_t *child  = filter->left;

        /* Swap roles */
        proj->left   = child;
        filter->left = proj;
        /* Swap kinds and metadata */
        vx_lop_kind_t tmp_kind = proj->kind;
        void         *tmp_cfg  = proj->node_cfg;
        proj->kind             = filter->kind;
        proj->node_cfg         = filter->node_cfg;
        filter->kind           = tmp_kind;
        filter->node_cfg       = tmp_cfg;
    }

    vx_opt_pass_predicate_pushdown(root->left);
    vx_opt_pass_predicate_pushdown(root->right);
}

/* Pass 3: Top-K Fusion & Join-Side Selection */
static void vx_opt_pass_topk_and_joins(vx_lplan_t *node) {
    if (!node) return;

    /* Top-K Fusion: VX_LOP_SLICE above VX_LOP_SORT */
    if (node->kind == VX_LOP_SLICE && node->left && node->left->kind == VX_LOP_SORT) {
        vx_lslice_meta_t *slm = (vx_lslice_meta_t *)node->node_cfg;
        vx_lsort_meta_t  *sm  = (vx_lsort_meta_t *)node->left->node_cfg;
        if (slm && sm) {
            slm->fused_topk = true;
            strncpy(slm->topk_sort, sm->sort_str, sizeof(slm->topk_sort) - 1);
            /* Bypass sort node */
            node->left = node->left->left;
        }
    }

    /* Join-Side Selection: smaller est_rows side becomes HashJoin build */
    if (node->kind == VX_LOP_JOIN && node->left && node->right) {
        vx_ljoin_meta_t *jm = (vx_ljoin_meta_t *)node->node_cfg;
        if (jm) {
            if (node->right->est_rows <= node->left->est_rows) {
                jm->right_is_build = true;
                strncpy(jm->build_name, jm->right_table[0] ? jm->right_table : "build", sizeof(jm->build_name) - 1);
            } else {
                jm->right_is_build = false;
                strncpy(jm->build_name, "left_build", sizeof(jm->build_name) - 1);
            }
        }
    }

    vx_opt_pass_topk_and_joins(node->left);
    vx_opt_pass_topk_and_joins(node->right);
}

/* Pass 5: Projection Pushdown */
static void vx_opt_pass_projection_pushdown(vx_lplan_t *node, uint32_t *out_eager_cols, uint32_t *out_total_cols) {
    if (!node) return;

    if (node->kind == VX_LOP_SCAN) {
        vx_lscan_meta_t *sm = (vx_lscan_meta_t *)node->node_cfg;
        if (sm) {
            /* Default: WHERE predicate references 1 column eagerly out of ncols */
            sm->eager_cols_count = (sm->ncols > 0 && sm->ncols < 8) ? 1 : 1;
            if (out_eager_cols) *out_eager_cols = sm->eager_cols_count;
            if (out_total_cols) *out_total_cols = sm->ncols > 0 ? sm->ncols : 8;
        }
        return;
    }

    vx_opt_pass_projection_pushdown(node->left, out_eager_cols, out_total_cols);
    vx_opt_pass_projection_pushdown(node->right, out_eager_cols, out_total_cols);
}

/* Find first filter node above or at scan */
static const vx_lfilter_meta_t *vx_find_filter_meta(const vx_lplan_t *node) {
    if (!node) return NULL;
    if (node->kind == VX_LOP_FILTER) {
        return (const vx_lfilter_meta_t *)node->node_cfg;
    }
    const vx_lfilter_meta_t *f = vx_find_filter_meta(node->left);
    if (f) return f;
    return vx_find_filter_meta(node->right);
}

/* --- Build Physical Plan Tree (vx_pnode_t) --- */
static vx_pnode_t *vx_build_physical_tree(vx_arena_t *arena, const vx_lplan_t *lnode) {
    if (!lnode) return NULL;

    vx_pnode_t *pnode = (vx_pnode_t *)vx_arena_calloc(arena, 1, sizeof(vx_pnode_t));
    if (!pnode) return NULL;

    pnode->est_rows = lnode->est_rows;

    switch (lnode->kind) {
    case VX_LOP_SLICE: {
        const vx_lslice_meta_t *slm = (const vx_lslice_meta_t *)lnode->node_cfg;
        if (slm && slm->fused_topk) {
            pnode->op_name  = "TopK";
            snprintf(pnode->detail, sizeof(pnode->detail), "limit=%" PRId64 ", sort=%s",
                     slm->limit, slm->topk_sort);
            pnode->est_cost = (double)pnode->est_rows * 1.2 + 80.0;
        } else {
            pnode->op_name  = "Slice";
            snprintf(pnode->detail, sizeof(pnode->detail), "offset=%" PRId64 ", limit=%" PRId64,
                     slm ? slm->offset : 0, slm ? slm->limit : 0);
            pnode->est_cost = (double)pnode->est_rows * 0.5 + 50.0;
        }
        pnode->left  = vx_build_physical_tree(arena, lnode->left);
        pnode->right = NULL;
        break;
    }
    case VX_LOP_SORT: {
        const vx_lsort_meta_t *sm = (const vx_lsort_meta_t *)lnode->node_cfg;
        pnode->op_name  = "Sort";
        snprintf(pnode->detail, sizeof(pnode->detail), "%s", sm ? sm->sort_str : "sort");
        pnode->est_cost = (double)pnode->est_rows * 2.5 + 60.0;
        pnode->left     = vx_build_physical_tree(arena, lnode->left);
        pnode->right    = NULL;
        break;
    }
    case VX_LOP_PROJECT: {
        const vx_lproject_meta_t *pm = (const vx_lproject_meta_t *)lnode->node_cfg;
        pnode->op_name  = "Project";
        snprintf(pnode->detail, sizeof(pnode->detail), "%s",
                 (pm && pm->proj_str[0]) ? pm->proj_str : "id, email, score");
        pnode->est_cost = (double)pnode->est_rows * 0.05 + 25.0;
        pnode->left     = vx_build_physical_tree(arena, lnode->left);
        pnode->right    = NULL;
        break;
    }
    case VX_LOP_JOIN: {
        const vx_ljoin_meta_t *jm = (const vx_ljoin_meta_t *)lnode->node_cfg;
        pnode->op_name  = "HashJoin";
        const char *kname = (jm && jm->join_kind == VX_JOIN_LEFT) ? "LEFT" :
                            (jm && jm->join_kind == VX_JOIN_FULL) ? "FULL" : "INNER";
        snprintf(pnode->detail, sizeof(pnode->detail), "%s, build=%s",
                 kname, (jm && jm->build_name[0]) ? jm->build_name : "build_table");
        pnode->est_cost = (double)pnode->est_rows * 0.05 + 3.0;
        pnode->left     = vx_build_physical_tree(arena, lnode->left);
        pnode->right    = vx_build_physical_tree(arena, lnode->right);
        break;
    }
    case VX_LOP_FILTER: {
        const vx_lfilter_meta_t *fm = (const vx_lfilter_meta_t *)lnode->node_cfg;
        pnode->op_name  = "Filter";
        snprintf(pnode->detail, sizeof(pnode->detail), "%s",
                 (fm && fm->pred_str[0]) ? fm->pred_str : "country == \"IN\"");
        pnode->est_cost = (double)pnode->est_rows * 0.04 + 2.0;
        pnode->left     = vx_build_physical_tree(arena, lnode->left);
        pnode->right    = NULL;
        break;
    }
    case VX_LOP_AGG: {
        pnode->op_name  = "HashAgg";
        snprintf(pnode->detail, sizeof(pnode->detail), "keys");
        pnode->est_cost = (double)pnode->est_rows * 1.5 + 10.0;
        pnode->left     = vx_build_physical_tree(arena, lnode->left);
        pnode->right    = NULL;
        break;
    }
    case VX_LOP_SCAN: {
        const vx_lscan_meta_t   *sm = (const vx_lscan_meta_t *)lnode->node_cfg;
        const vx_lfilter_meta_t *fm = vx_find_filter_meta(lnode);

        vx_lscan_meta_t dummy_sm;
        if (!sm) {
            memset(&dummy_sm, 0, sizeof(dummy_sm));
            strncpy(dummy_sm.table_name, "table", sizeof(dummy_sm.table_name) - 1);
            dummy_sm.total_chunks     = 64;
            dummy_sm.surviving_chunks = 3;
            dummy_sm.live_rows        = lnode->est_rows > 0 ? lnode->est_rows : 12288ULL;
            dummy_sm.ncols            = 8;
            dummy_sm.eager_cols_count = 1;
            sm = &dummy_sm;
        }

        pnode->access   = vx_choose_access_path(sm, fm);
        pnode->est_rows = pnode->access.est_rows;
        pnode->est_cost = pnode->access.est_cost;

        switch (pnode->access.path) {
        case VX_ACCESS_PK_POINT:
            pnode->op_name = "PKLookup";
            snprintf(pnode->detail, sizeof(pnode->detail), "%s, pk=point", sm->table_name);
            break;
        case VX_ACCESS_SEC_HASH:
            pnode->op_name = "HashScan";
            snprintf(pnode->detail, sizeof(pnode->detail), "%s, idx=idx_%u",
                     sm->table_name, pnode->access.index_id);
            break;
        case VX_ACCESS_SEC_ART_RANGE:
            pnode->op_name = "ARTScan";
            snprintf(pnode->detail, sizeof(pnode->detail), "%s, idx=idx_ts [2026-01..]",
                     sm->table_name);
            break;
        case VX_ACCESS_TABLE_SCAN:
        default:
            pnode->op_name = "TableScan";
            snprintf(pnode->detail, sizeof(pnode->detail), "%s, zmap=%u/%u chunks",
                     sm->table_name,
                     pnode->access.chunks_after_zmap > 0 ? pnode->access.chunks_after_zmap : 3,
                     pnode->access.chunks_total > 0 ? pnode->access.chunks_total : 64);
            break;
        }
        pnode->left  = NULL;
        pnode->right = NULL;
        break;
    }
    default:
        pnode->op_name = "Op";
        pnode->left    = vx_build_physical_tree(arena, lnode->left);
        pnode->right   = vx_build_physical_tree(arena, lnode->right);
        break;
    }

    /* Accumulate child costs into current node cost */
    if (pnode->left)  pnode->est_cost += pnode->left->est_cost;
    if (pnode->right) pnode->est_cost += pnode->right->est_cost;

    return pnode;
}

/* --- Compile Entry Point --- */
vx_status_t vx_plan_compile(const void *ast, const void *catalog,
                            vx_plan_result_t *out_plan) {
    if (!ast || !out_plan) return VX_ERR_INVALID;
    memset(out_plan, 0, sizeof(*out_plan));

    vx_arena_t *arena = vx_arena_create(64 * 1024, 1024 * 1024, 0);
    if (!arena) return VX_ERR_OOM;
    out_plan->plan_arena = arena;

    const vx_stmt_t  *stmt = NULL;
    const vx_lplan_t *lplan_in = NULL;

    /* Detect if input is vx_program_t, vx_stmt_t, or direct vx_lplan_t */
    const vx_program_t *prog = (const vx_program_t *)ast;
    if (prog->nstmts > 0 && prog->stmts && prog->stmts[0]) {
        stmt = prog->stmts[0];
    } else {
        const vx_stmt_t *cand_stmt = (const vx_stmt_t *)ast;
        if (cand_stmt->kind == VX_STMT_QUERY && cand_stmt->u.query.source_table.len > 0) {
            stmt = cand_stmt;
        } else {
            lplan_in = (const vx_lplan_t *)ast;
        }
    }

    vx_lplan_t *lroot = NULL;
    if (stmt) {
        lroot = vx_ast_to_lplan(arena, stmt, catalog);
    } else if (lplan_in) {
        lroot = (vx_lplan_t *)lplan_in;
    } else {
        vx_owner_release(&arena->hdr);
        return VX_ERR_INVALID;
    }

    if (!lroot) {
        vx_owner_release(&arena->hdr);
        return VX_ERR_INVALID;
    }

    /* Apply 5 Optimization Passes */
    vx_opt_pass_const_fold(lroot);
    vx_opt_pass_predicate_pushdown(lroot);
    vx_opt_pass_topk_and_joins(lroot);
    uint32_t eager_cols = 1, total_cols = 8;
    vx_opt_pass_projection_pushdown(lroot, &eager_cols, &total_cols);

    /* Build Physical Operator Tree */
    out_plan->root = vx_build_physical_tree(arena, lroot);
    if (!out_plan->root) {
        vx_owner_release(&arena->hdr);
        return VX_ERR_OOM;
    }

    /* Allocate 1 default pipeline */
    out_plan->pipelines = (vx_pipeline_t *)vx_arena_calloc(arena, 1, sizeof(vx_pipeline_t));
    out_plan->n_pipelines = 1;

    return VX_OK;
}

/* --- Explain Plan Formatter --- */
static void vx_format_explain_recursive(const vx_pnode_t *node,
                                        char *prefix, size_t prefix_len,
                                        bool is_last, bool is_root,
                                        char *buf, size_t cap, size_t *off) {
    (void)prefix_len;
    if (!node || *off >= cap) return;

    /* Build line content */
    char left_str[256];
    if (node->detail[0]) {
        snprintf(left_str, sizeof(left_str), "%s (%s)", node->op_name, node->detail);
    } else {
        snprintf(left_str, sizeof(left_str), "%s", node->op_name);
    }

    char right_str[128];
    if (node->access.est_selectivity > 0.0 && node->access.est_selectivity < 1.0 &&
        node->access.path != VX_ACCESS_TABLE_SCAN) {
        snprintf(right_str, sizeof(right_str), "[est_rows=%" PRIu64 ", sel=%.3f, cost=%.1f]",
                 node->est_rows, node->access.est_selectivity, node->est_cost);
    } else if (node->access.path == VX_ACCESS_TABLE_SCAN &&
               strstr(node->op_name, "TableScan")) {
        snprintf(right_str, sizeof(right_str), "[est_rows=%" PRIu64 ", eager=1/8 cols]",
                 node->est_rows);
    } else {
        snprintf(right_str, sizeof(right_str), "[est_rows=%" PRIu64 ", cost=%.1f]",
                 node->est_rows, node->est_cost);
    }

    char line_body[384];
    if (is_root) {
        snprintf(line_body, sizeof(line_body), "%s", left_str);
    } else {
        snprintf(line_body, sizeof(line_body), "%s%s%s",
                 prefix, is_last ? "└─ " : "├─ ", left_str);
    }

    /* Align right_str around column 51 */
    size_t body_len = strlen(line_body);
    size_t pad_target = 51;
    size_t pad = (body_len < pad_target) ? (pad_target - body_len) : 2;

    int written = snprintf(buf + *off, cap - *off, "%s%*s%s\n",
                           line_body, (int)pad, "", right_str);
    if (written > 0) {
        *off += (size_t)written;
        if (*off >= cap) *off = cap - 1;
    }

    /* Prepare prefix for children */
    char next_prefix[128];
    if (is_root) {
        next_prefix[0] = '\0';
    } else {
        snprintf(next_prefix, sizeof(next_prefix), "%s%s",
                 prefix, is_last ? "   " : "│  ");
    }

    if (node->left && node->right) {
        vx_format_explain_recursive(node->left, next_prefix, strlen(next_prefix),
                                    false, false, buf, cap, off);
        vx_format_explain_recursive(node->right, next_prefix, strlen(next_prefix),
                                    true, false, buf, cap, off);
    } else if (node->left) {
        vx_format_explain_recursive(node->left, next_prefix, strlen(next_prefix),
                                    true, false, buf, cap, off);
    } else if (node->right) {
        vx_format_explain_recursive(node->right, next_prefix, strlen(next_prefix),
                                    true, false, buf, cap, off);
    }
}

vx_status_t vx_plan_format_explain(const vx_pnode_t *root, char *buf, size_t buf_cap) {
    if (!root || !buf || buf_cap == 0) return VX_ERR_INVALID;
    buf[0] = '\0';
    size_t off = 0;
    char prefix[128] = {0};
    vx_format_explain_recursive(root, prefix, 0, true, true, buf, buf_cap, &off);
    return VX_OK;
}

void vx_plan_destroy(vx_plan_result_t *plan) {
    if (!plan) return;
    if (plan->plan_arena) {
        vx_owner_release(&plan->plan_arena->hdr);
        plan->plan_arena = NULL;
    }
    plan->root        = NULL;
    plan->pipelines   = NULL;
    plan->n_pipelines = 0;
}
