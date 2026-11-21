#include "vx_wmesh.h"
#include "vx_spsc.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VX_WMESH_MAX_POLL_US 500U

struct vx_wmesh_tx {
    vx_spsc_t   ring;
    vx_wmesh_t *owner;
    uint64_t    submitted_seq;

    _Alignas(VX_CACHELINE) atomic_int           active;
    _Alignas(VX_CACHELINE) atomic_int           claimed;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t committed_seq;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t backpressure_waits;
};

struct vx_wmesh {
    vx_wmesh_tx_t     *txs;
    vx_write_op_t     *batch_buf;
    uint64_t          *batch_max_seq;
    size_t             max_producers;
    size_t             ring_cap_pow2;
    size_t             batch_max_ops;
    uint32_t           flush_interval_us;
    vx_wmesh_commit_fn commit_fn;
    void              *user_ctx;

    _Alignas(VX_CACHELINE) atomic_size_t        reg_count;
    _Alignas(VX_CACHELINE) atomic_int           consumer_sleeping;
    _Alignas(VX_CACHELINE) atomic_int           stopping;
    _Alignas(VX_CACHELINE) atomic_size_t        urgent_waiters;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t ops_committed;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t batches_committed;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t last_lsn;

    pthread_mutex_t ctrl_mu;
    pthread_cond_t  ctrl_cv;
    pthread_cond_t  commit_cv;
    uint64_t        flush_req_gen;
    uint64_t        flush_done_gen;
    bool            stop_req;
    bool            stopped;
    pthread_t       worker;
};

static inline bool is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

static inline uint64_t mono_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static struct timespec mono_deadline_us(uint64_t delta_us) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + delta_us * 1000ULL;
    ts.tv_sec += (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    return ts;
}

static inline void wake_consumer_if_sleeping(vx_wmesh_t *m) {
    if (atomic_load_explicit(&m->consumer_sleeping, memory_order_acquire) != 0) {
        pthread_mutex_lock(&m->ctrl_mu);
        pthread_cond_signal(&m->ctrl_cv);
        pthread_mutex_unlock(&m->ctrl_mu);
    }
}

static void *vx_wmesh_worker(void *arg) {
    vx_wmesh_t *m = (vx_wmesh_t *)arg;
    size_t *flush_drained = (size_t *)calloc(m->max_producers, sizeof(size_t));

    size_t batch_n = 0;
    uint64_t batch_start_ns = 0;
    uint64_t lsn = 0;
    size_t rr_start = 0;
    uint64_t active_flush_gen = 0;
    bool stop_seen = false;
    bool stop_drained_once = false;

    size_t fair_slice = m->batch_max_ops;
    if (m->max_producers > 1) {
        fair_slice = m->batch_max_ops / 4;
        if (fair_slice < 1) {
            fair_slice = 1;
        }
        if (fair_slice > 256) {
            fair_slice = 256;
        }
    }

    const uint64_t flush_interval_ns = (uint64_t)m->flush_interval_us * 1000ULL;

    for (;;) {
        if (!stop_seen && atomic_load_explicit(&m->stopping, memory_order_acquire)) {
            stop_seen = true;
        }
        if (active_flush_gen == 0) {
            pthread_mutex_lock(&m->ctrl_mu);
            if (m->stop_req) {
                stop_seen = true;
            }
            if (m->flush_req_gen > m->flush_done_gen) {
                active_flush_gen = m->flush_req_gen;
                if (flush_drained) {
                    memset(flush_drained, 0, m->max_producers * sizeof(size_t));
                }
            }
            pthread_mutex_unlock(&m->ctrl_mu);
        }

        size_t drained_pass = 0;
        bool all_flush_capped = true;

        for (size_t k = 0; k < m->max_producers; k++) {
            size_t i = (rr_start + k) % m->max_producers;
            if (!atomic_load_explicit(&m->txs[i].active, memory_order_acquire)) {
                continue;
            }
            size_t space = m->batch_max_ops - batch_n;
            if (space == 0) {
                all_flush_capped = false;
                break;
            }
            size_t want = space < fair_slice ? space : fair_slice;
            size_t n = vx_spsc_drain(&m->txs[i].ring, m->batch_buf + batch_n, want);
            if (n > 0) {
                if (batch_n == 0) {
                    batch_start_ns = mono_now_ns();
                }
                m->batch_max_seq[i] = m->batch_buf[batch_n + n - 1].seq;
                batch_n += n;
                drained_pass += n;
                if (active_flush_gen > 0 && flush_drained) {
                    flush_drained[i] += n;
                    if (flush_drained[i] < m->ring_cap_pow2) {
                        all_flush_capped = false;
                    }
                }
            }
        }
        rr_start = (rr_start + 1) % m->max_producers;

        bool flush_sweep_done = (active_flush_gen > 0) &&
                                (drained_pass == 0 || all_flush_capped);
        bool urgent = atomic_load_explicit(&m->urgent_waiters, memory_order_acquire) > 0;
        bool timer_expired = false;
        if (batch_n > 0 && batch_n < m->batch_max_ops && !flush_sweep_done && !stop_seen) {
            uint64_t elapsed = mono_now_ns() - batch_start_ns;
            timer_expired = (elapsed >= flush_interval_ns);
        }

        bool should_commit = (batch_n >= m->batch_max_ops) ||
                             (batch_n > 0 && (timer_expired || flush_sweep_done ||
                                              (urgent && drained_pass == 0) || stop_seen));

        if (should_commit) {
            lsn++;
            m->commit_fn(m->batch_buf, batch_n, lsn, m->user_ctx);

            atomic_fetch_add_explicit(&m->ops_committed, batch_n, memory_order_release);
            atomic_store_explicit(&m->batches_committed, lsn, memory_order_release);
            atomic_store_explicit(&m->last_lsn, lsn, memory_order_release);

            for (size_t i = 0; i < m->max_producers; i++) {
                if (m->batch_max_seq[i] > 0) {
                    atomic_store_explicit(&m->txs[i].committed_seq,
                                          m->batch_max_seq[i],
                                          memory_order_seq_cst);
                    m->batch_max_seq[i] = 0;
                }
            }
            batch_n = 0;

            if (atomic_load_explicit(&m->urgent_waiters, memory_order_seq_cst) > 0) {
                pthread_mutex_lock(&m->ctrl_mu);
                pthread_cond_broadcast(&m->commit_cv);
                pthread_mutex_unlock(&m->ctrl_mu);
            }
        }

        if (active_flush_gen > 0 && flush_sweep_done && batch_n == 0) {
            pthread_mutex_lock(&m->ctrl_mu);
            m->flush_done_gen = active_flush_gen;
            pthread_cond_broadcast(&m->ctrl_cv);
            pthread_mutex_unlock(&m->ctrl_mu);
            active_flush_gen = 0;
        }

        if (stop_seen) {
            if (drained_pass == 0 && batch_n == 0 && stop_drained_once) {
                break;
            }
            stop_drained_once = true;
            continue;
        }

        if (drained_pass == 0 && !should_commit && active_flush_gen == 0) {
            uint64_t wait_us = VX_WMESH_MAX_POLL_US;
            if (batch_n > 0) {
                uint64_t elapsed = mono_now_ns() - batch_start_ns;
                if (elapsed >= flush_interval_ns) {
                    continue;
                }
                uint64_t rem_us = (flush_interval_ns - elapsed + 999ULL) / 1000ULL;
                if (rem_us < wait_us) {
                    wait_us = rem_us;
                }
            } else if (m->flush_interval_us < wait_us) {
                wait_us = m->flush_interval_us;
            }

            struct timespec deadline = mono_deadline_us(wait_us);
            pthread_mutex_lock(&m->ctrl_mu);
            if (!m->stop_req && m->flush_req_gen == m->flush_done_gen &&
                atomic_load_explicit(&m->urgent_waiters, memory_order_acquire) == 0) {
                atomic_store_explicit(&m->consumer_sleeping, 1, memory_order_release);
                pthread_cond_timedwait(&m->ctrl_cv, &m->ctrl_mu, &deadline);
                atomic_store_explicit(&m->consumer_sleeping, 0, memory_order_release);
            }
            pthread_mutex_unlock(&m->ctrl_mu);
        }
    }

    free(flush_drained);
    pthread_mutex_lock(&m->ctrl_mu);
    m->stopped = true;
    pthread_cond_broadcast(&m->commit_cv);
    pthread_cond_broadcast(&m->ctrl_cv);
    pthread_mutex_unlock(&m->ctrl_mu);
    return NULL;
}

vx_wmesh_t *vx_wmesh_create(const vx_wmesh_cfg_t *cfg) {
    if (!cfg || cfg->max_producers == 0 || cfg->ring_cap_pow2 < 2 ||
        !is_pow2(cfg->ring_cap_pow2) || cfg->batch_max_ops == 0 ||
        cfg->flush_interval_us < 100 || !cfg->commit_fn) {
        return NULL;
    }

    void *mem = NULL;
    if (posix_memalign(&mem, VX_CACHELINE, sizeof(vx_wmesh_t)) != 0 || !mem) {
        return NULL;
    }
    vx_wmesh_t *m = (vx_wmesh_t *)mem;
    memset(m, 0, sizeof(*m));

    m->max_producers = cfg->max_producers;
    m->ring_cap_pow2 = cfg->ring_cap_pow2;
    m->batch_max_ops = cfg->batch_max_ops;
    m->flush_interval_us = cfg->flush_interval_us;
    m->commit_fn = cfg->commit_fn;
    m->user_ctx = cfg->user_ctx;

    atomic_init(&m->reg_count, 0);
    atomic_init(&m->consumer_sleeping, 0);
    atomic_init(&m->stopping, 0);
    atomic_init(&m->urgent_waiters, 0);
    atomic_init(&m->ops_committed, 0);
    atomic_init(&m->batches_committed, 0);
    atomic_init(&m->last_lsn, 0);

    void *tx_mem = NULL;
    if (posix_memalign(&tx_mem, VX_CACHELINE, cfg->max_producers * sizeof(vx_wmesh_tx_t)) != 0 || !tx_mem) {
        free(m);
        return NULL;
    }
    m->txs = (vx_wmesh_tx_t *)tx_mem;
    memset(m->txs, 0, cfg->max_producers * sizeof(vx_wmesh_tx_t));

    void *batch_mem = NULL;
    if (posix_memalign(&batch_mem, VX_CACHELINE, cfg->batch_max_ops * sizeof(vx_write_op_t)) != 0 || !batch_mem) {
        free(m->txs);
        free(m);
        return NULL;
    }
    m->batch_buf = (vx_write_op_t *)batch_mem;

    m->batch_max_seq = (uint64_t *)calloc(cfg->max_producers, sizeof(uint64_t));
    if (!m->batch_max_seq) {
        free(m->batch_buf);
        free(m->txs);
        free(m);
        return NULL;
    }

    for (size_t i = 0; i < cfg->max_producers; i++) {
        if (!vx_spsc_init(&m->txs[i].ring, cfg->ring_cap_pow2, sizeof(vx_write_op_t))) {
            for (size_t j = 0; j < i; j++) {
                vx_spsc_destroy(&m->txs[j].ring);
            }
            free(m->batch_max_seq);
            free(m->batch_buf);
            free(m->txs);
            free(m);
            return NULL;
        }
        m->txs[i].owner = m;
        atomic_init(&m->txs[i].active, 0);
        atomic_init(&m->txs[i].claimed, 0);
        atomic_init(&m->txs[i].committed_seq, 0);
        atomic_init(&m->txs[i].backpressure_waits, 0);
    }

    pthread_mutex_init(&m->ctrl_mu, NULL);
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&m->ctrl_cv, &cattr);
    pthread_cond_init(&m->commit_cv, &cattr);
    pthread_condattr_destroy(&cattr);

    if (pthread_create(&m->worker, NULL, vx_wmesh_worker, m) != 0) {
        pthread_cond_destroy(&m->commit_cv);
        pthread_cond_destroy(&m->ctrl_cv);
        pthread_mutex_destroy(&m->ctrl_mu);
        for (size_t i = 0; i < cfg->max_producers; i++) {
            vx_spsc_destroy(&m->txs[i].ring);
        }
        free(m->batch_max_seq);
        free(m->batch_buf);
        free(m->txs);
        free(m);
        return NULL;
    }

    return m;
}

void vx_wmesh_destroy(vx_wmesh_t *mesh) {
    if (!mesh) {
        return;
    }

    atomic_store_explicit(&mesh->stopping, 1, memory_order_release);
    pthread_mutex_lock(&mesh->ctrl_mu);
    mesh->stop_req = true;
    pthread_cond_broadcast(&mesh->ctrl_cv);
    pthread_cond_broadcast(&mesh->commit_cv);
    pthread_mutex_unlock(&mesh->ctrl_mu);

    pthread_join(mesh->worker, NULL);

    pthread_cond_destroy(&mesh->commit_cv);
    pthread_cond_destroy(&mesh->ctrl_cv);
    pthread_mutex_destroy(&mesh->ctrl_mu);
    for (size_t i = 0; i < mesh->max_producers; i++) {
        vx_spsc_destroy(&mesh->txs[i].ring);
    }
    free(mesh->batch_max_seq);
    free(mesh->batch_buf);
    free(mesh->txs);
    free(mesh);
}

vx_wmesh_tx_t *vx_wmesh_register(vx_wmesh_t *mesh) {
    if (!mesh) {
        return NULL;
    }
    for (size_t i = 0; i < mesh->max_producers; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&mesh->txs[i].claimed,
                                                    &expected, 1,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            atomic_store_explicit(&mesh->txs[i].active, 1, memory_order_release);
            return &mesh->txs[i];
        }
    }
    return NULL;
}

void vx_wmesh_unregister(vx_wmesh_tx_t *tx) {
    if (!tx || !tx->owner) {
        return;
    }
    if (tx->submitted_seq > 0) {
        (void)vx_wmesh_wait(tx, tx->submitted_seq);
    }
    atomic_store_explicit(&tx->claimed, 0, memory_order_release);
}

bool vx_wmesh_try_submit(vx_wmesh_tx_t *tx, const vx_write_op_t *op,
                         uint64_t *out_token) {
    if (!tx || !tx->owner || !op) {
        return false;
    }
    if (atomic_load_explicit(&tx->owner->stopping, memory_order_acquire)) {
        return false;
    }

    vx_write_op_t copy = *op;
    copy.seq = tx->submitted_seq + 1;
    if (!vx_spsc_try_push(&tx->ring, &copy)) {
        return false;
    }

    tx->submitted_seq = copy.seq;
    if (out_token) {
        *out_token = copy.seq;
    }
    wake_consumer_if_sleeping(tx->owner);
    return true;
}

bool vx_wmesh_submit(vx_wmesh_tx_t *tx, const vx_write_op_t *op,
                     uint64_t *out_token) {
    if (!tx || !tx->owner || !op) {
        return false;
    }
    if (vx_wmesh_try_submit(tx, op, out_token)) {
        return true;
    }

    vx_wmesh_t *m = tx->owner;
    atomic_fetch_add_explicit(&tx->backpressure_waits, 1, memory_order_relaxed);

    uint32_t spins = 0;
    while (!atomic_load_explicit(&m->stopping, memory_order_acquire)) {
        wake_consumer_if_sleeping(m);
        if (vx_wmesh_try_submit(tx, op, out_token)) {
            return true;
        }
        if (spins < 64) {
            spins++;
            sched_yield();
        } else {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000 }; /* 50us */
            nanosleep(&ts, NULL);
        }
    }
    return false;
}

bool vx_wmesh_wait(vx_wmesh_tx_t *tx, uint64_t token) {
    if (!tx || !tx->owner) {
        return false;
    }
    if (token == 0) {
        return true;
    }
    if (token > tx->submitted_seq) {
        return false;
    }
    if (atomic_load_explicit(&tx->committed_seq, memory_order_acquire) >= token) {
        return true;
    }

    vx_wmesh_t *m = tx->owner;
    pthread_mutex_lock(&m->ctrl_mu);
    atomic_fetch_add_explicit(&m->urgent_waiters, 1, memory_order_seq_cst);
    pthread_cond_signal(&m->ctrl_cv);

    while (atomic_load_explicit(&tx->committed_seq, memory_order_seq_cst) < token && !m->stopped) {
        pthread_cond_wait(&m->commit_cv, &m->ctrl_mu);
    }

    atomic_fetch_sub_explicit(&m->urgent_waiters, 1, memory_order_seq_cst);
    bool ok = atomic_load_explicit(&tx->committed_seq, memory_order_acquire) >= token;
    pthread_mutex_unlock(&m->ctrl_mu);
    return ok;
}

void vx_wmesh_flush(vx_wmesh_t *mesh) {
    if (!mesh) {
        return;
    }

    pthread_mutex_lock(&mesh->ctrl_mu);
    uint64_t target = ++mesh->flush_req_gen;
    pthread_cond_signal(&mesh->ctrl_cv);
    while (mesh->flush_done_gen < target && !mesh->stopped) {
        pthread_cond_wait(&mesh->ctrl_cv, &mesh->ctrl_mu);
    }
    pthread_mutex_unlock(&mesh->ctrl_mu);
}

vx_wmesh_stats_t vx_wmesh_stats(const vx_wmesh_t *mesh) {
    vx_wmesh_stats_t st = {0};
    if (!mesh) {
        return st;
    }

    st.ops_committed = atomic_load_explicit(&mesh->ops_committed, memory_order_acquire);
    st.batches_committed = atomic_load_explicit(&mesh->batches_committed, memory_order_acquire);
    st.last_lsn = atomic_load_explicit(&mesh->last_lsn, memory_order_acquire);

    uint64_t bp = 0;
    for (size_t i = 0; i < mesh->max_producers; i++) {
        bp += atomic_load_explicit(&mesh->txs[i].backpressure_waits, memory_order_relaxed);
    }
    st.backpressure_waits = bp;
    return st;
}

