#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vx_bg.h"
#include "vx_failpoint.h"

static void vx_compact_plan_free(vx_compact_plan_t *p) {
    if (!p) return;
    if (p->victims) {
        for (uint32_t v = 0; v < p->nvictims; ++v) {
            vx_compact_victim_t *cv = &p->victims[v];
            if (cv->snap_delmaps && cv->seg) {
                for (uint32_t k = 0; k < cv->seg->nchunks; ++k) {
                    if (cv->snap_delmaps[k]) vx_owner_release(&cv->snap_delmaps[k]->hdr);
                }
            }
            free(cv->snap_delmaps);
            free(cv->row_map);
            if (cv->seg) vx_owner_release(&cv->seg->hdr);
        }
        free(p->victims);
    }
    memset(p, 0, sizeof(*p));
}

/* Helper called by db->swap_compact_catalog() under write_mu:
 * XORs each victim's current chunk_delmaps against its snapshot delmap, maps any newly
 * deleted old_row -> new_row via row_map, and sets the bit in new_delmaps[]. */
vx_status_t vx_compact_catchup_deletes(const vx_compact_victim_t *victim,
                                       const vx_delmap_t *const *curr_delmaps,
                                       uint32_t new_nrows, uint32_t new_nchunks,
                                       vx_delmap_t **new_delmaps, uint32_t *io_new_live) {
    if (!victim || !curr_delmaps || !victim->row_map) return VX_OK;
    const uint32_t v_nchunks = victim->seg->nchunks;

    for (uint32_t k = 0; k < v_nchunks; ++k) {
        const vx_delmap_t *cur = curr_delmaps[k];
        if (!cur) continue; /* no deletes on this victim chunk */

        const vx_delmap_t *snap = victim->snap_delmaps ? victim->snap_delmaps[k] : NULL;
        if (snap && cur->generation == snap->generation) continue; /* unchanged since snapshot */

        for (uint32_t w = 0; w < VX_BITMAP_WORDS; ++w) {
            uint64_t delta = cur->bits[w] & ~(snap ? snap->bits[w] : 0ULL);
            while (delta) {
                uint32_t bit     = (uint32_t)__builtin_ctzll(delta);
                delta           &= delta - 1ULL;
                uint32_t old_row = (k << 12) | (w << 6) | bit;
                if (old_row >= victim->meta.nrows) continue;

                uint32_t new_row = victim->row_map[old_row];
                if (new_row == UINT32_MAX || new_row >= new_nrows) continue;

                uint32_t nk = new_row >> 12;
                uint32_t nr = new_row & (VX_BATCH_CAP - 1u);
                if (nk >= new_nchunks) continue;

                if (!new_delmaps[nk]) {
                    uint32_t c_rows = ((uint64_t)(nk + 1u) * VX_BATCH_CAP <= new_nrows)
                                          ? VX_BATCH_CAP : (new_nrows - nk * VX_BATCH_CAP);
                    new_delmaps[nk] = vx_delmap_create(c_rows);
                    if (!new_delmaps[nk]) return VX_ERR_OOM;
                }
                uint64_t mask = 1ULL << (nr & 63u);
                if ((new_delmaps[nk]->bits[nr >> 6] & mask) == 0ULL) {
                    new_delmaps[nk]->bits[nr >> 6] |= mask;
                    new_delmaps[nk]->live_count--;
                    if (io_new_live && *io_new_live > 0) (*io_new_live)--;
                }
            }
        }
    }
    return VX_OK;
}

/* ============================================================================
 * Single Leveled Compaction Pass
 * ============================================================================ */
vx_status_t vx_compactor_run_once(vx_compactor_t *c, bool *out_did_work) {
    if (out_did_work) *out_did_work = false;
    if (!c || !c->ops) return VX_ERR_INVALID;

    vx_compact_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    bool found = false;

    /* Step 1: Select victim segments (tombstone ratio >= 25% or >= 4 L0 segments) */
    vx_status_t st = c->ops->plan_compaction(c->db_ctx, &plan, &found);
    if (st != VX_OK || !found || plan.nvictims == 0) {
        vx_compact_plan_free(&plan);
        return st;
    }

    /* Count total input chunk views across all victims */
    uint32_t total_chunks = 0;
    uint64_t max_lsn      = 0;
    for (uint32_t v = 0; v < plan.nvictims; ++v) {
        total_chunks += plan.victims[v].seg->nchunks;
        if (plan.victims[v].meta.lsn_max > max_lsn) max_lsn = plan.victims[v].meta.lsn_max;
    }

    const vx_chunk_t  **all_chunks  = (const vx_chunk_t **)calloc(total_chunks ? total_chunks : 1u,
                                                                  sizeof(*all_chunks));
    const vx_delmap_t **all_delmaps = (const vx_delmap_t **)calloc(total_chunks ? total_chunks : 1u,
                                                                   sizeof(*all_delmaps));
    vx_seg_t           *new_seg     = NULL;
    vx_delmap_t       **new_delmaps = NULL;
    uint32_t            new_nchunks = 0;

    if (!all_chunks || !all_delmaps) { st = VX_ERR_OOM; goto cleanup; }

    /* Step 2: Build deterministic old_row -> new_row map matching vx_seg_write() scan order */
    uint32_t flat_ci = 0;
    uint32_t next_nr = 0;
    for (uint32_t v = 0; v < plan.nvictims; ++v) {
        vx_compact_victim_t *cv = &plan.victims[v];
        const uint32_t       nr = cv->meta.nrows;
        cv->row_map = (uint32_t *)malloc((nr ? nr : 1u) * sizeof(uint32_t));
        if (!cv->row_map) { st = VX_ERR_OOM; goto cleanup; }

        for (uint32_t k = 0; k < cv->seg->nchunks; ++k) {
            const vx_chunk_t  *ch = cv->seg->chunk_views[k];
            const vx_delmap_t *dm = cv->snap_delmaps ? cv->snap_delmaps[k] : NULL;
            all_chunks[flat_ci]   = ch;
            all_delmaps[flat_ci]  = dm;
            flat_ci++;

            for (uint32_t r = 0; r < ch->nrows; ++r) {
                uint32_t old_r = (k << 12) | r;
                bool     del   = dm && (((dm->bits[r >> 6] >> (r & 63u)) & 1ULL) != 0);
                cv->row_map[old_r] = del ? UINT32_MAX : next_nr++;
            }
        }
    }

    /* Step 3: Stream merged live rows into a fresh coarse-grained .vx file */
    vx_man_seg_t new_meta;
    memset(&new_meta, 0, sizeof(new_meta));
    bool has_new_seg = (next_nr > 0);

    if (has_new_seg) {
        pthread_mutex_lock(c->manifest_mu);
        uint64_t new_seg_id = c->ops->alloc_seg_id(c->db_ctx);
        pthread_mutex_unlock(c->manifest_mu);

        char new_path[PATH_MAX];
        st = vx_seg_format_path(c->dir_path, new_seg_id, new_path, sizeof(new_path));
        if (st != VX_OK) goto cleanup;

        vx_seg_write_cfg_t wcfg = {
            .table_id   = plan.table_id,
            .segment_id = new_seg_id,
            .lsn_max    = max_lsn,
            .ncols      = plan.ncols,
            .pk_col_idx = plan.pk_col_idx,
            .col_types  = plan.col_types,
            .chunks     = all_chunks,
            .delmaps    = all_delmaps,
            .nchunks    = total_chunks,
            .spill_dir  = c->spill_dir
        };
        vx_seg_write_info_t winfo;
        st = vx_seg_write(new_path, &wcfg, &winfo);
        if (st != VX_OK) goto cleanup;

        st = vx_fsync_dir(c->dir_path);
        if (st != VX_OK) { (void)unlink(new_path); goto cleanup; }

        st = vx_seg_open(new_path, &new_seg);
        if (st != VX_OK) { (void)unlink(new_path); goto cleanup; }

        new_nchunks = new_seg->nchunks;
        if (new_nchunks > 0) {
            new_delmaps = (vx_delmap_t **)calloc(new_nchunks, sizeof(*new_delmaps));
            if (!new_delmaps) { st = VX_ERR_OOM; goto cleanup; }
        }

        new_meta = (vx_man_seg_t){
            .segment_id = new_seg_id,
            .del_gen    = 0ULL,
            .lsn_max    = max_lsn,
            .file_bytes = winfo.file_bytes,
            .min_pk     = winfo.min_pk,
            .max_pk     = winfo.max_pk,
            .table_id   = plan.table_id,
            .nrows      = (uint32_t)winfo.nrows,
            .live_rows  = (uint32_t)winfo.nrows,
            .level      = plan.target_level,
            .flags      = (uint16_t)winfo.flags
        };
    }

    /* Step 4: Under manifest_mu, publish MANIFEST then catch-up concurrent deletes & EBR swap.
     * Holding manifest_mu blocks the flusher from advancing checkpoint_lsn past any concurrent
     * deletes until swap_compact_catalog() has transferred them onto new_delmaps + dirty_lsn. */
    pthread_mutex_lock(c->manifest_mu);
    st = c->ops->commit_compact_manifest(c->db_ctx, &plan, has_new_seg ? &new_meta : NULL);
    if (st == VX_OK) {
        uint64_t caught_lsn = 0;
        st = c->ops->swap_compact_catalog(c->db_ctx, &plan, new_seg,
                                          has_new_seg ? &new_meta : NULL,
                                          new_delmaps, &caught_lsn);
    }
    pthread_mutex_unlock(c->manifest_mu);
    if (st != VX_OK) goto cleanup;

    VX_FAILPOINT("compact_post_man_pre_unlink");

    /* Ownership of new_delmaps transferred to catalog; mark retired victims for deferred unlink */
    new_delmaps = NULL;
    for (uint32_t v = 0; v < plan.nvictims; ++v) {
        vx_seg_mark_unlink(plan.victims[v].seg);
    }
    if (out_did_work) *out_did_work = true;

cleanup:
    if (new_delmaps) {
        for (uint32_t k = 0; k < new_nchunks; ++k) {
            if (new_delmaps[k]) vx_owner_release(&new_delmaps[k]->hdr);
        }
        free(new_delmaps);
    }
    if (new_seg) {
        if (st != VX_OK) vx_seg_mark_unlink(new_seg);
        vx_owner_release(&new_seg->hdr);
    }
    free(all_chunks);
    free(all_delmaps);
    vx_compact_plan_free(&plan);
    return st;
}

static void *vx_compactor_main(void *arg) {
    vx_compactor_t *c = (vx_compactor_t *)arg;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        while (!c->stop && !c->kick_requested) {
            pthread_cond_wait(&c->wake_cv, &c->mu);
        }
        if (c->stop) {
            pthread_mutex_unlock(&c->mu);
            break;
        }
        c->kick_requested = false;
        pthread_mutex_unlock(&c->mu);

        /* Drain all eligible compaction targets back-to-back */
        for (;;) {
            bool        did_work = false;
            vx_status_t st       = vx_compactor_run_once(c, &did_work);
            if (st != VX_OK) {
                pthread_mutex_lock(&c->mu);
                c->last_err = st;
                pthread_mutex_unlock(&c->mu);
                break;
            }
            if (!did_work) break;
        }
    }
    return NULL;
}

vx_status_t vx_compactor_start(vx_compactor_t *c, const char *dir_path, const char *spill_dir,
                               pthread_mutex_t *manifest_mu, const vx_bg_ops_t *ops, void *db_ctx) {
    if (!c || !dir_path || !manifest_mu || !ops) return VX_ERR_INVALID;
    memset(c, 0, sizeof(*c));
    c->dir_path    = dir_path;
    c->spill_dir   = spill_dir;
    c->db_ctx      = db_ctx;
    c->ops         = ops;
    c->manifest_mu = manifest_mu;

    if (pthread_mutex_init(&c->mu, NULL) != 0) return VX_ERR_OOM;
    if (pthread_cond_init(&c->wake_cv, NULL) != 0) {
        pthread_mutex_destroy(&c->mu);
        return VX_ERR_OOM;
    }
    if (pthread_create(&c->thread, NULL, vx_compactor_main, c) != 0) {
        pthread_cond_destroy(&c->wake_cv);
        pthread_mutex_destroy(&c->mu);
        return VX_ERR_OOM;
    }
    c->started = true;
    return VX_OK;
}

void vx_compactor_kick(vx_compactor_t *c) {
    if (!c || !c->started) return;
    pthread_mutex_lock(&c->mu);
    c->kick_requested = true;
    pthread_cond_signal(&c->wake_cv);
    pthread_mutex_unlock(&c->mu);
}

void vx_compactor_stop(vx_compactor_t *c) {
    if (!c || !c->started) return;
    pthread_mutex_lock(&c->mu);
    c->stop = true;
    pthread_cond_broadcast(&c->wake_cv);
    pthread_mutex_unlock(&c->mu);

    pthread_join(c->thread, NULL);
    pthread_cond_destroy(&c->wake_cv);
    pthread_mutex_destroy(&c->mu);
    c->started = false;
}
