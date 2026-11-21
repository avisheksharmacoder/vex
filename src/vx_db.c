#include "vx_internal.h"
#include "vx_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define VX_PENDING_FLUSH_THRESHOLD 4096U

/* ==================== WORKER POOL ==================== */

static void *vx_pool_worker(void *arg) {
    struct { vx_pool_t *pool; uint32_t id; } *w = arg;
    vx_pool_t *p = w->pool;
    uint32_t id = w->id;
    free(w);

    uint64_t seen_gen = 0;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->stop && p->gen == seen_gen) {
            pthread_cond_wait(&p->cv_work, &p->mu);
        }
        if (p->stop) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        seen_gen = p->gen;
        vx_pool_fn fn = p->fn;
        void *ctx = p->ctx;
        pthread_mutex_unlock(&p->mu);

        if (fn) {
            fn(id, ctx);
        }

        pthread_mutex_lock(&p->mu);
        p->done_count++;
        if (p->done_count == p->nworkers) {
            pthread_cond_signal(&p->cv_done);
        }
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}

static bool vx_pool_init(vx_pool_t *p, uint32_t nworkers) {
    memset(p, 0, sizeof(*p));
    if (nworkers == 0) nworkers = 1;
    if (nworkers > VX_MAX_WORKERS) nworkers = VX_MAX_WORKERS;
    p->nworkers = nworkers;
    pthread_mutex_init(&p->run_mu, NULL);
    pthread_mutex_init(&p->dispatch_mu, NULL);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv_work, NULL);
    pthread_cond_init(&p->cv_done, NULL);

    if (nworkers == 1) {
        return true;
    }

    for (uint32_t i = 0; i < nworkers; i++) {
        struct { vx_pool_t *pool; uint32_t id; } *w = malloc(sizeof(*w));
        if (!w) return false;
        w->pool = p;
        w->id = i;
        if (pthread_create(&p->threads[i], NULL, vx_pool_worker, w) != 0) {
            free(w);
            return false;
        }
    }
    return true;
}

static void vx_pool_destroy(vx_pool_t *p) {
    if (p->nworkers > 1) {
        pthread_mutex_lock(&p->mu);
        p->stop = true;
        pthread_cond_broadcast(&p->cv_work);
        pthread_mutex_unlock(&p->mu);
        for (uint32_t i = 0; i < p->nworkers; i++) {
            pthread_join(p->threads[i], NULL);
        }
    }
    pthread_cond_destroy(&p->cv_done);
    pthread_cond_destroy(&p->cv_work);
    pthread_mutex_destroy(&p->mu);
    pthread_mutex_destroy(&p->dispatch_mu);
    pthread_mutex_destroy(&p->run_mu);
}

void vx_pool_run(vx_pool_t *p, vx_pool_fn fn, void *ctx) {
    if (!p || !fn) return;
    if (p->nworkers <= 1) {
        fn(0, ctx);
        return;
    }
    if (pthread_mutex_trylock(&p->dispatch_mu) != 0) {
        /* Pool busy with another query: run directly on caller thread (zero wait, zero race) */
        fn(0, ctx);
        return;
    }
    pthread_mutex_lock(&p->mu);
    p->fn = fn;
    p->ctx = ctx;
    p->done_count = 0;
    p->gen++;
    pthread_cond_broadcast(&p->cv_work);
    while (p->done_count < p->nworkers) {
        pthread_cond_wait(&p->cv_done, &p->mu);
    }
    pthread_mutex_unlock(&p->mu);
    pthread_mutex_unlock(&p->dispatch_mu);
}

/* ==================== CHUNK REF & CATALOG ==================== */

vx_chunk_ref_t *vx_chunk_ref_new(vx_chunk_t *ch) {
    if (!ch) return NULL;
    vx_chunk_ref_t *r = (vx_chunk_ref_t *)calloc(1, sizeof(vx_chunk_ref_t));
    if (!r) return NULL;
    atomic_init(&r->refcnt, 1);
    r->chunk = ch;
    return r;
}

vx_chunk_ref_t *vx_chunk_ref_inc(vx_chunk_ref_t *r) {
    if (r) {
        atomic_fetch_add_explicit(&r->refcnt, 1, memory_order_relaxed);
    }
    return r;
}

void vx_chunk_ref_dec(vx_chunk_ref_t *r) {
    if (!r) return;
    if (atomic_fetch_sub_explicit(&r->refcnt, 1, memory_order_acq_rel) == 1) {
        vx_chunk_destroy(r->chunk);
        free(r);
    }
}

static inline uint32_t pk_shard_idx(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    return (uint32_t)(key >> 57) & (VX_PK_SHARDS - 1U);
}

static vx_rhmap_ref_t *rhmap_ref_new(size_t init_cap) {
    vx_rhmap_ref_t *r = (vx_rhmap_ref_t *)calloc(1, sizeof(vx_rhmap_ref_t));
    if (!r) return NULL;
    if (!vx_rhmap_init(&r->map, init_cap)) {
        free(r);
        return NULL;
    }
    atomic_init(&r->refcnt, 1);
    return r;
}

static vx_rhmap_ref_t *rhmap_ref_inc(vx_rhmap_ref_t *r) {
    if (r) atomic_fetch_add_explicit(&r->refcnt, 1, memory_order_relaxed);
    return r;
}

static void rhmap_ref_dec(vx_rhmap_ref_t *r) {
    if (!r) return;
    if (atomic_fetch_sub_explicit(&r->refcnt, 1, memory_order_acq_rel) == 1) {
        vx_rhmap_destroy(&r->map);
        free(r);
    }
}

static vx_rhmap_ref_t *rhmap_ref_clone(const vx_rhmap_ref_t *src) {
    vx_rhmap_t *cm = vx_rhmap_clone(&src->map);
    if (!cm) return NULL;
    vx_rhmap_ref_t *r = (vx_rhmap_ref_t *)calloc(1, sizeof(vx_rhmap_ref_t));
    if (!r) {
        vx_rhmap_destroy(cm);
        free(cm);
        return NULL;
    }
    r->map = *cm;
    free(cm);
    atomic_init(&r->refcnt, 1);
    return r;
}

vx_pk_index_t *vx_pk_index_create(void) {
    vx_pk_index_t *ix = (vx_pk_index_t *)calloc(1, sizeof(vx_pk_index_t));
    if (!ix) return NULL;
    atomic_init(&ix->refcnt, 1);
    return ix;
}

vx_pk_index_t *vx_pk_index_inc(vx_pk_index_t *ix) {
    if (ix) atomic_fetch_add_explicit(&ix->refcnt, 1, memory_order_relaxed);
    return ix;
}

void vx_pk_index_dec(vx_pk_index_t *ix) {
    if (!ix) return;
    if (atomic_fetch_sub_explicit(&ix->refcnt, 1, memory_order_acq_rel) == 1) {
        for (uint32_t i = 0; i < VX_PK_SHARDS; i++) {
            rhmap_ref_dec(ix->shards[i]);
        }
        free(ix);
    }
}

static bool pk_index_ensure_unique(vx_pk_index_t **ix_ptr) {
    if (!*ix_ptr) {
        *ix_ptr = vx_pk_index_create();
        return *ix_ptr != NULL;
    }
    vx_pk_index_t *old = *ix_ptr;
    if (atomic_load_explicit(&old->refcnt, memory_order_acquire) == 1) {
        return true;
    }
    vx_pk_index_t *dup = vx_pk_index_create();
    if (!dup) return false;
    for (uint32_t i = 0; i < VX_PK_SHARDS; i++) {
        dup->shards[i] = rhmap_ref_inc(old->shards[i]);
    }
    vx_pk_index_dec(old);
    *ix_ptr = dup;
    return true;
}

bool vx_pk_index_get(const vx_pk_index_t *ix, uint64_t key, uint64_t *out_packed) {
    if (!ix) return false;
    const vx_rhmap_ref_t *sh = ix->shards[pk_shard_idx(key)];
    if (!sh) return false;
    return vx_rhmap_get(&sh->map, key, out_packed);
}

bool vx_pk_index_put_cow(vx_pk_index_t **ix_ptr, uint64_t key, uint64_t packed) {
    if (!pk_index_ensure_unique(ix_ptr)) return false;
    vx_pk_index_t *ix = *ix_ptr;
    uint32_t s = pk_shard_idx(key);

    if (!ix->shards[s]) {
        ix->shards[s] = rhmap_ref_new(16);
        if (!ix->shards[s]) return false;
    } else if (atomic_load_explicit(&ix->shards[s]->refcnt, memory_order_acquire) > 1) {
        vx_rhmap_ref_t *dup = rhmap_ref_clone(ix->shards[s]);
        if (!dup) return false;
        rhmap_ref_dec(ix->shards[s]);
        ix->shards[s] = dup;
    }
    return vx_rhmap_put(&ix->shards[s]->map, key, packed);
}

bool vx_pk_index_del_cow(vx_pk_index_t **ix_ptr, uint64_t key, uint64_t *out_packed) {
    if (!ix_ptr || !*ix_ptr) return false;
    uint32_t s = pk_shard_idx(key);
    if (!(*ix_ptr)->shards[s]) return false;

    uint64_t probe = 0;
    if (!vx_rhmap_get(&(*ix_ptr)->shards[s]->map, key, &probe)) {
        return false;
    }
    if (!pk_index_ensure_unique(ix_ptr)) return false;
    vx_pk_index_t *ix = *ix_ptr;
    if (atomic_load_explicit(&ix->shards[s]->refcnt, memory_order_acquire) > 1) {
        vx_rhmap_ref_t *dup = rhmap_ref_clone(ix->shards[s]);
        if (!dup) return false;
        rhmap_ref_dec(ix->shards[s]);
        ix->shards[s] = dup;
    }
    return vx_rhmap_del(&ix->shards[s]->map, key, out_packed);
}

bool vx_val_to_pk_u64(const vx_val_t *v, vx_type_t pk_type, uint64_t *out_key) {
    if (!v || v->is_null || !out_key) return false;
    if (pk_type == VX_TYPE_U64) {
        if (v->type == VX_TYPE_U64) { *out_key = v->v.u64; return true; }
        if (v->type == VX_TYPE_I64 && v->v.i64 >= 0) { *out_key = (uint64_t)v->v.i64; return true; }
        return false;
    }
    if (pk_type == VX_TYPE_I64) {
        if (v->type == VX_TYPE_I64) { *out_key = (uint64_t)v->v.i64; return true; }
        if (v->type == VX_TYPE_U64) { *out_key = v->v.u64; return true; }
        return false;
    }
    return false;
}

bool vx_table_tombstone_row_cow(vx_table_t *t, uint32_t chunk_idx, uint32_t row_idx) {
    if (!t || chunk_idx >= t->nchunks) return false;
    const vx_chunk_t *ch = t->chunks[chunk_idx]->chunk;
    if (row_idx >= ch->nrows) return false;

    if (!t->delmaps[chunk_idx]) {
        t->delmaps[chunk_idx] = vx_delmap_create(ch->cap);
        if (!t->delmaps[chunk_idx]) return false;
    } else if (atomic_load_explicit(&t->delmaps[chunk_idx]->refcnt, memory_order_acquire) > 1) {
        vx_delmap_t *dup = vx_delmap_clone(t->delmaps[chunk_idx]);
        if (!dup) return false;
        vx_delmap_dec(t->delmaps[chunk_idx]);
        t->delmaps[chunk_idx] = dup;
    }

    if (vx_delmap_set(t->delmaps[chunk_idx], row_idx)) {
        if (t->total_rows > 0) t->total_rows--;
        return true;
    }
    return false;
}

static bool col_cell_eq_val(const vx_col_t *col, uint32_t r, const vx_val_t *v) {
    if (vx_col_is_null(col, r) || v->is_null) return false;
    switch (col->type) {
        case VX_TYPE_I64:  return ((const int64_t *)col->data)[r] == v->v.i64;
        case VX_TYPE_U64:  return ((const uint64_t *)col->data)[r] == v->v.u64;
        case VX_TYPE_F64:  return ((const double *)col->data)[r] == v->v.f64;
        case VX_TYPE_BOOL: return (((const uint8_t *)col->data)[r] != 0) == v->v.b;
        case VX_TYPE_STR: {
            const vx_str_t *s = &((const vx_str_t *)col->data)[r];
            return s->len == v->v.str.len &&
                   (s->len == 0 || memcmp(vx_str_data(s), v->v.str.ptr, s->len) == 0);
        }
        default: return false;
    }
}

bool vx_table_check_unique(const vx_table_t *t, const vx_val_t *row,
                           int64_t skip_ci, int64_t skip_ri) {
    if (!t || !row || t->nindexes == 0) return true;

    for (uint32_t ix = 0; ix < t->nindexes; ix++) {
        const vx_index_def_t *id = &t->indexes[ix];
        if (!id->is_unique) continue;

        bool any_null = false;
        for (uint32_t k = 0; k < id->ncols; k++) {
            if (row[id->col_idxs[k]].is_null) { any_null = true; break; }
        }
        if (any_null) continue; /* NULLs do not conflict under unique constraints */

        for (uint32_t ci = 0; ci < t->nchunks; ci++) {
            const vx_chunk_t *ch = t->chunks[ci]->chunk;
            const vx_delmap_t *dm = t->delmaps ? t->delmaps[ci] : NULL;
            if (dm && vx_delmap_count(dm) >= ch->nrows) continue;

            for (uint32_t r = 0; r < ch->nrows; r++) {
                if ((int64_t)ci == skip_ci && (int64_t)r == skip_ri) continue;
                if (vx_delmap_is_set(dm, r)) continue;

                bool all_eq = true;
                for (uint32_t k = 0; k < id->ncols; k++) {
                    uint32_t c = id->col_idxs[k];
                    if (!col_cell_eq_val(&ch->cols[c], r, &row[c])) {
                        all_eq = false;
                        break;
                    }
                }
                if (all_eq) return false;
            }
        }
    }
    return true;
}

static bool val_type_compatible(const vx_val_t *v, vx_type_t target) {
    if (!v || v->is_null) return true;
    if (v->type == target) return true;
    switch (target) {
        case VX_TYPE_I64: return v->type == VX_TYPE_U64;
        case VX_TYPE_U64: return v->type == VX_TYPE_I64 && v->v.i64 >= 0;
        case VX_TYPE_F64: return v->type == VX_TYPE_I64 || v->type == VX_TYPE_U64;
        default:          return false;
    }
}

uint64_t vx_catalog_dead_rows(const vx_catalog_t *cat) {
    if (!cat) return 0;
    uint64_t dead = 0;
    for (uint32_t ti = 0; ti < cat->ntables; ti++) {
        const vx_table_t *t = cat->tables[ti];
        if (!t->delmaps) continue;
        for (uint32_t ci = 0; ci < t->nchunks; ci++) {
            dead += vx_delmap_count(t->delmaps[ci]);
        }
    }
    return dead;
}

static vx_catalog_t *vx_catalog_compact(const vx_catalog_t *src, uint32_t chunk_rows,
                                        uint64_t *next_chunk_id) {
    vx_catalog_t *dst = (vx_catalog_t *)calloc(1, sizeof(vx_catalog_t));
    if (!dst) return NULL;
    dst->next_table_id = src->next_table_id;
    if (src->ntables == 0) return dst;

    dst->tables = (vx_table_t **)calloc(src->ntables, sizeof(vx_table_t *));
    if (!dst->tables) { free(dst); return NULL; }

    for (uint32_t ti = 0; ti < src->ntables; ti++) {
        const vx_table_t *st = src->tables[ti];
        vx_table_t *dt = (vx_table_t *)calloc(1, sizeof(vx_table_t));
        if (!dt) { vx_catalog_free(dst); return NULL; }
        dst->tables[dst->ntables++] = dt;

        memcpy(dt->name, st->name, sizeof(dt->name));
        dt->table_id = st->table_id;
        dt->ncols = st->ncols;
        dt->pk_col = st->pk_col;
        memcpy(dt->cols, st->cols, sizeof(dt->cols));
        memcpy(dt->indexes, st->indexes, sizeof(dt->indexes));
        dt->nindexes = 0; /* source already verified; re-enable after O(N) copy */

        if (dt->pk_col >= 0 &&
            (dt->cols[dt->pk_col].type == VX_TYPE_U64 || dt->cols[dt->pk_col].type == VX_TYPE_I64)) {
            dt->pk_index = vx_pk_index_create();
            if (!dt->pk_index) { vx_catalog_free(dst); return NULL; }
        }

        vx_val_t row[VX_MAX_COLS];
        for (uint32_t ci = 0; ci < st->nchunks; ci++) {
            const vx_chunk_t *ch = st->chunks[ci]->chunk;
            const vx_delmap_t *dm = st->delmaps ? st->delmaps[ci] : NULL;
            if (dm && vx_delmap_count(dm) >= ch->nrows) continue;

            for (uint32_t r = 0; r < ch->nrows; r++) {
                if (vx_delmap_is_set(dm, r)) continue;
                for (uint32_t c = 0; c < st->ncols; c++) {
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
                if (!vx_table_append_row_cow(dt, row, chunk_rows, next_chunk_id)) {
                    vx_catalog_free(dst);
                    return NULL;
                }
            }
        }
        dt->nindexes = st->nindexes;
    }
    return dst;
}

vx_table_t *vx_table_clone_shallow(const vx_table_t *src) {
    vx_table_t *t = (vx_table_t *)calloc(1, sizeof(vx_table_t));
    if (!t) return NULL;
    memcpy(t, src, sizeof(*t));
    t->chunks = NULL;
    t->delmaps = NULL;
    t->pk_index = vx_pk_index_inc(src->pk_index);
    t->chunk_cap = src->nchunks > 0 ? src->nchunks : 4U;
    t->chunks = (vx_chunk_ref_t **)calloc(t->chunk_cap, sizeof(vx_chunk_ref_t *));
    t->delmaps = (vx_delmap_t **)calloc(t->chunk_cap, sizeof(vx_delmap_t *));
    if (!t->chunks || !t->delmaps) {
        free(t->chunks);
        free(t->delmaps);
        vx_pk_index_dec(t->pk_index);
        free(t);
        return NULL;
    }
    for (uint32_t i = 0; i < src->nchunks; i++) {
        t->chunks[i] = vx_chunk_ref_inc(src->chunks[i]);
        t->delmaps[i] = vx_delmap_inc(src->delmaps[i]);
    }
    return t;
}

void vx_table_free(vx_table_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->nchunks; i++) {
        vx_chunk_ref_dec(t->chunks[i]);
        if (t->delmaps) vx_delmap_dec(t->delmaps[i]);
    }
    free(t->chunks);
    free(t->delmaps);
    vx_pk_index_dec(t->pk_index);
    free(t);
}

vx_catalog_t *vx_catalog_clone(const vx_catalog_t *src) {
    vx_catalog_t *c = (vx_catalog_t *)calloc(1, sizeof(vx_catalog_t));
    if (!c) return NULL;
    if (!src) return c;
    c->ntables = src->ntables;
    c->next_table_id = src->next_table_id;
    if (src->ntables > 0) {
        c->tables = (vx_table_t **)calloc(src->ntables, sizeof(vx_table_t *));
        if (!c->tables) {
            free(c);
            return NULL;
        }
        for (uint32_t i = 0; i < src->ntables; i++) {
            c->tables[i] = vx_table_clone_shallow(src->tables[i]);
            if (!c->tables[i]) {
                c->ntables = i;
                vx_catalog_free(c);
                return NULL;
            }
        }
    }
    return c;
}

void vx_catalog_free(vx_catalog_t *cat) {
    if (!cat) return;
    for (uint32_t i = 0; i < cat->ntables; i++) {
        vx_table_free(cat->tables[i]);
    }
    free(cat->tables);
    free(cat);
}

void vx_catalog_ebr_free(void *ptr, void *user_ctx) {
    (void)user_ctx;
    vx_catalog_free((vx_catalog_t *)ptr);
}

vx_table_t *vx_catalog_find(const vx_catalog_t *cat, const char *name, size_t len) {
    if (!cat || !name) return NULL;
    for (uint32_t i = 0; i < cat->ntables; i++) {
        if (strlen(cat->tables[i]->name) == len &&
            memcmp(cat->tables[i]->name, name, len) == 0) {
            return cat->tables[i];
        }
    }
    return NULL;
}

int32_t vx_table_find_col(const vx_table_t *t, const char *name, size_t len) {
    if (!t || !name) return -1;
    for (uint32_t i = 0; i < t->ncols; i++) {
        if (strlen(t->cols[i].name) == len &&
            memcmp(t->cols[i].name, name, len) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static size_t col_elem_bytes(vx_type_t tp) {
    switch (tp) {
        case VX_TYPE_I64:  return sizeof(int64_t);
        case VX_TYPE_U64:  return sizeof(uint64_t);
        case VX_TYPE_F64:  return sizeof(double);
        case VX_TYPE_BOOL: return sizeof(uint8_t);
        case VX_TYPE_STR:  return sizeof(vx_str_t);
        default:           return 8;
    }
}

static vx_chunk_t *vx_chunk_clone_for_append(const vx_chunk_t *src, uint32_t new_cap,
                                             uint64_t chunk_id) {
    vx_type_t types[VX_MAX_COLS];
    bool has_str = false;
    for (uint32_t c = 0; c < src->ncols; c++) {
        types[c] = src->cols[c].type;
        if (types[c] == VX_TYPE_STR) has_str = true;
    }
    size_t str_cap = 64U;
    if (has_str) {
        str_cap = src->str_arena.cap < 65536U ? 65536U : src->str_arena.cap;
        if (src->str_arena.offset + 1024U > str_cap && str_cap < 16U * 1024U * 1024U) {
            str_cap *= 2U;
        }
    }
    vx_chunk_t *dst = vx_chunk_create(chunk_id, src->ncols, types, new_cap, str_cap);
    if (!dst) return NULL;

    size_t null_bytes = (((size_t)src->nrows + 63U) / 64U) * sizeof(uint64_t);
    for (uint32_t c = 0; c < src->ncols; c++) {
        const vx_col_t *sc = &src->cols[c];
        vx_col_t *dc = &dst->cols[c];
        dc->zone = sc->zone;
        if (null_bytes > 0) {
            memcpy(dc->null_bits, sc->null_bits, null_bytes);
        }
        if (sc->type != VX_TYPE_STR) {
            if (src->nrows > 0) {
                memcpy(dc->data, sc->data, (size_t)src->nrows * col_elem_bytes(sc->type));
            }
        } else {
            const vx_str_t *ss = (const vx_str_t *)sc->data;
            vx_str_t *ds = (vx_str_t *)dc->data;
            for (uint32_t r = 0; r < src->nrows; r++) {
                if (vx_col_is_null(sc, r)) continue;
                if (vx_str_is_inline(&ss[r])) {
                    ds[r] = ss[r];
                } else if (!vx_str_make(&dst->str_arena, ss[r].u.ptr, ss[r].len, &ds[r])) {
                    vx_chunk_destroy(dst);
                    return NULL;
                }
            }
        }
    }
    dst->nrows = src->nrows;
    return dst;
}

bool vx_table_append_row_cow(vx_table_t *t, const vx_val_t *vals,
                             uint32_t chunk_rows, uint64_t *next_chunk_id) {
    if (!t || !vals) return false;
    if (chunk_rows == 0) chunk_rows = 4096U;

    if (t->pk_col >= 0 && !vals[t->pk_col].is_null) {
        if (t->pk_index) {
            uint64_t pk_key = 0;
            if (vx_val_to_pk_u64(&vals[t->pk_col], t->cols[t->pk_col].type, &pk_key)) {
                uint64_t packed = 0;
                if (vx_pk_index_get(t->pk_index, pk_key, &packed)) {
                    uint32_t pci = vx_pk_chunk(packed), pri = vx_pk_row(packed);
                    if (pci < t->nchunks && !vx_delmap_is_set(t->delmaps[pci], pri)) {
                        return false; /* duplicate live PK */
                    }
                }
            }
        } else {
            uint32_t pkc = (uint32_t)t->pk_col;
            for (uint32_t ci = 0; ci < t->nchunks; ci++) {
                const vx_chunk_t *ch = t->chunks[ci]->chunk;
                const vx_delmap_t *dm = t->delmaps ? t->delmaps[ci] : NULL;
                for (uint32_t r = 0; r < ch->nrows; r++) {
                    if (vx_delmap_is_set(dm, r)) continue;
                    if (col_cell_eq_val(&ch->cols[pkc], r, &vals[pkc])) return false;
                }
            }
        }
    }
    if (!vx_table_check_unique(t, vals, -1, -1)) {
        return false;
    }

    uint32_t target_ci = 0;
    uint32_t target_ri = 0;
    bool appended = false;

    if (t->nchunks > 0) {
        vx_chunk_ref_t *tail = t->chunks[t->nchunks - 1];
        if (tail->chunk->nrows < tail->chunk->cap) {
            if (atomic_load_explicit(&tail->refcnt, memory_order_acquire) == 1) {
                target_ri = tail->chunk->nrows;
                if (vx_chunk_push_row(tail->chunk, vals)) {
                    target_ci = t->nchunks - 1;
                    appended = true;
                }
            } else {
                vx_chunk_t *cloned = vx_chunk_clone_for_append(tail->chunk,
                                                               tail->chunk->cap,
                                                               ++(*next_chunk_id));
                target_ri = cloned ? cloned->nrows : 0;
                if (cloned && vx_chunk_push_row(cloned, vals)) {
                    vx_chunk_ref_t *nr = vx_chunk_ref_new(cloned);
                    if (!nr) {
                        vx_chunk_destroy(cloned);
                        return false;
                    }
                    vx_chunk_ref_dec(tail);
                    t->chunks[t->nchunks - 1] = nr;
                    target_ci = t->nchunks - 1;
                    appended = true;
                } else {
                    vx_chunk_destroy(cloned);
                }
            }
        }
    }

    if (!appended) {
        if (t->nchunks == t->chunk_cap) {
            uint32_t ncap = t->chunk_cap ? t->chunk_cap * 2U : 4U;
            vx_chunk_ref_t **arr = (vx_chunk_ref_t **)realloc(t->chunks, ncap * sizeof(*arr));
            if (!arr) return false;
            t->chunks = arr;
            vx_delmap_t **darr = (vx_delmap_t **)realloc(t->delmaps, ncap * sizeof(*darr));
            if (!darr) return false;
            memset(darr + t->chunk_cap, 0, (ncap - t->chunk_cap) * sizeof(*darr));
            t->delmaps = darr;
            t->chunk_cap = ncap;
        }

        vx_type_t types[VX_MAX_COLS];
        bool has_str = false;
        for (uint32_t c = 0; c < t->ncols; c++) {
            types[c] = t->cols[c].type;
            if (types[c] == VX_TYPE_STR) has_str = true;
        }
        vx_chunk_t *ch = vx_chunk_create(++(*next_chunk_id), t->ncols, types, chunk_rows,
                                         has_str ? 131072U : 64U);
        if (!ch || !vx_chunk_push_row(ch, vals)) {
            vx_chunk_destroy(ch);
            return false;
        }
        vx_chunk_ref_t *ref = vx_chunk_ref_new(ch);
        if (!ref) {
            vx_chunk_destroy(ch);
            return false;
        }
        target_ci = t->nchunks;
        target_ri = 0;
        t->chunks[t->nchunks] = ref;
        t->delmaps[t->nchunks] = NULL;
        t->nchunks++;
    }

    t->total_rows++;
    if (t->pk_col >= 0 && t->pk_index) {
        uint64_t pk_key = 0;
        if (vx_val_to_pk_u64(&vals[t->pk_col], t->cols[t->pk_col].type, &pk_key)) {
            vx_pk_index_put_cow(&t->pk_index, pk_key, vx_pk_pack(target_ci, target_ri));
        }
    }
    return true;
}

static vx_val_t coerce_val(vx_val_t v, vx_type_t target) {
    if (v.is_null) {
        v.type = target;
        return v;
    }
    if (v.type == target) return v;
    switch (target) {
        case VX_TYPE_I64:
            if (v.type == VX_TYPE_U64) v.v.i64 = (int64_t)v.v.u64;
            else if (v.type == VX_TYPE_F64) v.v.i64 = (int64_t)v.v.f64;
            v.type = VX_TYPE_I64;
            break;
        case VX_TYPE_U64:
            if (v.type == VX_TYPE_I64) v.v.u64 = (uint64_t)v.v.i64;
            else if (v.type == VX_TYPE_F64) v.v.u64 = (uint64_t)v.v.f64;
            v.type = VX_TYPE_U64;
            break;
        case VX_TYPE_F64:
            if (v.type == VX_TYPE_I64) v.v.f64 = (double)v.v.i64;
            else if (v.type == VX_TYPE_U64) v.v.f64 = (double)v.v.u64;
            v.type = VX_TYPE_F64;
            break;
        default:
            v.type = target;
            break;
    }
    return v;
}

bool vx_table_upsert_row_cow(vx_table_t *t, const vx_val_t *vals,
                             uint32_t chunk_rows, uint64_t *next_chunk_id) {
    if (!t || !vals) return false;
    if (t->pk_col < 0 || vals[t->pk_col].is_null) {
        return vx_table_append_row_cow(t, vals, chunk_rows, next_chunk_id);
    }

    /* O(1) Robin Hood PK Index + Tombstone COW fast path */
    uint64_t pk_key = 0;
    if (t->pk_index && vx_val_to_pk_u64(&vals[t->pk_col], t->cols[t->pk_col].type, &pk_key)) {
        uint64_t packed = 0;
        int64_t old_ci = -1, old_ri = -1;
        if (vx_pk_index_get(t->pk_index, pk_key, &packed)) {
            old_ci = (int64_t)vx_pk_chunk(packed);
            old_ri = (int64_t)vx_pk_row(packed);
        }
        if (!vx_table_check_unique(t, vals, old_ci, old_ri)) return false;
        if (old_ci >= 0) {
            vx_table_tombstone_row_cow(t, (uint32_t)old_ci, (uint32_t)old_ri);
        }
        return vx_table_append_row_cow(t, vals, chunk_rows, next_chunk_id);
    }

    /* Fallback for non-integer (e.g. str/f64) PKs */
    uint32_t pkc = (uint32_t)t->pk_col;
    vx_type_t pkt = t->cols[pkc].type;
    for (uint32_t ci = 0; ci < t->nchunks; ci++) {
        const vx_chunk_t *ch = t->chunks[ci]->chunk;
        const vx_col_t *col = &ch->cols[pkc];
        for (uint32_t r = 0; r < ch->nrows; r++) {
            if (vx_delmap_is_set(t->delmaps[ci], r) || vx_col_is_null(col, r)) continue;
            bool match = false;
            if (pkt == VX_TYPE_STR) {
                const vx_str_t *s = &((const vx_str_t *)col->data)[r];
                match = (s->len == vals[pkc].v.str.len) &&
                        (s->len == 0 || memcmp(vx_str_data(s), vals[pkc].v.str.ptr, s->len) == 0);
            } else if (pkt == VX_TYPE_F64) {
                match = ((const double *)col->data)[r] == vals[pkc].v.f64;
            }
            if (match) {
                vx_table_tombstone_row_cow(t, ci, r);
                return vx_table_append_row_cow(t, vals, chunk_rows, next_chunk_id);
            }
        }
    }
    return vx_table_append_row_cow(t, vals, chunk_rows, next_chunk_id);
}

/* ==================== BINARY WAL CODECS (PUT_ROW & QUERY_PARAMS) ==================== */

static inline void w_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU); p[1] = (uint8_t)((v >> 8) & 0xFFU);
}
static inline void w_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xFFU);
}
static inline void w_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xFFU);
}
static inline uint16_t r_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t r_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t r_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i] << (i * 8));
    return v;
}

static size_t val_bin_size(const vx_val_t *v) {
    size_t sz = 2; /* type + is_null */
    if (v->is_null) return sz;
    switch (v->type) {
        case VX_TYPE_I64:
        case VX_TYPE_U64:
        case VX_TYPE_F64:  return sz + 8;
        case VX_TYPE_BOOL: return sz + 1;
        case VX_TYPE_STR:  return sz + 4 + (size_t)v->v.str.len;
        default:           return sz;
    }
}

static uint8_t *encode_val_bin(uint8_t *p, const vx_val_t *v) {
    *p++ = (uint8_t)v->type;
    *p++ = v->is_null ? 1U : 0U;
    if (v->is_null) return p;
    switch (v->type) {
        case VX_TYPE_I64:  w_le64(p, (uint64_t)v->v.i64); return p + 8;
        case VX_TYPE_U64:  w_le64(p, v->v.u64); return p + 8;
        case VX_TYPE_F64: {
            uint64_t bits = 0;
            memcpy(&bits, &v->v.f64, sizeof(bits));
            w_le64(p, bits);
            return p + 8;
        }
        case VX_TYPE_BOOL: *p++ = v->v.b ? 1U : 0U; return p;
        case VX_TYPE_STR:
            w_le32(p, v->v.str.len);
            p += 4;
            if (v->v.str.len > 0) {
                memcpy(p, v->v.str.ptr, v->v.str.len);
                p += v->v.str.len;
            }
            return p;
        default:
            return p;
    }
}

static bool decode_val_bin(const uint8_t *buf, size_t len, size_t *off, vx_val_t *out) {
    if (*off + 2 > len) return false;
    memset(out, 0, sizeof(*out));
    out->type = (vx_type_t)buf[(*off)++];
    out->is_null = buf[(*off)++] != 0;
    if (out->is_null) return true;
    switch (out->type) {
        case VX_TYPE_I64:
            if (*off + 8 > len) return false;
            out->v.i64 = (int64_t)r_le64(buf + *off); *off += 8; return true;
        case VX_TYPE_U64:
            if (*off + 8 > len) return false;
            out->v.u64 = r_le64(buf + *off); *off += 8; return true;
        case VX_TYPE_F64: {
            if (*off + 8 > len) return false;
            uint64_t bits = r_le64(buf + *off); *off += 8;
            memcpy(&out->v.f64, &bits, sizeof(bits));
            return true;
        }
        case VX_TYPE_BOOL:
            if (*off + 1 > len) return false;
            out->v.b = buf[(*off)++] != 0; return true;
        case VX_TYPE_STR: {
            if (*off + 4 > len) return false;
            uint32_t slen = r_le32(buf + *off); *off += 4;
            if (slen > len - *off) return false;
            out->v.str.ptr = slen ? (const char *)(buf + *off) : "";
            out->v.str.len = slen;
            *off += slen;
            return true;
        }
        default:
            return false;
    }
}

typedef struct {
    uint32_t bin_len;
    uint8_t  bin[];
} vx_put_msg_t;

static vx_put_msg_t *build_put_msg(const char *table, const char *const *col_names,
                                   const vx_val_t *vals, uint32_t ncols, bool upsert) {
    size_t tlen = strlen(table);
    if (tlen == 0 || tlen >= VX_COL_NAME_MAX) return NULL;
    size_t total = 1 + 2 + tlen + 2;
    for (uint32_t i = 0; i < ncols; i++) {
        size_t cl = strlen(col_names[i]);
        if (cl == 0 || cl >= VX_COL_NAME_MAX) return NULL;
        total += 2 + cl + val_bin_size(&vals[i]);
    }
    if (total > VX_WAL_MAX_PAYLOAD) return NULL;

    vx_put_msg_t *msg = (vx_put_msg_t *)malloc(sizeof(vx_put_msg_t) + total);
    if (!msg) return NULL;
    msg->bin_len = (uint32_t)total;

    uint8_t *p = msg->bin;
    *p++ = upsert ? 1U : 0U;
    w_le16(p, (uint16_t)tlen); p += 2;
    memcpy(p, table, tlen); p += tlen;
    w_le16(p, (uint16_t)ncols); p += 2;
    for (uint32_t i = 0; i < ncols; i++) {
        size_t cl = strlen(col_names[i]);
        w_le16(p, (uint16_t)cl); p += 2;
        memcpy(p, col_names[i], cl); p += cl;
        p = encode_val_bin(p, &vals[i]);
    }
    return msg;
}

static bool apply_put_bin_to_cat(vx_catalog_t *cat, const uint8_t *buf, size_t len,
                                 uint32_t chunk_rows, uint64_t *next_chunk_id,
                                 uint32_t *out_table_id) {
    size_t off = 0;
    if (len < 5) return false;
    bool upsert = (buf[off++] & 1U) != 0;
    uint16_t tlen = r_le16(buf + off); off += 2;
    if (tlen == 0 || tlen > len - off) return false;
    const char *tname = (const char *)(buf + off); off += tlen;
    if (off + 2 > len) return false;
    uint16_t ncols = r_le16(buf + off); off += 2;
    if (ncols == 0 || ncols > VX_MAX_COLS) return false;

    vx_table_t *t = vx_catalog_find(cat, tname, tlen);
    if (!t) return false;
    if (out_table_id) *out_table_id = t->table_id;

    vx_val_t row[VX_MAX_COLS];
    for (uint32_t c = 0; c < t->ncols; c++) {
        row[c].type = t->cols[c].type;
        row[c].is_null = true;
    }

    for (uint16_t i = 0; i < ncols; i++) {
        if (off + 2 > len) return false;
        uint16_t clen = r_le16(buf + off); off += 2;
        if (clen == 0 || clen > len - off) return false;
        const char *cname = (const char *)(buf + off); off += clen;
        vx_val_t v;
        if (!decode_val_bin(buf, len, &off, &v)) return false;

        int32_t ci = vx_table_find_col(t, cname, clen);
        if (ci < 0) return false;
        row[ci] = coerce_val(v, t->cols[ci].type);
    }

    return upsert
        ? vx_table_upsert_row_cow(t, row, chunk_rows, next_chunk_id)
        : vx_table_append_row_cow(t, row, chunk_rows, next_chunk_id);
}

bool vx_db_wal_log_query_params(vx_db_t *db, const char *query, size_t query_len,
                                const vx_param_t *params, size_t nparams) {
    if (!db->wal) return true;
    size_t total = 4 + query_len + 2;
    for (size_t i = 0; i < nparams; i++) {
        size_t nl = strlen(params[i].name);
        total += 2 + nl + val_bin_size(&params[i].val);
    }
    if (total > VX_WAL_MAX_PAYLOAD) return false;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return false;
    uint8_t *p = buf;
    w_le32(p, (uint32_t)query_len); p += 4;
    memcpy(p, query, query_len); p += query_len;
    w_le16(p, (uint16_t)nparams); p += 2;
    for (size_t i = 0; i < nparams; i++) {
        size_t nl = strlen(params[i].name);
        w_le16(p, (uint16_t)nl); p += 2;
        memcpy(p, params[i].name, nl); p += nl;
        p = encode_val_bin(p, &params[i].val);
    }

    bool ok = vx_db_wal_log(db, VX_WAL_OP_QUERY_PARAMS, 0, buf, (uint32_t)total);
    free(buf);
    return ok;
}

/* ==================== RESULT HELPERS ==================== */

vx_result_t *vx_result_new_err(const char *msg) {
    vx_result_t *r = (vx_result_t *)calloc(1, sizeof(vx_result_t));
    if (!r) return NULL;
    r->ok = false;
    snprintf(r->err_msg, sizeof(r->err_msg), "%s", msg ? msg : "error");
    return r;
}

vx_result_t *vx_result_new_ok(uint64_t rows_affected) {
    vx_result_t *r = (vx_result_t *)calloc(1, sizeof(vx_result_t));
    if (!r) return NULL;
    r->ok = true;
    r->rows_affected = rows_affected;
    return r;
}

void vx_result_free(vx_result_t *res) {
    if (!res) return;
    if (res->cols) {
        for (uint32_t c = 0; c < res->ncols; c++) {
            free(res->cols[c].data);
            free(res->cols[c].null_bits);
        }
        free(res->cols);
    }
    vx_arena_destroy(&res->str_arena);
    free(res->explain_text);
    free(res);
}

int vx_result_col_idx(const vx_result_t *res, const char *name) {
    if (!res || !name) return -1;
    for (uint32_t c = 0; c < res->ncols; c++) {
        if (strcmp(res->cols[c].name, name) == 0) {
            return (int)c;
        }
    }
    return -1;
}

bool vx_result_is_null(const vx_result_t *res, uint32_t col, uint32_t row) {
    if (!res || col >= res->ncols || row >= res->nrows || !res->cols[col].null_bits) {
        return true;
    }
    return (res->cols[col].null_bits[row >> 6] & (1ULL << (row & 63U))) != 0;
}

int64_t vx_result_i64(const vx_result_t *res, uint32_t col, uint32_t row) {
    return ((const int64_t *)res->cols[col].data)[row];
}

uint64_t vx_result_u64(const vx_result_t *res, uint32_t col, uint32_t row) {
    return ((const uint64_t *)res->cols[col].data)[row];
}

double vx_result_f64(const vx_result_t *res, uint32_t col, uint32_t row) {
    return ((const double *)res->cols[col].data)[row];
}

bool vx_result_bool(const vx_result_t *res, uint32_t col, uint32_t row) {
    return ((const uint8_t *)res->cols[col].data)[row] != 0;
}

vx_str_t vx_result_str(const vx_result_t *res, uint32_t col, uint32_t row) {
    return ((const vx_str_t *)res->cols[col].data)[row];
}

/* ==================== DB LIFECYCLE & STAGING FLUSH ==================== */

vx_db_cfg_t vx_db_default_cfg(void) {
    vx_db_cfg_t c = {
        .worker_threads = 4,
        .chunk_rows = 4096,
        .max_readers = 64,
        .flush_interval_us = 2000,
        .fsync_on_commit = false
    };
    return c;
}

static void noop_log_sink(const vx_log_rec_t *recs, size_t count, void *user_ctx) {
    (void)recs; (void)count; (void)user_ctx;
}

static void reader_tls_dtor(void *ptr) {
    vx_ebr_unregister_reader((vx_ebr_reader_t *)ptr);
}

static void wmesh_tx_tls_dtor(void *ptr) {
    vx_wmesh_unregister((vx_wmesh_tx_t *)ptr);
}

vx_ebr_reader_t *vx_db_get_reader(vx_db_t *db) {
    vx_ebr_reader_t *r = (vx_ebr_reader_t *)pthread_getspecific(db->reader_key);
    if (r) return r;
    r = vx_ebr_register_reader(db->ebr);
    if (r) {
        pthread_setspecific(db->reader_key, r);
    }
    return r;
}

bool vx_db_wal_log(vx_db_t *db, uint16_t op_type, uint32_t table_id,
                   const uint8_t *payload, uint32_t len) {
    if (!db->wal) return true;
    vx_wal_rec_t rec = {
        .lsn = ++db->next_lsn,
        .table_id = table_id,
        .op_type = op_type,
        .flags = 0,
        .payload_len = len,
        .payload = payload
    };
    bool ok = vx_wal_append_batch(db->wal, &rec, 1, db->cfg.fsync_on_commit);
    if (ok) {
        atomic_store_explicit(&db->last_wal_lsn, rec.lsn, memory_order_release);
    }
    return ok;
}

static void db_wmesh_commit_cb(const vx_write_op_t *ops, size_t count,
                               uint64_t batch_lsn, void *user_ctx) {
    vx_db_t *db = (vx_db_t *)user_ctx;
    if (count == 0) return;

    pthread_mutex_lock(&db->write_mu);

    const vx_catalog_t *cur = (const vx_catalog_t *)vx_ebr_current_root(db->ebr);
    vx_catalog_t *next = vx_catalog_clone(cur);

    vx_wal_rec_t stack_recs[256];
    vx_wal_rec_t *wrecs = count <= 256 ? stack_recs
                        : (vx_wal_rec_t *)malloc(count * sizeof(vx_wal_rec_t));
    size_t applied = 0;

    for (size_t i = 0; i < count; i++) {
        vx_put_msg_t *msg = NULL;
        memcpy(&msg, ops[i].data, sizeof(msg));
        if (!msg) continue;

        uint32_t tid = 0;
        if (next && apply_put_bin_to_cat(next, msg->bin, msg->bin_len,
                                         db->cfg.chunk_rows, &db->next_chunk_id, &tid)) {
            if (db->wal && wrecs) {
                wrecs[applied] = (vx_wal_rec_t){
                    .lsn = ++db->next_lsn,
                    .table_id = tid,
                    .op_type = VX_WAL_OP_PUT_ROW,
                    .flags = 0,
                    .payload_len = msg->bin_len,
                    .payload = msg->bin
                };
            }
            applied++;
        }
    }

    if (applied > 0 && next) {
        if (db->wal && wrecs) {
            if (vx_wal_append_batch(db->wal, wrecs, applied, db->cfg.fsync_on_commit)) {
                atomic_store_explicit(&db->last_wal_lsn,
                                      wrecs[applied - 1].lsn, memory_order_release);
            }
        }
        void *old = vx_ebr_publish(db->ebr, next);
        atomic_store_explicit(&db->dead_rows, vx_catalog_dead_rows(next), memory_order_release);
        vx_ebr_retire(db->ebr, old, vx_catalog_ebr_free, NULL);
        vx_ebr_reclaim(db->ebr);
        atomic_fetch_add_explicit(&db->mutations_committed, applied, memory_order_relaxed);
    } else {
        vx_catalog_free(next);
    }

    for (size_t i = 0; i < count; i++) {
        vx_put_msg_t *msg = NULL;
        memcpy(&msg, ops[i].data, sizeof(msg));
        free(msg);
    }
    if (wrecs != stack_recs) free(wrecs);

    if (db->wmesh_log_tx) {
        vx_log_emit(db->wmesh_log_tx, VX_LOG_INFO, VX_LOG_EV_BATCH_COMMIT,
                    "wmesh_commit", applied, batch_lsn, 0, 0);
    }

    pthread_mutex_unlock(&db->write_mu);
}

static bool wal_replay_cb(const vx_wal_rec_t *rec, void *user_ctx) {
    vx_db_t *db = (vx_db_t *)user_ctx;
    if (rec->lsn <= db->replay_min_lsn) return true;
    if (!rec->payload || rec->payload_len == 0) return true;

    if (rec->op_type == VX_WAL_OP_PUT_ROW) {
        const vx_catalog_t *cur = (const vx_catalog_t *)vx_ebr_current_root(db->ebr);
        vx_catalog_t *next = vx_catalog_clone(cur);
        if (!next) return false;
        bool ok = apply_put_bin_to_cat(next, rec->payload, rec->payload_len,
                                       db->cfg.chunk_rows, &db->next_chunk_id, NULL);
        if (!ok) {
            vx_catalog_free(next);
            return false;
        }
        void *old = vx_ebr_publish(db->ebr, next);
        atomic_store_explicit(&db->dead_rows, vx_catalog_dead_rows(next), memory_order_release);
        vx_ebr_retire(db->ebr, old, vx_catalog_ebr_free, NULL);
        vx_ebr_reclaim(db->ebr);
        return true;
    }

    if (rec->op_type == VX_WAL_OP_QUERY) {
        size_t arena_sz = (size_t)rec->payload_len * 32U + 65536U;
        vx_arena_t arena;
        if (!vx_arena_init(&arena, arena_sz)) return false;
        vx_parse_result_t pr;
        bool ok = vx_parse((const char *)rec->payload, rec->payload_len, &arena, &pr);
        if (ok && pr.ok) {
            vx_result_t *r = vx_exec_program(db, &pr.program,
                                             (const char *)rec->payload, rec->payload_len,
                                             NULL, 0, true, true);
            ok = r && r->ok;
            vx_result_free(r);
        }
        vx_arena_destroy(&arena);
        return ok;
    }

    if (rec->op_type == VX_WAL_OP_QUERY_PARAMS) {
        size_t off = 0, len = rec->payload_len;
        if (len < 6) return false;
        uint32_t qlen = r_le32(rec->payload + off); off += 4;
        if (qlen > len - off) return false;
        const char *qstr = (const char *)(rec->payload + off); off += qlen;
        if (off + 2 > len) return false;
        uint16_t nparams = r_le16(rec->payload + off); off += 2;
        if (nparams > 64U) return false;

        vx_param_t params[64];
        char pnames[64][VX_COL_NAME_MAX];
        for (uint16_t i = 0; i < nparams; i++) {
            if (off + 2 > len) return false;
            uint16_t nl = r_le16(rec->payload + off); off += 2;
            if (nl == 0 || nl >= VX_COL_NAME_MAX || nl > len - off) return false;
            memcpy(pnames[i], rec->payload + off, nl);
            pnames[i][nl] = '\0';
            off += nl;
            params[i].name = pnames[i];
            if (!decode_val_bin(rec->payload, len, &off, &params[i].val)) return false;
        }

        size_t arena_sz = (size_t)qlen * 32U + 65536U;
        vx_arena_t arena;
        if (!vx_arena_init(&arena, arena_sz)) return false;
        vx_parse_result_t pr;
        bool ok = vx_parse(qstr, qlen, &arena, &pr);
        if (ok && pr.ok) {
            vx_result_t *r = vx_exec_program(db, &pr.program, qstr, qlen,
                                             params, nparams, true, true);
            ok = r && r->ok;
            vx_result_free(r);
        }
        vx_arena_destroy(&arena);
        return ok;
    }

    return true;
}

vx_db_t *vx_db_open(const char *wal_path, const vx_db_cfg_t *cfg) {
    vx_db_t *db = (vx_db_t *)calloc(1, sizeof(vx_db_t));
    if (!db) return NULL;

    db->cfg = cfg ? *cfg : vx_db_default_cfg();
    if (db->cfg.worker_threads == 0) db->cfg.worker_threads = 4;
    if (db->cfg.chunk_rows == 0) db->cfg.chunk_rows = 4096;
    if (db->cfg.max_readers == 0) db->cfg.max_readers = 64;
    if (db->cfg.flush_interval_us == 0) db->cfg.flush_interval_us = 2000;
    if (db->cfg.flush_interval_us < 100) db->cfg.flush_interval_us = 100;

    vx_catalog_t *init_cat = (vx_catalog_t *)calloc(1, sizeof(vx_catalog_t));
    if (!init_cat) { free(db); return NULL; }

    db->ebr = vx_ebr_create(db->cfg.max_readers, init_cat);
    if (!db->ebr) { vx_catalog_free(init_cat); free(db); return NULL; }

    if (!vx_pool_init(&db->pool, db->cfg.worker_threads)) {
        vx_ebr_destroy(db->ebr); vx_catalog_free(init_cat); free(db); return NULL;
    }

    pthread_mutex_init(&db->write_mu, NULL);
    pthread_mutex_init(&db->fallback_wtx_mu, NULL);
    pthread_key_create(&db->reader_key, reader_tls_dtor);
    pthread_key_create(&db->wmesh_tx_key, wmesh_tx_tls_dtor);
    atomic_init(&db->last_wal_lsn, 0);
    atomic_init(&db->checkpoints, 0);
    atomic_init(&db->dead_rows, 0);

    if (wal_path) {
        size_t wlen = strlen(wal_path);
        db->wal_path = (char *)malloc(wlen + 1U);
        if (!db->wal_path) { vx_db_close(db); return NULL; }
        memcpy(db->wal_path, wal_path, wlen + 1U);

        vx_wal_recovery_t rec = {0};
        db->wal = vx_wal_open(wal_path, &rec);
        if (!db->wal) { vx_db_close(db); return NULL; }

        uint64_t snap_lsn = 0;
        char *snap_path = (char *)malloc(wlen + 8U);
        if (!snap_path) { vx_db_close(db); return NULL; }
        snprintf(snap_path, wlen + 8U, "%s.snap", wal_path);

        struct stat sst;
        if (stat(snap_path, &sst) == 0) {
            vx_catalog_t *loaded_cat = NULL;
            if (!vx_snap_load(snap_path, &loaded_cat, &snap_lsn,
                              db->cfg.chunk_rows, &db->next_chunk_id)) {
                free(snap_path);
                vx_db_close(db);
                return NULL;
            }
            void *old = vx_ebr_publish(db->ebr, loaded_cat);
            vx_catalog_free((vx_catalog_t *)old);
        }
        free(snap_path);

        db->replay_min_lsn = snap_lsn;
        if (rec.records_replayed > 0) {
            if (!vx_wal_replay(wal_path, wal_replay_cb, db, NULL)) {
                vx_db_close(db); return NULL;
            }
        }

        uint64_t top_lsn = vx_wal_last_lsn(db->wal);
        if (snap_lsn > top_lsn) top_lsn = snap_lsn;
        db->next_lsn = top_lsn;
        atomic_store_explicit(&db->last_wal_lsn, top_lsn, memory_order_release);
    }

    vx_log_cfg_t lcfg = {
        .max_producers = 4,
        .ring_cap_pow2 = 1024,
        .min_level = VX_LOG_INFO,
        .flush_interval_us = 1000,
        .sink = db->cfg.log_sink ? db->cfg.log_sink : noop_log_sink,
        .user_ctx = db->cfg.log_sink ? db->cfg.log_ctx : NULL
    };
    db->logger = vx_log_create(&lcfg);
    if (db->logger) {
        db->wmesh_log_tx = vx_log_register(db->logger, 1);
        db->write_log_tx = vx_log_register(db->logger, 2);
    }

    vx_wmesh_cfg_t wcfg = {
        .max_producers = 64,
        .ring_cap_pow2 = 4096,
        .batch_max_ops = 1024,
        .flush_interval_us = db->cfg.flush_interval_us,
        .commit_fn = db_wmesh_commit_cb,
        .user_ctx = db
    };
    db->wmesh = vx_wmesh_create(&wcfg);
    if (!db->wmesh) { vx_db_close(db); return NULL; }
    db->fallback_wtx = vx_wmesh_register(db->wmesh);

    return db;
}

void vx_db_close(vx_db_t *db) {
    if (!db) return;

    if (db->wmesh) {
        vx_wmesh_destroy(db->wmesh);
        db->wmesh = NULL;
    }
    if (db->logger) {
        vx_log_destroy(db->logger);
        db->logger = NULL;
    }
    vx_pool_destroy(&db->pool);

    if (db->wal) {
        vx_wal_close(db->wal);
        db->wal = NULL;
    }

    void *live = vx_ebr_current_root(db->ebr);
    vx_ebr_destroy(db->ebr);
    vx_catalog_free((vx_catalog_t *)live);

    pthread_key_delete(db->wmesh_tx_key);
    pthread_key_delete(db->reader_key);
    pthread_mutex_destroy(&db->fallback_wtx_mu);
    pthread_mutex_destroy(&db->write_mu);
    free(db->wal_path); db->wal_path = NULL;
    free(db);
}

void vx_db_flush(vx_db_t *db) {
    if (!db || !db->wmesh) return;
    vx_wmesh_flush(db->wmesh);
}

bool vx_db_checkpoint(vx_db_t *db) {
    if (!db || !db->wal || !db->wal_path) {
        return false;
    }

    if (db->wmesh) {
        vx_wmesh_flush(db->wmesh);
    }

    pthread_mutex_lock(&db->write_mu);

    const vx_catalog_t *cur = (const vx_catalog_t *)vx_ebr_current_root(db->ebr);
    uint64_t ckpt_lsn = db->next_lsn;

    size_t wlen = strlen(db->wal_path);
    char *snap_path = (char *)malloc(wlen + 8U);
    if (!snap_path) {
        pthread_mutex_unlock(&db->write_mu);
        return false;
    }
    snprintf(snap_path, wlen + 8U, "%s.snap", db->wal_path);

    bool ok = vx_snap_write(snap_path, cur, ckpt_lsn) &&
              vx_wal_reset(db->wal, ckpt_lsn);
    free(snap_path);

    if (ok) {
        if (vx_catalog_dead_rows(cur) > 0) {
            vx_catalog_t *compacted = vx_catalog_compact(cur, db->cfg.chunk_rows,
                                                         &db->next_chunk_id);
            if (compacted) {
                void *old = vx_ebr_publish(db->ebr, compacted);
                vx_ebr_retire(db->ebr, old, vx_catalog_ebr_free, NULL);
                vx_ebr_reclaim(db->ebr);
                atomic_store_explicit(&db->dead_rows, 0, memory_order_release);
            }
        }
        atomic_fetch_add_explicit(&db->checkpoints, 1, memory_order_release);
    }

    pthread_mutex_unlock(&db->write_mu);
    return ok;
}

bool vx_db_put_row(vx_db_t *db, const char *table,
                   const char *const *col_names, const vx_val_t *vals,
                   uint32_t ncols, bool upsert, bool sync) {
    if (!db || !db->wmesh || !table || !col_names || !vals || ncols == 0 || ncols > VX_MAX_COLS) {
        return false;
    }

    vx_ebr_reader_t *reader = vx_db_get_reader(db);
    if (!reader) return false;
    const vx_catalog_t *cat = (const vx_catalog_t *)vx_ebr_enter(reader);
    const vx_table_t *t = vx_catalog_find(cat, table, strlen(table));
    bool schema_ok = (t != NULL);
    if (schema_ok) {
        for (uint32_t i = 0; i < ncols; i++) {
            int32_t ci = vx_table_find_col(t, col_names[i], strlen(col_names[i]));
            if (ci < 0 || !val_type_compatible(&vals[i], t->cols[ci].type)) {
                schema_ok = false;
                break;
            }
        }
    }
    vx_ebr_exit(reader);
    if (!schema_ok) return false;

    vx_put_msg_t *msg = build_put_msg(table, col_names, vals, ncols, upsert);
    if (!msg) return false;

    vx_write_op_t op;
    memset(&op, 0, sizeof(op));
    op.op_type = upsert ? VX_OP_UPSERT : VX_OP_INSERT;
    op.len = msg->bin_len;
    memcpy(op.data, &msg, sizeof(msg));

    vx_wmesh_tx_t *tx = (vx_wmesh_tx_t *)pthread_getspecific(db->wmesh_tx_key);
    if (!tx) {
        tx = vx_wmesh_register(db->wmesh);
        if (tx) {
            pthread_setspecific(db->wmesh_tx_key, tx);
        }
    }

    uint64_t tok = 0;
    bool ok = false;
    if (tx) {
        ok = vx_wmesh_submit(tx, &op, &tok);
        if (!ok) { free(msg); return false; }
        if (sync) return vx_wmesh_wait(tx, tok);
        return true;
    }

    pthread_mutex_lock(&db->fallback_wtx_mu);
    ok = vx_wmesh_submit(db->fallback_wtx, &op, &tok);
    pthread_mutex_unlock(&db->fallback_wtx_mu);
    if (!ok) { free(msg); return false; }
    if (sync) vx_wmesh_flush(db->wmesh);
    return true;
}

vx_result_t *vx_db_query(vx_db_t *db, const char *query,
                         const vx_param_t *params, size_t nparams, bool sync) {
    if (!db || !query) {
        return vx_result_new_err("invalid arguments to vx_db_query");
    }

    size_t qlen = strlen(query);
    size_t arena_sz = qlen * 32U + 65536U;
    vx_arena_t arena;
    if (!vx_arena_init(&arena, arena_sz)) {
        return vx_result_new_err("out of memory for parser arena");
    }
    vx_parse_result_t pr;
    if (!vx_parse(query, qlen, &arena, &pr) || !pr.ok) {
        vx_result_t *err = vx_result_new_err(pr.err_msg[0] ? pr.err_msg : "parse error");
        vx_arena_destroy(&arena);
        return err;
    }

    vx_result_t *res = vx_exec_program(db, &pr.program, query, qlen,
                                       params, nparams, sync, false);
    vx_arena_destroy(&arena);
    return res;
}

vx_db_stats_t vx_db_stats(const vx_db_t *db) {
    vx_db_stats_t st = {0};
    if (!db) return st;
    st.queries_executed = atomic_load_explicit(&db->queries_executed, memory_order_relaxed);
    st.mutations_committed = atomic_load_explicit(&db->mutations_committed, memory_order_relaxed);
    st.wal_lsn = atomic_load_explicit(&db->last_wal_lsn, memory_order_acquire);
    st.chunks_pruned = atomic_load_explicit(&db->chunks_pruned, memory_order_relaxed);
    if (db->logger) {
        vx_log_flush(db->logger);
        vx_log_stats_t ls = vx_log_stats(db->logger);
        st.logs_emitted = ls.emitted;
        st.logs_dropped = ls.dropped;
    }
    st.checkpoints = atomic_load_explicit(&db->checkpoints, memory_order_acquire);
    st.dead_rows = atomic_load_explicit(&db->dead_rows, memory_order_acquire);
    return st;
}
