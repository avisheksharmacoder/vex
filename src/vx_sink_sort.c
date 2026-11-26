#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "vx_sort.h"

#define VX_SORT_MAX_FANIN     64u
#define VX_SORT_MIN_WORK_MEM  (256u * 1024u)
#define VX_SORT_IO_BUF_BYTES  (64u * 1024u)

/* --- Shared Columnar Unpacker (used by both TopKScan and ExtSortScan) --- */
vx_status_t vx_sort_unpack_batch(const vx_sort_cfg_t *cfg, const vx_sort_row_t *const *rows,
                                 uint32_t nrows, vx_arena_t *scratch, vx_batch_t *out) {
    vx_batch_reset(out);
    if (out->cap_cols < cfg->ncols) {
        vx_vec_t *nc = (vx_vec_t *)realloc(out->cols, (size_t)cfg->ncols * sizeof(vx_vec_t));
        if (!nc) return VX_ERR_OOM;
        out->cols = nc;
        out->cap_cols = cfg->ncols;
    }
    out->ncols           = cfg->ncols;
    out->sel.count       = nrows;
    out->sel.is_identity = true;
    if (nrows == 0) return VX_OK;

    const uint32_t nwords = (nrows + 63u) >> 6;

    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        const vx_type_t type = cfg->col_types[c];
        uint64_t       *val  = (uint64_t *)vx_arena_calloc(scratch, nwords, sizeof(uint64_t));
        if (!val) { vx_batch_reset(out); return VX_ERR_OOM; }

        uint32_t valid_n = 0;
        vx_vec_t *v      = &out->cols[c];
        memset(v, 0, sizeof(*v));
        v->type     = type;
        v->kind     = VX_VEC_FLAT;
        v->phys_len = nrows;

        if (type == VX_TYPE_STR) {
            size_t ool_bytes = 0;
            for (uint32_t i = 0; i < nrows; ++i) {
                if (!vx_sr_is_valid(rows[i], c)) continue;
                int32_t len = vx_sr_cells(rows[i], cfg->ncols)[c].str.len;
                if (len > VX_STR_INLINE_MAX) ool_bytes += (size_t)len;
            }
            if (ool_bytes > (size_t)INT32_MAX) { vx_batch_reset(out); return VX_ERR_OOM; }

            vx_strview_t *dst  = (vx_strview_t *)vx_arena_alloc(scratch, (size_t)nrows * sizeof(*dst));
            vx_strpool_t *pool = NULL;
            uint8_t      *sbuf = NULL;
            if (!dst) { vx_batch_reset(out); return VX_ERR_OOM; }

            if (ool_bytes > 0) {
                pool                 = (vx_strpool_t *)vx_arena_calloc(scratch, 1u, sizeof(*pool));
                const uint8_t **ptrs = (const uint8_t **)vx_arena_calloc(scratch, 1u, sizeof(*ptrs));
                int64_t        *lens = (int64_t *)vx_arena_calloc(scratch, 1u, sizeof(*lens));
                sbuf                 = (uint8_t *)vx_arena_alloc(scratch, ool_bytes);
                if (!pool || !ptrs || !lens || !sbuf) { vx_batch_reset(out); return VX_ERR_OOM; }
                ptrs[0] = sbuf; lens[0] = (int64_t)ool_bytes;
                pool->ptrs = ptrs; pool->lens = lens; pool->owners = NULL;
                pool->nbufs = 1u;  pool->cap  = 1u;
            }

            int32_t off = 0;
            for (uint32_t i = 0; i < nrows; ++i) {
                if (!vx_sr_is_valid(rows[i], c)) {
                    memset(&dst[i], 0, sizeof(dst[i]));
                    continue;
                }
                const vx_sort_cell_t cell = vx_sr_cells(rows[i], cfg->ncols)[c];
                const uint8_t       *sp   = vx_sr_strdata(rows[i], cfg->ncols) + cell.str.off;
                if (cell.str.len <= VX_STR_INLINE_MAX) {
                    memset(&dst[i], 0, sizeof(dst[i]));
                    dst[i].inl.len = cell.str.len;
                    if (cell.str.len > 0) memcpy(dst[i].inl.data, sp, (size_t)cell.str.len);
                } else {
                    memcpy(sbuf + off, sp, (size_t)cell.str.len);
                    dst[i].out.len     = cell.str.len;
                    memcpy(dst[i].out.prefix, sp, 4u);
                    dst[i].out.buf_idx = 0;
                    dst[i].out.offset  = off;
                    off               += cell.str.len;
                }
                val[i >> 6] |= (1ULL << (i & 63u));
                valid_n++;
            }
            v->data    = dst;
            v->strpool = pool;
        } else {
            size_t esz = (type == VX_TYPE_BOOL) ? 1u : 8u;
            void  *dst = vx_arena_alloc(scratch, (size_t)nrows * esz);
            if (!dst) { vx_batch_reset(out); return VX_ERR_OOM; }

            for (uint32_t i = 0; i < nrows; ++i) {
                if (!vx_sr_is_valid(rows[i], c)) {
                    if (esz == 1u) ((uint8_t *)dst)[i] = 0u;
                    else           ((uint64_t *)dst)[i] = 0ULL;
                    continue;
                }
                const vx_sort_cell_t cell = vx_sr_cells(rows[i], cfg->ncols)[c];
                if (esz == 1u) ((uint8_t *)dst)[i]  = cell.b;
                else           ((uint64_t *)dst)[i] = cell.u64; /* preserves i64/u64/f64 bits */
                val[i >> 6] |= (1ULL << (i & 63u));
                valid_n++;
            }
            v->data = dst;
        }

        v->has_nulls = (valid_n < nrows) ? 1u : 0u;
        v->validity  = (valid_n < nrows) ? val : NULL;
        v->owner     = vx_owner_retain(&scratch->hdr);
    }
    return VX_OK;
}

/* --- Crash-Safe Anonymous Spill File Helper --- */
static FILE *vx_spill_open_tmp(const char *spill_dir) {
#if !defined(_WIN32)
    const char *dir = (spill_dir && spill_dir[0]) ? spill_dir : "/tmp";
    char        tmpl[512];
    int         n = snprintf(tmpl, sizeof(tmpl), "%s/vex_sort_XXXXXX", dir);
    if (n <= 0 || (size_t)n >= sizeof(tmpl)) return NULL;

    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    (void)unlink(tmpl); /* OS reclaims disk blocks automatically on close or SIGKILL */

    FILE *fp = fdopen(fd, "w+b");
    if (!fp) { close(fd); return NULL; }
    (void)setvbuf(fp, NULL, _IOFBF, VX_SORT_IO_BUF_BYTES);
    return fp;
#else
    (void)spill_dir;
    FILE *fp = tmpfile();
    if (fp) (void)setvbuf(fp, NULL, _IOFBF, VX_SORT_IO_BUF_BYTES);
    return fp;
#endif
}

/* --- Sorted Run Descriptor (In-Memory OR Spilled on Disk) --- */
typedef struct vx_sort_run {
    bool            on_disk;
    /* In-memory run fields */
    uint8_t        *slab;
    size_t          slab_bytes;
    vx_sort_row_t **rows;
    size_t          nrows;
    /* Spilled run fields */
    FILE           *fp;
} vx_sort_run_t;

static void vx_sort_run_free(vx_sort_run_t *r) {
    if (r->fp) fclose(r->fp);
    free(r->slab);
    free(r->rows);
    memset(r, 0, sizeof(*r));
}

/* Converts an in-memory sorted run into a sequential on-disk spill file and frees RAM. */
static vx_status_t vx_sort_run_spill(vx_sort_run_t *run, const char *spill_dir) {
    if (run->on_disk) return VX_OK;
    FILE *fp = vx_spill_open_tmp(spill_dir);
    if (!fp) return VX_ERR_IO;

    for (size_t i = 0; i < run->nrows; ++i) {
        const vx_sort_row_t *row = run->rows[i];
        if (fwrite(row, 1u, row->total_bytes, fp) != row->total_bytes) {
            fclose(fp);
            return VX_ERR_IO;
        }
    }
    if (fflush(fp) != 0 || fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return VX_ERR_IO;
    }

    free(run->slab);
    free(run->rows);
    run->slab       = NULL;
    run->rows       = NULL;
    run->slab_bytes = 0;
    run->fp         = fp;
    run->on_disk    = true;
    return VX_OK;
}

/* --- Run Cursor & k-Way Min-Heap --- */
typedef struct vx_run_cursor {
    vx_sort_run_t *run;
    size_t         idx;          /* in-mem row index */
    vx_sort_row_t *cur;          /* current head row (NULL => exhausted) */
    uint8_t       *io_buf;       /* reusable decode buffer for on-disk runs */
    size_t         io_cap;
    size_t         min_row_size;
} vx_run_cursor_t;

static vx_status_t vx_cursor_advance(vx_run_cursor_t *c) {
    if (!c->run->on_disk) {
        c->cur = (c->idx < c->run->nrows) ? c->run->rows[c->idx++] : NULL;
        return VX_OK;
    }

    vx_sort_row_t hdr;
    size_t got = fread(&hdr, 1u, sizeof(hdr), c->run->fp);
    if (got == 0) {
        if (ferror(c->run->fp)) return VX_ERR_IO;
        c->cur = NULL;
        return VX_OK;
    }
    if (got != sizeof(hdr) || hdr.total_bytes < c->min_row_size) return VX_ERR_CORRUPT;

    if (hdr.total_bytes > c->io_cap) {
        size_t   ncap = c->io_cap ? c->io_cap * 2u : 4096u;
        while (ncap < hdr.total_bytes) ncap *= 2u;
        uint8_t *nb   = (uint8_t *)realloc(c->io_buf, ncap);
        if (!nb) return VX_ERR_OOM;
        c->io_buf = nb;
        c->io_cap = ncap;
    }
    memcpy(c->io_buf, &hdr, sizeof(hdr));
    size_t rem = hdr.total_bytes - sizeof(hdr);
    if (rem > 0 && fread(c->io_buf + sizeof(hdr), 1u, rem, c->run->fp) != rem) {
        return VX_ERR_IO;
    }
    c->cur = (vx_sort_row_t *)c->io_buf;
    return VX_OK;
}

static void vx_cursor_destroy(vx_run_cursor_t *c) {
    free(c->io_buf);
    memset(c, 0, sizeof(*c));
}

typedef struct vx_kmerge {
    const vx_sort_cfg_t *cfg;
    vx_run_cursor_t     *cursors;
    uint32_t            *heap; /* min-heap of cursor indices */
    uint32_t             ncursors;
    uint32_t             heap_len;
} vx_kmerge_t;

static void vx_kmerge_sift_down(vx_kmerge_t *km, uint32_t idx) {
    uint32_t x = km->heap[idx];
    for (;;) {
        uint32_t l = (idx << 1) + 1u;
        if (l >= km->heap_len) break;
        uint32_t r    = l + 1u;
        uint32_t best = l;
        if (r < km->heap_len &&
            vx_sort_row_cmp(km->cfg, km->cursors[km->heap[r]].cur,
                            km->cursors[km->heap[l]].cur) < 0) {
            best = r;
        }
        if (vx_sort_row_cmp(km->cfg, km->cursors[x].cur,
                            km->cursors[km->heap[best]].cur) <= 0) {
            break;
        }
        km->heap[idx] = km->heap[best];
        idx = best;
    }
    km->heap[idx] = x;
}

static void vx_kmerge_destroy(vx_kmerge_t *km) {
    if (km->cursors) {
        for (uint32_t i = 0; i < km->ncursors; ++i) vx_cursor_destroy(&km->cursors[i]);
        free(km->cursors);
    }
    free(km->heap);
    memset(km, 0, sizeof(*km));
}

static vx_status_t vx_kmerge_init(vx_kmerge_t *km, const vx_sort_cfg_t *cfg,
                                  vx_sort_run_t *runs, uint32_t nruns) {
    memset(km, 0, sizeof(*km));
    km->cfg      = cfg;
    km->ncursors = nruns;
    if (nruns == 0) return VX_OK;

    km->cursors = (vx_run_cursor_t *)calloc(nruns, sizeof(*km->cursors));
    km->heap    = (uint32_t *)malloc(nruns * sizeof(*km->heap));
    if (!km->cursors || !km->heap) { vx_kmerge_destroy(km); return VX_ERR_OOM; }

    const size_t min_sz = vx_sr_str_off(cfg->ncols);
    for (uint32_t i = 0; i < nruns; ++i) {
        if (runs[i].on_disk && fseek(runs[i].fp, 0L, SEEK_SET) != 0) {
            vx_kmerge_destroy(km);
            return VX_ERR_IO;
        }
        km->cursors[i].run          = &runs[i];
        km->cursors[i].min_row_size = min_sz;
        vx_status_t st = vx_cursor_advance(&km->cursors[i]);
        if (st != VX_OK) { vx_kmerge_destroy(km); return st; }
        if (km->cursors[i].cur) km->heap[km->heap_len++] = i;
    }

    for (uint32_t i = km->heap_len / 2u; i > 0; --i) {
        vx_kmerge_sift_down(km, i - 1u);
    }
    return VX_OK;
}

/* Advances the winning head cursor after caller has copied/consumed km->cursors[km->heap[0]].cur */
static vx_status_t vx_kmerge_pop_advance(vx_kmerge_t *km) {
    uint32_t    win = km->heap[0];
    vx_status_t st  = vx_cursor_advance(&km->cursors[win]);
    if (st != VX_OK) return st;

    if (km->cursors[win].cur) {
        vx_kmerge_sift_down(km, 0u);
    } else {
        km->heap_len--;
        if (km->heap_len > 0) {
            km->heap[0] = km->heap[km->heap_len];
            vx_kmerge_sift_down(km, 0u);
        }
    }
    return VX_OK;
}

/* --- SortSink Global & Local State --- */
typedef struct vx_sort_gstate {
    const vx_sort_cfg_t *cfg;
    pthread_mutex_t      mu;
    vx_sort_run_t       *runs;
    uint32_t             nruns;
    uint32_t             cap_runs;
    size_t               in_mem_bytes;
    size_t               work_mem_limit;
} vx_sort_gstate_t;

typedef struct vx_sort_lstate {
    const vx_sort_cfg_t *cfg;
    size_t               budget;     /* per-worker memory ceiling before spilling */
    uint8_t             *slab;
    size_t               slab_used;
    size_t               slab_cap;
    uint32_t            *offsets;    /* byte offsets into slab[] */
    size_t               nrows;
    size_t               cap_rows;

    vx_sort_run_t       *spilled;    /* runs already spilled to disk by this worker */
    uint32_t             n_spilled;
    uint32_t             cap_spilled;
} vx_sort_lstate_t;

static vx_status_t vx_sort_g_add_run(vx_sort_gstate_t *g, vx_sort_run_t run) {
    if (!run.on_disk && g->in_mem_bytes + run.slab_bytes > g->work_mem_limit) {
        vx_status_t st = vx_sort_run_spill(&run, g->cfg->spill_dir);
        if (st != VX_OK) { vx_sort_run_free(&run); return st; }
    }
    if (g->nruns == g->cap_runs) {
        uint32_t       ncap = g->cap_runs ? g->cap_runs * 2u : 16u;
        vx_sort_run_t *nr   = (vx_sort_run_t *)realloc(g->runs, (size_t)ncap * sizeof(*nr));
        if (!nr) { vx_sort_run_free(&run); return VX_ERR_OOM; }
        g->runs     = nr;
        g->cap_runs = ncap;
    }
    if (!run.on_disk) g->in_mem_bytes += run.slab_bytes;
    g->runs[g->nruns++] = run;
    return VX_OK;
}

/* Sorts worker's current in-memory slab into a vx_sort_run_t (spilling if `force_spill`). */
static vx_status_t vx_sort_l_seal_run(vx_sort_lstate_t *l, bool force_spill,
                                      vx_sort_run_t *out_run) {
    memset(out_run, 0, sizeof(*out_run));
    if (l->nrows == 0) return VX_OK;

    vx_sort_row_t **ptrs = (vx_sort_row_t **)malloc(l->nrows * sizeof(*ptrs));
    if (!ptrs) return VX_ERR_OOM;
    for (size_t i = 0; i < l->nrows; ++i) {
        ptrs[i] = (vx_sort_row_t *)(l->slab + l->offsets[i]);
    }

    vx_status_t st = vx_sort_rows_stable(l->cfg, ptrs, l->nrows);
    if (st != VX_OK) { free(ptrs); return st; }

    out_run->on_disk    = false;
    out_run->slab       = l->slab;
    out_run->slab_bytes = l->slab_used + l->nrows * sizeof(*ptrs);
    out_run->rows       = ptrs;
    out_run->nrows      = l->nrows;

    if (force_spill) {
        /* Keep l->slab reusable across spills without re-malloc churn */
        out_run->slab = NULL; /* borrow temporarily */
        FILE *fp = vx_spill_open_tmp(l->cfg->spill_dir);
        if (!fp) { free(ptrs); memset(out_run, 0, sizeof(*out_run)); return VX_ERR_IO; }
        for (size_t i = 0; i < l->nrows; ++i) {
            if (fwrite(ptrs[i], 1u, ptrs[i]->total_bytes, fp) != ptrs[i]->total_bytes) {
                fclose(fp); free(ptrs); memset(out_run, 0, sizeof(*out_run)); return VX_ERR_IO;
            }
        }
        if (fflush(fp) != 0 || fseek(fp, 0L, SEEK_SET) != 0) {
            fclose(fp); free(ptrs); memset(out_run, 0, sizeof(*out_run)); return VX_ERR_IO;
        }
        free(ptrs);
        out_run->on_disk    = true;
        out_run->fp         = fp;
        out_run->rows       = NULL;
        out_run->slab_bytes = 0;
        l->slab_used = 0;
        l->nrows     = 0;
    } else {
        /* Transfer slab ownership to run */
        l->slab      = NULL;
        l->slab_used = 0;
        l->slab_cap  = 0;
        l->nrows     = 0;
    }
    return VX_OK;
}

static vx_status_t vx_sort_init_global(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void **gstate) {
    vx_sort_gstate_t *g = (vx_sort_gstate_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;
    if (pthread_mutex_init(&g->mu, NULL) != 0) { free(g); return VX_ERR_OOM; }
    g->cfg            = (const vx_sort_cfg_t *)sk->plan_cfg;
    g->work_mem_limit = (ctx->work_mem_bytes >= VX_SORT_MIN_WORK_MEM)
                            ? ctx->work_mem_bytes : VX_SORT_MIN_WORK_MEM;
    *gstate = g;
    return VX_OK;
}

static vx_status_t vx_sort_init_local(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                      void *gstate, void **lstate) {
    vx_sort_gstate_t *g = (vx_sort_gstate_t *)gstate;
    vx_sort_lstate_t *l = (vx_sort_lstate_t *)calloc(1u, sizeof(*l));
    if (!l) return VX_ERR_OOM;
    l->cfg    = (const vx_sort_cfg_t *)sk->plan_cfg;
    size_t nw = ctx->nworkers ? ctx->nworkers : 1u;
    l->budget = g->work_mem_limit / nw;
    if (l->budget < VX_SORT_MIN_WORK_MEM) l->budget = VX_SORT_MIN_WORK_MEM;
    *lstate = l;
    return VX_OK;
}

static vx_op_res_t vx_sort_consume(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                   void *gstate, void *lstate, vx_batch_t *in) {
    (void)sk; (void)gstate;
    if (in->sel.count == 0) return VX_OP_NEED_MORE_INPUT;

    vx_sort_lstate_t    *l   = (vx_sort_lstate_t *)lstate;
    const vx_sort_cfg_t *cfg = l->cfg;

    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        vx_status_t st = vx_batch_materialize_col(in, cfg->batch_col_ids[c],
                                                  cfg->table_col_ids[c], ctx->scratch);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    const uint32_t n  = in->sel.count;
    const bool     id = in->sel.is_identity;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r   = id ? i : in->sel.idx[i];
        uint32_t sb  = 0;
        size_t   tot = vx_sort_row_measure(cfg, in, r, &sb);

        /* Spill current sorted run if appending this row exceeds per-worker work_mem */
        size_t projected = l->slab_used + tot + (l->nrows + 1u) * (sizeof(uint32_t) + sizeof(void *));
        if (l->nrows > 0 && projected > l->budget) {
            vx_sort_run_t run;
            vx_status_t   st = vx_sort_l_seal_run(l, true, &run);
            if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;

            if (l->n_spilled == l->cap_spilled) {
                uint32_t       ncap = l->cap_spilled ? l->cap_spilled * 2u : 8u;
                vx_sort_run_t *ns   = (vx_sort_run_t *)realloc(l->spilled, (size_t)ncap * sizeof(*ns));
                if (!ns) { vx_sort_run_free(&run); return VX_OP_OOM; }
                l->spilled     = ns;
                l->cap_spilled = ncap;
            }
            l->spilled[l->n_spilled++] = run;
        }

        if (l->slab_used + tot > l->slab_cap) {
            size_t   ncap = l->slab_cap ? l->slab_cap * 2u : (64u * 1024u);
            while (ncap < l->slab_used + tot) ncap *= 2u;
            uint8_t *ns   = (uint8_t *)realloc(l->slab, ncap);
            if (!ns) return VX_OP_OOM;
            l->slab     = ns;
            l->slab_cap = ncap;
        }
        if (l->nrows == l->cap_rows) {
            size_t    ncap = l->cap_rows ? l->cap_rows * 2u : 1024u;
            uint32_t *no   = (uint32_t *)realloc(l->offsets, ncap * sizeof(*no));
            if (!no) return VX_OP_OOM;
            l->offsets  = no;
            l->cap_rows = ncap;
        }

        l->offsets[l->nrows++] = (uint32_t)l->slab_used;
        vx_sort_row_pack(cfg, in, r, tot, sb, (vx_sort_row_t *)(l->slab + l->slab_used));
        l->slab_used += tot;
    }

    return VX_OP_NEED_MORE_INPUT;
}

static vx_status_t vx_sort_combine(const vx_sink_t *sk, vx_exec_ctx_t *ctx,
                                   void *gstate, void *lstate) {
    (void)sk; (void)ctx;
    vx_sort_gstate_t *g = (vx_sort_gstate_t *)gstate;
    vx_sort_lstate_t *l = (vx_sort_lstate_t *)lstate;

    vx_sort_run_t tail_run;
    memset(&tail_run, 0, sizeof(tail_run));
    bool has_tail = (l->nrows > 0);
    if (has_tail) {
        vx_status_t st = vx_sort_l_seal_run(l, false, &tail_run);
        if (st != VX_OK) return st;
    }

    pthread_mutex_lock(&g->mu);
    vx_status_t st = VX_OK;
    for (uint32_t i = 0; i < l->n_spilled; ++i) {
        st = vx_sort_g_add_run(g, l->spilled[i]);
        if (st != VX_OK) {
            for (uint32_t j = i + 1u; j < l->n_spilled; ++j) vx_sort_run_free(&l->spilled[j]);
            break;
        }
    }
    l->n_spilled = 0;

    if (st == VX_OK && has_tail) {
        st = vx_sort_g_add_run(g, tail_run);
    } else if (has_tail) {
        vx_sort_run_free(&tail_run);
    }
    pthread_mutex_unlock(&g->mu);
    return st;
}

/* Merges runs[start .. start+count-1] into a single on-disk run to keep fan-in <= 64. */
static vx_status_t vx_sort_merge_to_disk(const vx_sort_cfg_t *cfg, vx_sort_run_t *runs,
                                         uint32_t count, vx_sort_run_t *out_run) {
    memset(out_run, 0, sizeof(*out_run));
    vx_kmerge_t km;
    vx_status_t st = vx_kmerge_init(&km, cfg, runs, count);
    if (st != VX_OK) return st;

    FILE *fp = vx_spill_open_tmp(cfg->spill_dir);
    if (!fp) { vx_kmerge_destroy(&km); return VX_ERR_IO; }

    size_t total = 0;
    while (km.heap_len > 0) {
        const vx_sort_row_t *row = km.cursors[km.heap[0]].cur;
        if (fwrite(row, 1u, row->total_bytes, fp) != row->total_bytes) {
            fclose(fp); vx_kmerge_destroy(&km); return VX_ERR_IO;
        }
        total++;
        st = vx_kmerge_pop_advance(&km);
        if (st != VX_OK) { fclose(fp); vx_kmerge_destroy(&km); return st; }
    }

    vx_kmerge_destroy(&km);
    if (fflush(fp) != 0 || fseek(fp, 0L, SEEK_SET) != 0) { fclose(fp); return VX_ERR_IO; }

    out_run->on_disk = true;
    out_run->fp      = fp;
    out_run->nrows   = total;
    return VX_OK;
}

static vx_status_t vx_sort_finalize(const vx_sink_t *sk, vx_exec_ctx_t *ctx, void *gstate) {
    (void)sk; (void)ctx;
    vx_sort_gstate_t *g = (vx_sort_gstate_t *)gstate;

    /* Multi-pass fan-in reduction: guarantees <= VX_SORT_MAX_FANIN open FDs during SortScan */
    while (g->nruns > VX_SORT_MAX_FANIN) {
        uint32_t       next_n    = (g->nruns + VX_SORT_MAX_FANIN - 1u) / VX_SORT_MAX_FANIN;
        vx_sort_run_t *next_runs = (vx_sort_run_t *)calloc(next_n, sizeof(*next_runs));
        if (!next_runs) return VX_ERR_OOM;

        uint32_t out_i = 0;
        for (uint32_t pos = 0; pos < g->nruns; pos += VX_SORT_MAX_FANIN) {
            uint32_t batch_k = (g->nruns - pos > VX_SORT_MAX_FANIN)
                                   ? VX_SORT_MAX_FANIN : (g->nruns - pos);
            vx_status_t st = vx_sort_merge_to_disk(g->cfg, &g->runs[pos], batch_k, &next_runs[out_i]);
            if (st != VX_OK) {
                for (uint32_t j = 0; j < out_i; ++j) vx_sort_run_free(&next_runs[j]);
                free(next_runs);
                return st;
            }
            for (uint32_t j = 0; j < batch_k; ++j) vx_sort_run_free(&g->runs[pos + j]);
            out_i++;
        }

        free(g->runs);
        g->runs         = next_runs;
        g->nruns        = out_i;
        g->cap_runs     = next_n;
        g->in_mem_bytes = 0;
    }
    return VX_OK;
}

static void vx_sort_destroy_local(void *lstate) {
    vx_sort_lstate_t *l = (vx_sort_lstate_t *)lstate;
    if (!l) return;
    for (uint32_t i = 0; i < l->n_spilled; ++i) vx_sort_run_free(&l->spilled[i]);
    free(l->spilled);
    free(l->slab);
    free(l->offsets);
    free(l);
}

static void vx_sort_destroy_global(void *gstate) {
    vx_sort_gstate_t *g = (vx_sort_gstate_t *)gstate;
    if (!g) return;
    for (uint32_t i = 0; i < g->nruns; ++i) vx_sort_run_free(&g->runs[i]);
    free(g->runs);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

const vx_sink_vtbl_t VX_SINK_EXT_SORT = {
    .name           = "ExternalSortSink",
    .init_global    = vx_sort_init_global,
    .init_local     = vx_sort_init_local,
    .sink           = vx_sort_consume,
    .combine        = vx_sort_combine,
    .finalize       = vx_sort_finalize,
    .destroy_local  = vx_sort_destroy_local,
    .destroy_global = vx_sort_destroy_global
};

/* --- Streaming Ordered k-Way Merge Source (plan_cfg = finalized vx_sort_gstate_t*) --- */
typedef struct vx_sort_scan_g {
    vx_sort_gstate_t *sort_g;
    vx_kmerge_t       km;
    uint64_t          skipped;
    uint64_t          emitted;
} vx_sort_scan_g_t;

static vx_status_t vx_sorts_init_g(const vx_source_t *s, vx_exec_ctx_t *ctx, void **gs) {
    (void)ctx;
    vx_sort_gstate_t *sg = (vx_sort_gstate_t *)s->plan_cfg;
    vx_sort_scan_g_t *g  = (vx_sort_scan_g_t *)calloc(1u, sizeof(*g));
    if (!g) return VX_ERR_OOM;

    g->sort_g = sg;
    vx_status_t st = vx_kmerge_init(&g->km, sg->cfg, sg->runs, sg->nruns);
    if (st != VX_OK) { free(g); return st; }
    *gs = g;
    return VX_OK;
}

static vx_status_t vx_sorts_init_l(const vx_source_t *s, vx_exec_ctx_t *c, void *g, void **l) {
    (void)s; (void)c; (void)g; *l = NULL; return VX_OK;
}

static vx_op_res_t vx_sorts_next(const vx_source_t *s, vx_exec_ctx_t *ctx,
                                 void *gs, void *ls, vx_batch_t *out) {
    (void)s; (void)ls;
    if (ctx->worker_id != 0) return VX_OP_FINISHED; /* preserve strict global order */

    vx_sort_scan_g_t    *g   = (vx_sort_scan_g_t *)gs;
    const vx_sort_cfg_t *cfg = g->sort_g->cfg;

    /* Skip offset rows if sort was combined with [offset:] */
    while (g->skipped < cfg->offset && g->km.heap_len > 0) {
        vx_status_t st = vx_kmerge_pop_advance(&g->km);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
        g->skipped++;
    }

    if (g->km.heap_len == 0 || (cfg->limit > 0 && g->emitted >= cfg->limit)) {
        return VX_OP_FINISHED;
    }

    uint32_t max_rows = VX_BATCH_CAP;
    if (cfg->limit > 0) {
        uint64_t rem = cfg->limit - g->emitted;
        if (rem < max_rows) max_rows = (uint32_t)rem;
    }

    /* Stage popped rows in ctx->scratch so advancing disk cursors cannot overwrite live rows */
    const vx_sort_row_t *batch_rows[VX_BATCH_CAP];
    uint32_t             cnt = 0;

    while (cnt < max_rows && g->km.heap_len > 0) {
        const vx_sort_row_t *head = g->km.cursors[g->km.heap[0]].cur;
        vx_sort_row_t       *copy = (vx_sort_row_t *)vx_arena_alloc_aligned(
                                        ctx->scratch, head->total_bytes, 8u);
        if (!copy) return VX_OP_OOM;
        memcpy(copy, head, head->total_bytes);
        batch_rows[cnt++] = copy;

        vx_status_t st = vx_kmerge_pop_advance(&g->km);
        if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    }

    vx_status_t st = vx_sort_unpack_batch(cfg, batch_rows, cnt, ctx->scratch, out);
    if (st != VX_OK) return (st == VX_ERR_OOM) ? VX_OP_OOM : VX_OP_ERR;
    g->emitted += cnt;
    return VX_OP_HAVE_MORE_OUTPUT;
}

static void vx_sorts_destroy_l(void *ls) { (void)ls; }
static void vx_sorts_destroy_g(void *gs) {
    vx_sort_scan_g_t *g = (vx_sort_scan_g_t *)gs;
    if (!g) return;
    vx_kmerge_destroy(&g->km);
    free(g);
}

const vx_source_vtbl_t VX_SOURCE_EXT_SORT_SCAN = {
    .name           = "ExternalSortScan",
    .init_global    = vx_sorts_init_g,
    .init_local     = vx_sorts_init_l,
    .next_batch     = vx_sorts_next,
    .destroy_local  = vx_sorts_destroy_l,
    .destroy_global = vx_sorts_destroy_g
};

