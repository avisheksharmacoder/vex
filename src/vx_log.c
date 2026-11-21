#include "vx_log.h"
#include "vx_spsc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VX_LOG_DRAIN_BATCH 512

struct vx_log_tx {
    vx_spsc_t      ring;
    vx_logger_t   *owner;
    vx_log_level_t min_level;
    uint32_t       tid;
    uint64_t       seq;
    _Alignas(VX_CACHELINE) atomic_int           active;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t dropped;
};

struct vx_logger {
    vx_log_tx_t   *txs;
    vx_log_rec_t  *drain_buf;
    size_t         max_producers;
    size_t         ring_cap_pow2;
    vx_log_level_t min_level;
    uint32_t       flush_interval_us;
    vx_log_sink_fn sink;
    void          *user_ctx;

    _Alignas(VX_CACHELINE) atomic_size_t        reg_count;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t emitted;
    _Alignas(VX_CACHELINE) atomic_uint_fast64_t flushes;

    pthread_mutex_t ctrl_mu;
    pthread_cond_t  ctrl_cv;
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

static void *vx_log_worker(void *arg) {
    vx_logger_t *log = (vx_logger_t *)arg;
    size_t *flush_drained = (size_t *)calloc(log->max_producers, sizeof(size_t));
    size_t buf_n = 0;
    uint64_t active_flush_gen = 0;
    bool stop_seen = false;
    bool stop_drained_once = false;

    for (;;) {
        pthread_mutex_lock(&log->ctrl_mu);
        if (log->stop_req) {
            stop_seen = true;
        }
        if (active_flush_gen == 0 && log->flush_req_gen > log->flush_done_gen) {
            active_flush_gen = log->flush_req_gen;
            if (flush_drained) {
                memset(flush_drained, 0, log->max_producers * sizeof(size_t));
            }
        }
        pthread_mutex_unlock(&log->ctrl_mu);

        size_t drained_pass = 0;
        bool all_flush_capped = true;

        for (size_t i = 0; i < log->max_producers; i++) {
            if (!atomic_load_explicit(&log->txs[i].active, memory_order_acquire)) {
                continue;
            }
            size_t space = VX_LOG_DRAIN_BATCH - buf_n;
            if (space == 0) {
                log->sink(log->drain_buf, buf_n, log->user_ctx);
                atomic_fetch_add_explicit(&log->emitted, buf_n, memory_order_release);
                atomic_fetch_add_explicit(&log->flushes, 1, memory_order_release);
                buf_n = 0;
                space = VX_LOG_DRAIN_BATCH;
            }

            size_t n = vx_spsc_drain(&log->txs[i].ring, log->drain_buf + buf_n, space);
            buf_n += n;
            drained_pass += n;

            if (active_flush_gen > 0 && flush_drained) {
                flush_drained[i] += n;
                if (n > 0 && flush_drained[i] < log->ring_cap_pow2) {
                    all_flush_capped = false;
                }
            }
        }

        if (buf_n > 0 && (buf_n >= VX_LOG_DRAIN_BATCH || drained_pass == 0 ||
                          active_flush_gen > 0 || stop_seen)) {
            log->sink(log->drain_buf, buf_n, log->user_ctx);
            atomic_fetch_add_explicit(&log->emitted, buf_n, memory_order_release);
            atomic_fetch_add_explicit(&log->flushes, 1, memory_order_release);
            buf_n = 0;
        }

        if (active_flush_gen > 0 && (drained_pass == 0 || all_flush_capped) && buf_n == 0) {
            pthread_mutex_lock(&log->ctrl_mu);
            log->flush_done_gen = active_flush_gen;
            pthread_cond_broadcast(&log->ctrl_cv);
            pthread_mutex_unlock(&log->ctrl_mu);
            active_flush_gen = 0;
        }

        if (stop_seen) {
            if (drained_pass == 0 && buf_n == 0 && stop_drained_once) {
                break;
            }
            stop_drained_once = true;
            continue;
        }

        if (drained_pass == 0 && active_flush_gen == 0) {
            struct timespec deadline = mono_deadline_us(log->flush_interval_us);
            pthread_mutex_lock(&log->ctrl_mu);
            if (!log->stop_req && log->flush_req_gen == log->flush_done_gen) {
                pthread_cond_timedwait(&log->ctrl_cv, &log->ctrl_mu, &deadline);
            }
            pthread_mutex_unlock(&log->ctrl_mu);
        }
    }

    free(flush_drained);
    pthread_mutex_lock(&log->ctrl_mu);
    log->stopped = true;
    pthread_cond_broadcast(&log->ctrl_cv);
    pthread_mutex_unlock(&log->ctrl_mu);
    return NULL;
}

vx_logger_t *vx_log_create(const vx_log_cfg_t *cfg) {
    if (!cfg || cfg->max_producers == 0 || cfg->ring_cap_pow2 < 2 ||
        !is_pow2(cfg->ring_cap_pow2) || !cfg->sink) {
        return NULL;
    }

    void *mem = NULL;
    if (posix_memalign(&mem, VX_CACHELINE, sizeof(vx_logger_t)) != 0 || !mem) {
        return NULL;
    }
    vx_logger_t *log = (vx_logger_t *)mem;
    memset(log, 0, sizeof(*log));

    log->max_producers = cfg->max_producers;
    log->ring_cap_pow2 = cfg->ring_cap_pow2;
    log->min_level = cfg->min_level;
    log->flush_interval_us = cfg->flush_interval_us ? cfg->flush_interval_us : 1000;
    log->sink = cfg->sink;
    log->user_ctx = cfg->user_ctx;

    atomic_init(&log->reg_count, 0);
    atomic_init(&log->emitted, 0);
    atomic_init(&log->flushes, 0);

    void *tx_mem = NULL;
    if (posix_memalign(&tx_mem, VX_CACHELINE, cfg->max_producers * sizeof(vx_log_tx_t)) != 0 || !tx_mem) {
        free(log);
        return NULL;
    }
    log->txs = (vx_log_tx_t *)tx_mem;
    memset(log->txs, 0, cfg->max_producers * sizeof(vx_log_tx_t));

    log->drain_buf = (vx_log_rec_t *)malloc(VX_LOG_DRAIN_BATCH * sizeof(vx_log_rec_t));
    if (!log->drain_buf) {
        free(log->txs);
        free(log);
        return NULL;
    }

    for (size_t i = 0; i < cfg->max_producers; i++) {
        if (!vx_spsc_init(&log->txs[i].ring, cfg->ring_cap_pow2, sizeof(vx_log_rec_t))) {
            for (size_t j = 0; j < i; j++) {
                vx_spsc_destroy(&log->txs[j].ring);
            }
            free(log->drain_buf);
            free(log->txs);
            free(log);
            return NULL;
        }
        log->txs[i].owner = log;
        log->txs[i].min_level = cfg->min_level;
        atomic_init(&log->txs[i].active, 0);
        atomic_init(&log->txs[i].dropped, 0);
    }

    pthread_mutex_init(&log->ctrl_mu, NULL);
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&log->ctrl_cv, &cattr);
    pthread_condattr_destroy(&cattr);

    if (pthread_create(&log->worker, NULL, vx_log_worker, log) != 0) {
        pthread_cond_destroy(&log->ctrl_cv);
        pthread_mutex_destroy(&log->ctrl_mu);
        for (size_t i = 0; i < cfg->max_producers; i++) {
            vx_spsc_destroy(&log->txs[i].ring);
        }
        free(log->drain_buf);
        free(log->txs);
        free(log);
        return NULL;
    }

    return log;
}

void vx_log_destroy(vx_logger_t *log) {
    if (!log) {
        return;
    }

    pthread_mutex_lock(&log->ctrl_mu);
    log->stop_req = true;
    pthread_cond_broadcast(&log->ctrl_cv);
    pthread_mutex_unlock(&log->ctrl_mu);

    pthread_join(log->worker, NULL);

    pthread_cond_destroy(&log->ctrl_cv);
    pthread_mutex_destroy(&log->ctrl_mu);
    for (size_t i = 0; i < log->max_producers; i++) {
        vx_spsc_destroy(&log->txs[i].ring);
    }
    free(log->drain_buf);
    free(log->txs);
    free(log);
}

vx_log_tx_t *vx_log_register(vx_logger_t *log, uint32_t tid) {
    if (!log) {
        return NULL;
    }

    size_t cur = atomic_load_explicit(&log->reg_count, memory_order_relaxed);
    while (cur < log->max_producers) {
        if (atomic_compare_exchange_weak_explicit(&log->reg_count, &cur, cur + 1,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            vx_log_tx_t *tx = &log->txs[cur];
            tx->tid = tid;
            tx->seq = 0;
            atomic_store_explicit(&tx->active, 1, memory_order_release);
            return tx;
        }
    }
    return NULL;
}

bool vx_log_emit(vx_log_tx_t *tx, vx_log_level_t level,
                 uint16_t event_id, const char *tag,
                 uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!tx) {
        return false;
    }
    if (level < tx->min_level) {
        return true;
    }

    vx_log_rec_t rec;
    rec.ts_ns = mono_now_ns();
    rec.tid = tx->tid;
    rec.event_id = event_id;
    rec.level = (uint8_t)level;
    rec._pad = 0;
    rec.a0 = a0;
    rec.a1 = a1;
    rec.a2 = a2;
    rec.a3 = a3;
    rec.tag = tag;
    rec.seq = ++tx->seq;

    if (!vx_spsc_try_push(&tx->ring, &rec)) {
        atomic_fetch_add_explicit(&tx->dropped, 1, memory_order_relaxed);
        return false;
    }
    return true;
}

void vx_log_flush(vx_logger_t *log) {
    if (!log) {
        return;
    }

    pthread_mutex_lock(&log->ctrl_mu);
    uint64_t target = ++log->flush_req_gen;
    pthread_cond_signal(&log->ctrl_cv);
    while (log->flush_done_gen < target && !log->stopped) {
        pthread_cond_wait(&log->ctrl_cv, &log->ctrl_mu);
    }
    pthread_mutex_unlock(&log->ctrl_mu);
}

vx_log_stats_t vx_log_stats(const vx_logger_t *log) {
    vx_log_stats_t st = {0};
    if (!log) {
        return st;
    }

    st.emitted = atomic_load_explicit(&log->emitted, memory_order_acquire);
    st.flushes = atomic_load_explicit(&log->flushes, memory_order_acquire);
    size_t n = atomic_load_explicit(&log->reg_count, memory_order_acquire);
    if (n > log->max_producers) {
        n = log->max_producers;
    }
    uint64_t dropped = 0;
    for (size_t i = 0; i < n; i++) {
        dropped += atomic_load_explicit(&log->txs[i].dropped, memory_order_relaxed);
    }
    st.dropped = dropped;
    return st;
}

