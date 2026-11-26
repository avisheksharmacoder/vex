#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(VEX_ENABLE_FAILPOINTS) && VEX_ENABLE_FAILPOINTS
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct vx_fp_cfg {
    const char      *target_name;
    uint64_t         target_hit;  /* 1-based hit index; 0 = disabled */
    _Atomic uint64_t hit_count;
} vx_fp_cfg_t;

static vx_fp_cfg_t    g_vx_fp_cfg  = { NULL, 0, 0 };
static pthread_once_t g_vx_fp_once = PTHREAD_ONCE_INIT;

static void vx_fp_init(void) {
    const char *name = getenv("VEX_CRASH_POINT");
    const char *hit  = getenv("VEX_CRASH_HIT");
    if (name && name[0]) {
        g_vx_fp_cfg.target_name = name;
        g_vx_fp_cfg.target_hit  = (hit && hit[0]) ? strtoull(hit, NULL, 10) : 1ULL;
    }
}

static inline void vx_failpoint_hit(const char *name) {
    pthread_once(&g_vx_fp_once, vx_fp_init);
    if (!g_vx_fp_cfg.target_name || g_vx_fp_cfg.target_hit == 0) return;
    if (strcmp(name, g_vx_fp_cfg.target_name) != 0) return;

    uint64_t n = atomic_fetch_add_explicit(&g_vx_fp_cfg.hit_count, 1ULL, memory_order_acq_rel) + 1ULL;
    if (n == g_vx_fp_cfg.target_hit) {
        _exit(137); /* hard crash: no stdio flush, no atexit, no flock unlock cleanup */
    }
}

/* Returns true if the active failpoint matches `name` (used to inject torn partial WAL writes) */
static inline bool vx_failpoint_armed(const char *name) {
    pthread_once(&g_vx_fp_once, vx_fp_init);
    return g_vx_fp_cfg.target_name && strcmp(name, g_vx_fp_cfg.target_name) == 0;
}

#define VX_FAILPOINT(name) vx_failpoint_hit(name)
#else
#define VX_FAILPOINT(name) ((void)0)
static inline bool vx_failpoint_armed(const char *name) { (void)name; return false; }
#endif
