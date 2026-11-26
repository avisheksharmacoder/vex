#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "vx_tx.h"
#include "vx_failpoint.h"

extern uint32_t vx_crc32_update(uint32_t crc, const void *buf, size_t len);

static inline size_t vx_tx_align8(size_t v) {
    return (v + 7u) & ~(size_t)7u;
}

static int vx_tx_fdatasync(int fd) {
#if defined(__linux__)
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

static vx_status_t vx_pread_exact(int fd, void *buf, size_t len, uint64_t off, size_t *out_got) {
    uint8_t *p   = (uint8_t *)buf;
    size_t   got = 0;
    while (got < len) {
        ssize_t n = pread(fd, p + got, len - got, (off_t)(off + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            return VX_ERR_IO;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    *out_got = got;
    return VX_OK;
}

/* ============================================================================
 * 1. Framed WAL Writer & M13 Recovery Reader
 * ============================================================================ */
vx_status_t vx_wal_append_frame(int wal_fd, uint64_t lsn, uint32_t op_type,
                                const uint8_t *payload, uint32_t payload_len, bool sync_disk) {
    if (wal_fd < 0 || (payload_len > 0 && !payload)) return VX_ERR_INVALID;
    if (payload_len > VX_TX_MAX_BATCH_BYTES) return VX_ERR_OOM;

    /* Snapshot exact file end so partial-write errors can truncate cleanly */
    off_t saved_off = lseek(wal_fd, 0, SEEK_END);
    if (saved_off < 0) return VX_ERR_IO;

    vx_wal_rec_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_WAL_MAGIC, 4u);
    hdr.op_type     = op_type;
    hdr.lsn         = lsn;
    hdr.payload_len = payload_len;
    hdr.flags       = sync_disk ? VX_WAL_FLAG_SYNC : 0u;

    uint32_t crc = vx_crc32_update(0u, &hdr, 24u);
    if (payload_len > 0) crc = vx_crc32_update(crc, payload, payload_len);
    hdr.crc32 = crc;

    /* Failpoint wal_tx_mid_frame: inject partial torn write */
    if (vx_failpoint_armed("wal_tx_mid_frame") && op_type == VX_WAL_OP_TX_BATCH) {
        ssize_t pw = write(wal_fd, &hdr, sizeof(hdr));
        (void)pw;
        VX_FAILPOINT("wal_tx_mid_frame");
    }

    /* Single vectored write (loop handles short writes cleanly) */
    struct iovec iov[2];
    int          iovcnt = 1;
    iov[0].iov_base     = &hdr;
    iov[0].iov_len      = sizeof(hdr);
    if (payload_len > 0) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len  = payload_len;
        iovcnt          = 2;
    }

    size_t total   = sizeof(hdr) + (size_t)payload_len;
    size_t written = 0;
    while (written < total) {
        ssize_t n = writev(wal_fd, iov, iovcnt);
        if (n < 0) {
            if (errno == EINTR) continue;
            int trc = ftruncate(wal_fd, saved_off);
            (void)trc;
            (void)lseek(wal_fd, saved_off, SEEK_SET);
            return VX_ERR_IO;
        }
        size_t adv = (size_t)n;
        written   += adv;
        while (adv > 0 && iovcnt > 0) {
            if (adv >= iov[0].iov_len) {
                adv -= iov[0].iov_len;
                iov[0] = iov[1];
                iovcnt--;
            } else {
                iov[0].iov_base = (uint8_t *)iov[0].iov_base + adv;
                iov[0].iov_len -= adv;
                adv = 0;
            }
        }
    }

    if (sync_disk && vx_tx_fdatasync(wal_fd) != 0) {
        int trc = ftruncate(wal_fd, saved_off);
        (void)trc;
        (void)lseek(wal_fd, saved_off, SEEK_SET);
        return VX_ERR_IO;
    }
    return VX_OK;
}

void vx_wal_reader_destroy(vx_wal_reader_t *r) {
    if (!r) return;
    free(r->scratch);
    memset(r, 0, sizeof(*r));
}

/* M13 vx_wal_next_fn: returns VX_ERR_CORRUPT on torn/bad-CRC tail so vx_recover_open() heals. */
vx_status_t vx_wal_read_next_frame(void *wal_ctx, int wal_fd, uint64_t *io_off,
                                   vx_wal_frame_t *out_frame, bool *out_eof) {
    vx_wal_reader_t *r = (vx_wal_reader_t *)wal_ctx;
    if (!r || !io_off || !out_frame || !out_eof) return VX_ERR_INVALID;
    *out_eof = false;

    vx_wal_rec_hdr_t hdr;
    size_t           got = 0;
    vx_status_t      st  = vx_pread_exact(wal_fd, &hdr, sizeof(hdr), *io_off, &got);
    if (st != VX_OK) return st;
    if (got == 0) {
        *out_eof = true;
        return VX_OK;
    }
    if (got < sizeof(hdr)) return VX_ERR_CORRUPT; /* torn header */

    if (memcmp(hdr.magic, VX_WAL_MAGIC, 4u) != 0 ||
        hdr.op_type < VX_WAL_OP_QUERY || hdr.op_type > VX_WAL_OP_TX_BATCH ||
        hdr.payload_len > VX_TX_MAX_BATCH_BYTES) {
        return VX_ERR_CORRUPT;
    }

    if (hdr.payload_len > r->scratch_cap) {
        size_t   ncap = r->scratch_cap ? r->scratch_cap : 4096u;
        while (ncap < hdr.payload_len) ncap *= 2u;
        uint8_t *nb   = (uint8_t *)realloc(r->scratch, ncap);
        if (!nb) return VX_ERR_OOM;
        r->scratch     = nb;
        r->scratch_cap = ncap;
    }

    if (hdr.payload_len > 0) {
        size_t pgot = 0;
        st = vx_pread_exact(wal_fd, r->scratch, hdr.payload_len, *io_off + sizeof(hdr), &pgot);
        if (st != VX_OK) return st;
        if (pgot < hdr.payload_len) return VX_ERR_CORRUPT; /* torn payload */
    }

    uint32_t calc = vx_crc32_update(0u, &hdr, 24u);
    if (hdr.payload_len > 0) calc = vx_crc32_update(calc, r->scratch, hdr.payload_len);
    if (calc != hdr.crc32) return VX_ERR_CORRUPT;

    *io_off               += sizeof(hdr) + (uint64_t)hdr.payload_len;
    out_frame->lsn         = hdr.lsn;
    out_frame->op_type     = hdr.op_type;
    out_frame->payload     = hdr.payload_len ? r->scratch : NULL;
    out_frame->payload_len = hdr.payload_len;
    return VX_OK;
}

/* ============================================================================
 * 2. Internal Batch Buffer & Replay Helper (Shared by Commit Rebase & Recovery)
 * ============================================================================ */
static vx_status_t vx_tx_append_subop_buf(vx_tx_t *tx, uint32_t op_type,
                                          const uint8_t *payload, uint32_t payload_len) {
    size_t aligned_len = vx_tx_align8((size_t)payload_len);
    size_t add_bytes   = sizeof(vx_tx_subop_hdr_t) + aligned_len;
    if (add_bytes > VX_TX_MAX_BATCH_BYTES - tx->batch_len) return VX_ERR_OOM;

    size_t need = tx->batch_len + add_bytes;
    if (need > tx->batch_cap) {
        size_t ncap = tx->batch_cap ? tx->batch_cap : 4096u;
        while (ncap < need) ncap *= 2u;
        if (ncap > VX_TX_MAX_BATCH_BYTES) ncap = VX_TX_MAX_BATCH_BYTES;
        uint8_t *nb = (uint8_t *)realloc(tx->batch_buf, ncap);
        if (!nb) return VX_ERR_OOM;
        tx->batch_buf = nb;
        tx->batch_cap = ncap;
    }

    vx_tx_subop_hdr_t sh = { .op_type = op_type, .data_len = payload_len };
    memcpy(tx->batch_buf + tx->batch_len, &sh, sizeof(sh));
    size_t data_off = tx->batch_len + sizeof(sh);
    if (payload_len > 0) memcpy(tx->batch_buf + data_off, payload, payload_len);
    if (aligned_len > payload_len) {
        memset(tx->batch_buf + data_off + payload_len, 0, aligned_len - payload_len);
    }

    tx->batch_len += add_bytes;
    tx->n_ops++;
    return VX_OK;
}

/* Validates and replays all sub-ops in a framed TX_BATCH payload onto a COW fork of `start_cat`. */
static vx_status_t vx_tx_apply_batch_payload(void *db_ctx, const vx_tx_db_ops_t *ops,
                                             vx_owner_t *start_cat,
                                             const uint8_t *buf, uint32_t buf_len,
                                             vx_owner_t **out_final_cat,
                                             uint64_t *out_write_mask) {
    if (buf_len < sizeof(vx_tx_wal_hdr_t)) return VX_ERR_CORRUPT;

    vx_tx_wal_hdr_t wh;
    memcpy(&wh, buf, sizeof(wh));

    /* Pass 1: validate framing boundaries before mutating anything */
    size_t pos = sizeof(wh);
    for (uint32_t i = 0; i < wh.n_ops; ++i) {
        if (sizeof(vx_tx_subop_hdr_t) > buf_len - pos) return VX_ERR_CORRUPT;
        vx_tx_subop_hdr_t sh;
        memcpy(&sh, buf + pos, sizeof(sh));
        pos += sizeof(sh);
        if (sh.op_type < VX_WAL_OP_QUERY || sh.op_type >= VX_WAL_OP_TX_BATCH) {
            return VX_ERR_CORRUPT;
        }
        size_t aligned = vx_tx_align8((size_t)sh.data_len);
        if (aligned > buf_len - pos) return VX_ERR_CORRUPT;
        pos += aligned;
    }
    if (pos != (size_t)buf_len) return VX_ERR_CORRUPT;

    /* Pass 2: apply sub-ops sequentially onto COW catalog fork */
    vx_owner_t *cur   = vx_owner_retain(start_cat);
    uint64_t    wmask = 0;
    pos               = sizeof(wh);

    for (uint32_t i = 0; i < wh.n_ops; ++i) {
        vx_tx_subop_hdr_t sh;
        memcpy(&sh, buf + pos, sizeof(sh));
        pos += sizeof(sh);

        const uint8_t *data     = sh.data_len ? (buf + pos) : NULL;
        vx_owner_t    *next_cat = NULL;
        uint64_t       tbit     = 0;
        size_t         hbytes   = 0;

        vx_status_t st = ops->apply_cow_op(db_ctx, cur, sh.op_type, data, sh.data_len,
                                           &next_cat, &tbit, &hbytes);
        if (st != VX_OK) {
            vx_owner_release(cur);
            return st;
        }
        vx_owner_release(cur);
        cur    = next_cat;
        wmask |= tbit;
        pos   += vx_tx_align8((size_t)sh.data_len);
    }

    *out_final_cat = cur;
    if (out_write_mask) *out_write_mask = wmask;
    return VX_OK;
}

/* ============================================================================
 * 3. Transaction Lifecycle (Begin, RYOW Read, Stage, Commit, Rollback)
 * ============================================================================ */
vx_status_t vx_tx_begin(void *db_ctx, const vx_tx_db_ops_t *ops, pthread_mutex_t *write_mu,
                        int wal_fd, bool wal_sync, uint64_t tx_id, vx_tx_t **out_tx) {
    if (!db_ctx || !ops || !write_mu || !out_tx) return VX_ERR_INVALID;
    *out_tx = NULL;

    /* Ensure RYOW visibility of any prior non-TX fast-path db.put() calls on this thread */
    if (ops->drain_wmesh) ops->drain_wmesh(db_ctx);

    vx_tx_t *tx = (vx_tx_t *)calloc(1u, sizeof(*tx));
    if (!tx) return VX_ERR_OOM;

    if (pthread_mutex_init(&tx->tx_mu, NULL) != 0) {
        free(tx);
        return VX_ERR_OOM;
    }

    /* Pre-allocate 16-byte vx_tx_wal_hdr_t slot at front of batch_buf */
    tx->batch_cap = 4096u;
    tx->batch_buf = (uint8_t *)calloc(1u, tx->batch_cap);
    if (!tx->batch_buf) {
        pthread_mutex_destroy(&tx->tx_mu);
        free(tx);
        return VX_ERR_OOM;
    }
    tx->batch_len = sizeof(vx_tx_wal_hdr_t);

    tx->db_ctx   = db_ctx;
    tx->ops      = ops;
    tx->write_mu = write_mu;
    tx->wal_fd   = wal_fd;
    tx->wal_sync = wal_sync;
    tx->tx_id    = tx_id;
    tx->state    = VX_TX_ACTIVE;

    tx->base_cat = ops->acquire_snapshot(db_ctx, &tx->base_commit_seq);
    if (!tx->base_cat) {
        free(tx->batch_buf);
        pthread_mutex_destroy(&tx->tx_mu);
        free(tx);
        return VX_ERR_INVALID;
    }
    tx->staged_cat = vx_owner_retain(tx->base_cat);

    *out_tx = tx;
    return VX_OK;
}

vx_owner_t *vx_tx_get_read_catalog(vx_tx_t *tx, uint64_t read_table_bit) {
    if (!tx) return NULL;
    pthread_mutex_lock(&tx->tx_mu);
    if (tx->state != VX_TX_ACTIVE || !tx->staged_cat) {
        pthread_mutex_unlock(&tx->tx_mu);
        return NULL;
    }
    tx->read_tables_mask |= read_table_bit;
    vx_owner_t *snap = vx_owner_retain(tx->staged_cat);
    pthread_mutex_unlock(&tx->tx_mu);
    return snap;
}

vx_status_t vx_tx_stage_op(vx_tx_t *tx, uint32_t op_type,
                           const uint8_t *payload, uint32_t payload_len) {
    if (!tx || op_type < VX_WAL_OP_QUERY || op_type >= VX_WAL_OP_TX_BATCH) {
        return VX_ERR_INVALID;
    }

    pthread_mutex_lock(&tx->tx_mu);
    if (tx->state != VX_TX_ACTIVE) {
        pthread_mutex_unlock(&tx->tx_mu);
        return VX_ERR_INVALID;
    }

    /* Step 1: Apply mutation onto private COW fork (validates unique/schema before WAL buffer) */
    vx_owner_t *next_cat   = NULL;
    uint64_t    table_bit  = 0;
    size_t      hot_added  = 0;
    vx_status_t st = tx->ops->apply_cow_op(tx->db_ctx, tx->staged_cat, op_type,
                                           payload, payload_len,
                                           &next_cat, &table_bit, &hot_added);
    if (st != VX_OK) {
        pthread_mutex_unlock(&tx->tx_mu);
        return st;
    }

    /* Step 2: Buffer sub-op for single atomic WAL commit (rollback next_cat on OOM) */
    st = vx_tx_append_subop_buf(tx, op_type, payload, payload_len);
    if (st != VX_OK) {
        vx_owner_release(next_cat);
        pthread_mutex_unlock(&tx->tx_mu);
        return st;
    }

    vx_owner_release(tx->staged_cat);
    tx->staged_cat         = next_cat;
    tx->write_tables_mask |= table_bit;
    tx->hot_bytes_delta   += hot_added;

    pthread_mutex_unlock(&tx->tx_mu);
    return VX_OK;
}

static void vx_tx_release_snapshots_locked(vx_tx_t *tx) {
    if (tx->staged_cat) { vx_owner_release(tx->staged_cat); tx->staged_cat = NULL; }
    if (tx->base_cat)   { vx_owner_release(tx->base_cat);   tx->base_cat   = NULL; }
    free(tx->batch_buf);
    tx->batch_buf = NULL;
    tx->batch_len = 0;
    tx->batch_cap = 0;
}

vx_status_t vx_tx_commit(vx_tx_t *tx, uint64_t *out_commit_lsn) {
    if (!tx) return VX_ERR_INVALID;
    if (out_commit_lsn) *out_commit_lsn = 0;

    pthread_mutex_lock(&tx->tx_mu);
    if (tx->state != VX_TX_ACTIVE) {
        pthread_mutex_unlock(&tx->tx_mu);
        return VX_ERR_INVALID;
    }

    /* Fast path: read-only transaction commits with zero WAL I/O and zero locks */
    if (tx->n_ops == 0) {
        tx->state = VX_TX_COMMITTED;
        vx_tx_release_snapshots_locked(tx);
        pthread_mutex_unlock(&tx->tx_mu);
        return VX_OK;
    }

    /* Finalize 16-byte VX_WAL_OP_TX_BATCH payload header */
    vx_tx_wal_hdr_t wh = { .tx_id = tx->tx_id, .n_ops = tx->n_ops, ._reserved = 0u };
    memcpy(tx->batch_buf, &wh, sizeof(wh));

    /* Reserve Hot RAM quota OUTSIDE write_mu so background flusher cannot deadlock */
    bool hot_reserved = false;
    if (tx->hot_bytes_delta > 0 && tx->ops->reserve_hot) {
        vx_status_t rst = tx->ops->reserve_hot(tx->db_ctx, tx->hot_bytes_delta);
        if (rst != VX_OK) {
            tx->state = VX_TX_ABORTED;
            vx_tx_release_snapshots_locked(tx);
            pthread_mutex_unlock(&tx->tx_mu);
            return rst; /* VX_ERR_MEMLIM */
        }
        hot_reserved = true;
    }

    if (tx->ops->drain_wmesh) tx->ops->drain_wmesh(tx->db_ctx);

    pthread_mutex_lock(tx->write_mu);

    uint64_t    cur_seq     = 0;
    vx_owner_t *pub_cat     = tx->ops->current_published(tx->db_ctx, &cur_seq);
    vx_owner_t *commit_cat  = NULL;
    vx_status_t st          = VX_OK;

    if (pub_cat == tx->base_cat && cur_seq == tx->base_commit_seq) {
        /* Uncontended O(1) fast path: staged_cat is an exact COW successor of published_cat */
        commit_cat = vx_owner_retain(tx->staged_cat);
    } else {
        /* Concurrent commit or background flush/compaction occurred since tx_begin */
        if (cur_seq != tx->base_commit_seq &&
            tx->ops->has_write_conflict(tx->db_ctx, tx->base_commit_seq,
                                        tx->read_tables_mask, tx->write_tables_mask)) {
            st = VX_ERR_CONFLICT;
        } else {
            /* Safe to rebase: replay buffered sub-ops onto current published_cat */
            st = vx_tx_apply_batch_payload(tx->db_ctx, tx->ops, pub_cat,
                                           tx->batch_buf, (uint32_t)tx->batch_len,
                                           &commit_cat, NULL);
        }
    }

    uint64_t commit_lsn = 0;
    if (st == VX_OK) {
        commit_lsn = tx->ops->next_lsn(tx->db_ctx);
        if (tx->wal_fd >= 0) {
            st = vx_wal_append_frame(tx->wal_fd, commit_lsn, VX_WAL_OP_TX_BATCH,
                                     tx->batch_buf, (uint32_t)tx->batch_len, tx->wal_sync);
        }
    }

    if (st == VX_OK) {
        VX_FAILPOINT("wal_tx_post_sync_pre_ebr");
        /* Single atomic EBR root swap: all readers immediately see entire batch or none */
        tx->ops->publish_ebr(tx->db_ctx, commit_cat, commit_lsn, tx->write_tables_mask);
        tx->state = VX_TX_COMMITTED;
        if (out_commit_lsn) *out_commit_lsn = commit_lsn;
    } else {
        if (commit_cat) vx_owner_release(commit_cat);
        tx->state = VX_TX_ABORTED;
    }

    pthread_mutex_unlock(tx->write_mu);

    if (st != VX_OK && hot_reserved && tx->ops->release_hot) {
        tx->ops->release_hot(tx->db_ctx, tx->hot_bytes_delta);
    }

    vx_tx_release_snapshots_locked(tx);
    pthread_mutex_unlock(&tx->tx_mu);
    return st;
}

void vx_tx_rollback(vx_tx_t *tx) {
    if (!tx) return;
    pthread_mutex_lock(&tx->tx_mu);
    if (tx->state == VX_TX_ACTIVE) {
        tx->state = VX_TX_ABORTED;
        vx_tx_release_snapshots_locked(tx);
    }
    pthread_mutex_unlock(&tx->tx_mu);
}

void vx_tx_destroy(vx_tx_t *tx) {
    if (!tx) return;
    vx_tx_rollback(tx);
    pthread_mutex_destroy(&tx->tx_mu);
    free(tx);
}

/* ============================================================================
 * 4. Recovery Replay Hook for VX_WAL_OP_TX_BATCH (called inside vx_recover_open)
 * ============================================================================ */
vx_status_t vx_tx_replay_batch(void *db_ctx, const vx_tx_db_ops_t *ops,
                               const vx_wal_frame_t *frame) {
    if (!db_ctx || !ops || !frame || frame->op_type != VX_WAL_OP_TX_BATCH) {
        return VX_ERR_INVALID;
    }
    uint64_t    cur_seq    = 0;
    vx_owner_t *pub_cat    = ops->current_published(db_ctx, &cur_seq);
    vx_owner_t *replayed   = NULL;
    uint64_t    write_mask = 0;

    vx_status_t st = vx_tx_apply_batch_payload(db_ctx, ops, pub_cat,
                                               frame->payload, frame->payload_len,
                                               &replayed, &write_mask);
    if (st != VX_OK) return st;

    ops->publish_ebr(db_ctx, replayed, frame->lsn, write_mask);
    return VX_OK;
}
