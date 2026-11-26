#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vx_bg.h"
#include "vx_failpoint.h"

static void vx_flush_batch_free(vx_flush_batch_t *b) {
    if (!b) return;
    if (b->tables) {
        for (uint32_t t = 0; t < b->n_tables; ++t) {
            vx_flush_table_snap_t *ts = &b->tables[t];
            for (uint32_t i = 0; i < ts->nchunks; ++i) {
                if (ts->chunks && ts->chunks[i])   vx_owner_release(&ts->chunks[i]->hdr);
                if (ts->delmaps && ts->delmaps[i]) vx_owner_release(&ts->delmaps[i]->hdr);
            }
            free(ts->chunks);
            free(ts->delmaps);
        }
        free(b->tables);
    }
    if (b->dirty_dels) {
        for (uint32_t d = 0; d < b->n_dirty_dels; ++d) {
            vx_dirty_del_snap_t *ds = &b->dirty_dels[d];
            for (uint32_t k = 0; k < ds->nchunks; ++k) {
                if (ds->chunk_delmaps && ds->chunk_delmaps[k]) {
                    vx_owner_release(&ds->chunk_delmaps[k]->hdr);
                }
            }
            free(ds->chunk_delmaps);
        }
        free(b->dirty_dels);
    }
    memset(b, 0, sizeof(*b));
}

static void vx_add_ms_timespec(struct timespec *ts, uint32_t ms) {
    ts->tv_sec  += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ============================================================================
 * Hot RAM Accounting & max_memory_bytes Backpressure
 * ============================================================================ */
vx_status_t vx_flusher_reserve_hot(vx_flusher_t *f, size_t add_bytes) {
    if (!f || f->max_memory_bytes == 0 || add_bytes == 0) {
        if (f && add_bytes > 0) {
            atomic_fetch_add_explicit(&f->hot_bytes, add_bytes, memory_order_relaxed);
        }
        return VX_OK;
    }

    const size_t limit      = f->max_memory_bytes;
    const size_t high_water = (limit / 100u) * VX_FLUSH_HIGH_WATER_PCT;

    /* Fast path: atomic CAS reservation while under limit */
    size_t cur = atomic_load_explicit(&f->hot_bytes, memory_order_relaxed);
    while (cur <= limit && add_bytes <= limit - cur) {
        if (atomic_compare_exchange_weak_explicit(&f->hot_bytes, &cur, cur + add_bytes,
                                                  memory_order_acq_rel, memory_order_relaxed)) {
            if (cur + add_bytes >= high_water) {
                pthread_mutex_lock(&f->mu);
                f->flush_requested = true;
                pthread_cond_signal(&f->wake_cv);
                pthread_mutex_unlock(&f->mu);
            }
            return VX_OK;
        }
    }

    /* Backpressure path: wake flusher and wait up to VX_FLUSH_WAIT_TIMEOUT_MS */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    vx_add_ms_timespec(&deadline, VX_FLUSH_WAIT_TIMEOUT_MS);

    pthread_mutex_lock(&f->mu);
    for (;;) {
        cur = atomic_load_explicit(&f->hot_bytes, memory_order_acquire);
        if (cur <= limit && add_bytes <= limit - cur) {
            if (atomic_compare_exchange_strong_explicit(&f->hot_bytes, &cur, cur + add_bytes,
                                                        memory_order_acq_rel, memory_order_relaxed)) {
                pthread_mutex_unlock(&f->mu);
                return VX_OK;
            }
            continue;
        }

        if (f->stop) {
            pthread_mutex_unlock(&f->mu);
            return VX_ERR_MEMLIM;
        }
        f->flush_requested = true;
        pthread_cond_signal(&f->wake_cv);

        int rc = pthread_cond_timedwait(&f->freed_cv, &f->mu, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&f->mu);
            return VX_ERR_MEMLIM; /* surfaces as VexError("memory limit exceeded") */
        }
    }
}

void vx_flusher_release_hot(vx_flusher_t *f, size_t sub_bytes) {
    if (!f || sub_bytes == 0) return;
    size_t cur = atomic_load_explicit(&f->hot_bytes, memory_order_relaxed);
    for (;;) {
        size_t next = (sub_bytes >= cur) ? 0u : (cur - sub_bytes);
        if (atomic_compare_exchange_weak_explicit(&f->hot_bytes, &cur, next,
                                                  memory_order_acq_rel, memory_order_relaxed)) {
            break;
        }
    }
    pthread_mutex_lock(&f->mu);
    pthread_cond_broadcast(&f->freed_cv);
    pthread_mutex_unlock(&f->mu);
}

/* ============================================================================
 * Single Flush Cycle (Hot Chunks + Dirty Cold .del -> .vx/.del -> MANIFEST -> WAL)
 * ============================================================================ */
vx_status_t vx_flusher_run_once(vx_flusher_t *f, bool *out_did_work) {
    if (out_did_work) *out_did_work = false;
    if (!f || !f->ops) return VX_ERR_INVALID;

    vx_flush_batch_t batch;
    memset(&batch, 0, sizeof(batch));

    /* Step 1: Snapshot sealed Hot chunks & dirty cold delmaps (short write_mu inside hook) */
    vx_status_t st = f->ops->snapshot_flush(f->db_ctx, &batch);
    if (st != VX_OK) return st;

    if (batch.n_tables == 0 && batch.n_dirty_dels == 0) {
        vx_flush_batch_free(&batch);
        return VX_OK;
    }

    vx_man_seg_t *new_meta = NULL;
    vx_seg_t    **new_segs = NULL;
    uint32_t      n_new    = 0;

    if (batch.n_tables > 0) {
        new_meta = (vx_man_seg_t *)calloc(batch.n_tables, sizeof(*new_meta));
        new_segs = (vx_seg_t **)calloc(batch.n_tables, sizeof(*new_segs));
        if (!new_meta || !new_segs) { st = VX_ERR_OOM; goto cleanup; }
    }

    /* Step 2: Stream sealed Hot chunks into immutable L0 .vx files (no locks held) */
    for (uint32_t t = 0; t < batch.n_tables; ++t) {
        const vx_flush_table_snap_t *ts = &batch.tables[t];
        if (ts->nchunks == 0) continue;

        pthread_mutex_lock(f->manifest_mu);
        uint64_t seg_id = f->ops->alloc_seg_id(f->db_ctx);
        pthread_mutex_unlock(f->manifest_mu);

        char seg_path[PATH_MAX];
        st = vx_seg_format_path(f->dir_path, seg_id, seg_path, sizeof(seg_path));
        if (st != VX_OK) goto cleanup;

        vx_seg_write_cfg_t wcfg = {
            .table_id   = ts->table_id,
            .segment_id = seg_id,
            .lsn_max    = batch.flush_lsn,
            .ncols      = ts->ncols,
            .pk_col_idx = ts->pk_col_idx,
            .col_types  = ts->col_types,
            .chunks     = (const vx_chunk_t *const *)ts->chunks,
            .delmaps    = (const vx_delmap_t *const *)ts->delmaps,
            .nchunks    = ts->nchunks,
            .spill_dir  = f->spill_dir
        };
        vx_seg_write_info_t winfo;
        st = vx_seg_write(seg_path, &wcfg, &winfo);
        if (st != VX_OK) goto cleanup;

        if (winfo.nrows == 0) {
            /* All rows in sealed hot chunks were already deleted; drop empty file */
            (void)unlink(seg_path);
            continue;
        }

        st = vx_seg_open(seg_path, &new_segs[n_new]);
        if (st != VX_OK) { (void)unlink(seg_path); goto cleanup; }

        new_meta[n_new] = (vx_man_seg_t){
            .segment_id = seg_id,
            .del_gen    = 0ULL,
            .lsn_max    = batch.flush_lsn,
            .file_bytes = winfo.file_bytes,
            .min_pk     = winfo.min_pk,
            .max_pk     = winfo.max_pk,
            .table_id   = ts->table_id,
            .nrows      = (uint32_t)winfo.nrows,
            .live_rows  = (uint32_t)winfo.nrows,
            .level      = 0u,
            .flags      = (uint16_t)winfo.flags
        };
        n_new++;
    }
    VX_FAILPOINT("seg_post_rename_pre_man");

    /* Step 3: Persist dirty cold-segment .del sidecars <= flush_lsn */
    for (uint32_t d = 0; d < batch.n_dirty_dels; ++d) {
        vx_dirty_del_snap_t *ds = &batch.dirty_dels[d];
        st = vx_del_write(f->dir_path, ds->table_id, ds->segment_id, ds->new_del_gen,
                          ds->nrows, ds->nchunks,
                          (const vx_delmap_t *const *)ds->chunk_delmaps, &ds->live_rows);
        if (st != VX_OK) goto cleanup;
    }
    VX_FAILPOINT("del_post_rename_pre_man");

    /* Step 4: Directory fsync barrier BEFORE MANIFEST references new .vx / .del files */
    st = vx_fsync_dir(f->dir_path);
    if (st != VX_OK) goto cleanup;

    /* Step 5: Atomic MANIFEST publish (advances checkpoint_lsn = batch.flush_lsn) */
    pthread_mutex_lock(f->manifest_mu);
    st = f->ops->commit_flush_manifest(f->db_ctx, &batch, new_meta, n_new);
    pthread_mutex_unlock(f->manifest_mu);
    if (st != VX_OK) goto cleanup;

    /* Step 6: Atomic EBR catalog swap (replaces flushed Hot chunks with Cold vx_seg_t) */
    size_t freed_bytes = 0;
    st = f->ops->swap_flush_catalog(f->db_ctx, &batch, new_segs, n_new, &freed_bytes);
    if (st != VX_OK) goto cleanup;

    /* Step 7: Release Hot RAM accounting & wake any backpressured writers */
    vx_flusher_release_hot(f, freed_bytes);

    /* Step 8: Truncate/rotate WAL prefix <= batch.flush_lsn */
    VX_FAILPOINT("man_post_rename_pre_wal_trunc");
    if (f->ops->truncate_wal) {
        (void)f->ops->truncate_wal(f->db_ctx, batch.flush_lsn);
    }

    if (out_did_work) *out_did_work = true;

cleanup:
    if (new_segs) {
        for (uint32_t i = 0; i < n_new; ++i) {
            if (st != VX_OK && new_segs[i]) vx_seg_mark_unlink(new_segs[i]);
            if (new_segs[i]) vx_owner_release(&new_segs[i]->hdr);
        }
        free(new_segs);
    }
    free(new_meta);
    vx_flush_batch_free(&batch);
    return st;
}

static void *vx_flusher_main(void *arg) {
    vx_flusher_t *f = (vx_flusher_t *)arg;
    for (;;) {
        pthread_mutex_lock(&f->mu);
        while (!f->stop && !f->flush_requested) {
            pthread_cond_wait(&f->wake_cv, &f->mu);
        }
        if (f->stop) {
            pthread_mutex_unlock(&f->mu);
            break;
        }
        f->flush_requested = false;
        pthread_mutex_unlock(&f->mu);

        bool        did_work = false;
        vx_status_t st       = vx_flusher_run_once(f, &did_work);
        if (st != VX_OK) {
            pthread_mutex_lock(&f->mu);
            f->last_err = st;
            pthread_cond_broadcast(&f->freed_cv);
            pthread_mutex_unlock(&f->mu);
        }
    }
    return NULL;
}

vx_status_t vx_flusher_start(vx_flusher_t *f, const char *dir_path, const char *spill_dir,
                             size_t max_memory_bytes, pthread_mutex_t *manifest_mu,
                             const vx_bg_ops_t *ops, void *db_ctx) {
    if (!f || !dir_path || !manifest_mu || !ops) return VX_ERR_INVALID;
    memset(f, 0, sizeof(*f));
    f->dir_path         = dir_path;
    f->spill_dir        = spill_dir;
    f->db_ctx           = db_ctx;
    f->ops              = ops;
    f->manifest_mu      = manifest_mu;
    f->max_memory_bytes = max_memory_bytes;
    atomic_init(&f->hot_bytes, 0);

    if (pthread_mutex_init(&f->mu, NULL) != 0) return VX_ERR_OOM;
    if (pthread_cond_init(&f->wake_cv, NULL) != 0) {
        pthread_mutex_destroy(&f->mu);
        return VX_ERR_OOM;
    }
    if (pthread_cond_init(&f->freed_cv, NULL) != 0) {
        pthread_cond_destroy(&f->wake_cv);
        pthread_mutex_destroy(&f->mu);
        return VX_ERR_OOM;
    }
    if (pthread_create(&f->thread, NULL, vx_flusher_main, f) != 0) {
        pthread_cond_destroy(&f->freed_cv);
        pthread_cond_destroy(&f->wake_cv);
        pthread_mutex_destroy(&f->mu);
        return VX_ERR_OOM;
    }
    f->started = true;
    return VX_OK;
}

void vx_flusher_stop(vx_flusher_t *f) {
    if (!f || !f->started) return;
    pthread_mutex_lock(&f->mu);
    f->stop = true;
    pthread_cond_broadcast(&f->wake_cv);
    pthread_cond_broadcast(&f->freed_cv);
    pthread_mutex_unlock(&f->mu);

    pthread_join(f->thread, NULL);
    pthread_cond_destroy(&f->freed_cv);
    pthread_cond_destroy(&f->wake_cv);
    pthread_mutex_destroy(&f->mu);
    f->started = false;
}
