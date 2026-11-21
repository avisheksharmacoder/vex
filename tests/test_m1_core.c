#include "vx_arena.h"
#include "vx_spsc.h"
#include "vx_test.h"

#include <pthread.h>
#include <string.h>

static void test_arena_invalid_args(void) {
    vx_arena_t a;
    VX_ASSERT(!vx_arena_init(NULL, 1024));
    VX_ASSERT(!vx_arena_init(&a, 0));

    VX_ASSERT(vx_arena_init(&a, 256));
    VX_ASSERT(vx_arena_alloc(&a, 16, 0) == NULL);   /* align 0 invalid */
    VX_ASSERT(vx_arena_alloc(&a, 16, 3) == NULL);   /* non-pow2 align invalid */
    VX_ASSERT(vx_arena_alloc(&a, 0, 8) == NULL);    /* zero size invalid */
    VX_ASSERT(vx_arena_used(&a) == 0);
    vx_arena_destroy(&a);
}

static void test_arena_alignment_oom_reset(void) {
    vx_arena_t a;
    VX_ASSERT(vx_arena_init(&a, 256));
    VX_ASSERT(vx_arena_cap(&a) == 256);

    uint8_t *p1 = vx_arena_alloc(&a, 1, 1);
    VX_ASSERT(p1 != NULL);
    *p1 = 0xAA;

    /* Force 64-byte SIMD alignment after a 1-byte allocation */
    double *p64 = vx_arena_alloc(&a, 8 * sizeof(double), 64);
    VX_ASSERT(p64 != NULL);
    VX_ASSERT(((uintptr_t)p64 % 64) == 0);
    for (int i = 0; i < 8; i++) p64[i] = (double)i;

    size_t used_before_oom = vx_arena_used(&a);
    VX_ASSERT(vx_arena_alloc(&a, 256, 8) == NULL);  /* must fail cleanly */
    VX_ASSERT(vx_arena_used(&a) == used_before_oom); /* offset untouched */

    vx_arena_reset(&a);
    VX_ASSERT(vx_arena_used(&a) == 0);

    /* Full reuse after reset */
    void *p2 = vx_arena_alloc(&a, 256, 64);
    VX_ASSERT(p2 != NULL);
    memset(p2, 0x55, 256);

    vx_arena_destroy(&a);
}

static void test_spsc_init_validation(void) {
    vx_spsc_t q;
    VX_ASSERT(!vx_spsc_init(NULL, 8, 8));
    VX_ASSERT(!vx_spsc_init(&q, 0, 8));
    VX_ASSERT(!vx_spsc_init(&q, 1, 8));             /* min capacity is 2 */
    VX_ASSERT(!vx_spsc_init(&q, 6, 8));             /* non-pow2 */
    VX_ASSERT(!vx_spsc_init(&q, 8, 0));             /* zero slot_size */
    VX_ASSERT(vx_spsc_init(&q, 4, sizeof(uint64_t)));
    vx_spsc_destroy(&q);
}

static void test_spsc_fifo_wrap_and_drain(void) {
    vx_spsc_t q;
    VX_ASSERT(vx_spsc_init(&q, 4, sizeof(uint64_t)));

    uint64_t out = 0;
    VX_ASSERT(!vx_spsc_try_pop(&q, &out));          /* empty */

    for (uint64_t i = 1; i <= 4; i++) {
        VX_ASSERT(vx_spsc_try_push(&q, &i));
    }
    uint64_t extra = 99;
    VX_ASSERT(!vx_spsc_try_push(&q, &extra));       /* full (all 4 slots used) */
    VX_ASSERT(vx_spsc_size_approx(&q) == 4);

    /* Pop 2, push 2 to exercise wrap-around */
    VX_ASSERT(vx_spsc_try_pop(&q, &out) && out == 1);
    VX_ASSERT(vx_spsc_try_pop(&q, &out) && out == 2);

    uint64_t v5 = 5, v6 = 6;
    VX_ASSERT(vx_spsc_try_push(&q, &v5));
    VX_ASSERT(vx_spsc_try_push(&q, &v6));

    uint64_t batch[8] = {0};
    size_t n = vx_spsc_drain(&q, batch, 8);
    VX_ASSERT(n == 4);
    VX_ASSERT(batch[0] == 3 && batch[1] == 4 && batch[2] == 5 && batch[3] == 6);
    VX_ASSERT(vx_spsc_size_approx(&q) == 0);

    vx_spsc_destroy(&q);
}

typedef struct {
    vx_spsc_t *q;
    uint64_t   count;
    bool       ok;
} spsc_mt_ctx_t;

static void *producer_fn(void *arg) {
    spsc_mt_ctx_t *ctx = (spsc_mt_ctx_t *)arg;
    for (uint64_t i = 0; i < ctx->count; i++) {
        while (!vx_spsc_try_push(ctx->q, &i)) {
            /* spin until consumer frees space */
        }
    }
    return NULL;
}

static void *consumer_fn(void *arg) {
    spsc_mt_ctx_t *ctx = (spsc_mt_ctx_t *)arg;
    uint64_t buf[64];
    uint64_t expected = 0;
    while (expected < ctx->count) {
        size_t n = vx_spsc_drain(ctx->q, buf, 64);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != expected) {
                ctx->ok = false;
                return NULL;
            }
            expected++;
        }
    }
    ctx->ok = true;
    return NULL;
}

static void test_spsc_mt_stress_and_sla(void) {
    vx_spsc_t q;
    VX_ASSERT(vx_spsc_init(&q, 4096, sizeof(uint64_t)));

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const uint64_t total = 250000;
#else
    const uint64_t total = 5000000;
#endif

    spsc_mt_ctx_t ctx = { .q = &q, .count = total, .ok = false };
    pthread_t prod, cons;

    double t0 = vx_now_sec();
    VX_ASSERT(pthread_create(&cons, NULL, consumer_fn, &ctx) == 0);
    VX_ASSERT(pthread_create(&prod, NULL, producer_fn, &ctx) == 0);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    double dt = vx_now_sec() - t0;

    VX_ASSERT(ctx.ok);
    VX_ASSERT(vx_spsc_size_approx(&q) == 0);

    double mops = ((double)total / dt) / 1e6;
    printf("    [METRIC] SPSC MT throughput: %.2f M msgs/sec (%.3fs)\n", mops, dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* Release-build performance tripwire: >= 20M msgs/sec */
    VX_ASSERT(mops >= 20.0);
#endif

    vx_spsc_destroy(&q);
}

int main(void) {
    VX_RUN(test_arena_invalid_args);
    VX_RUN(test_arena_alignment_oom_reset);
    VX_RUN(test_spsc_init_validation);
    VX_RUN(test_spsc_fifo_wrap_and_drain);
    VX_RUN(test_spsc_mt_stress_and_sla);
    return vx_test_summary("M1 Core: Arena + SPSC");
}

