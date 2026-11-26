#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(VEX_ENABLE_OOM_INJECT) && VEX_ENABLE_OOM_INJECT
typedef struct vx_oom_tls {
    bool     enabled;
    bool     paused;
    uint64_t fail_at;   /* 1-based allocation count to fail; 0 = count only */
    uint64_t count;
    uint64_t triggered;
} vx_oom_tls_t;

extern _Thread_local vx_oom_tls_t g_vx_oom_tls;

static inline void vx_oom_arm(uint64_t fail_at) {
    g_vx_oom_tls.enabled   = true;
    g_vx_oom_tls.paused    = false;
    g_vx_oom_tls.fail_at   = fail_at;
    g_vx_oom_tls.count     = 0;
    g_vx_oom_tls.triggered = 0;
}
static inline void vx_oom_disarm(void) { g_vx_oom_tls.enabled = false; }
static inline void vx_oom_pause(void)  { g_vx_oom_tls.paused  = true;  }
static inline void vx_oom_resume(void) { g_vx_oom_tls.paused  = false; }

static inline bool vx_oom_should_fail(void) {
    if (!g_vx_oom_tls.enabled || g_vx_oom_tls.paused) return false;
    uint64_t n = ++g_vx_oom_tls.count;
    if (g_vx_oom_tls.fail_at > 0 && n >= g_vx_oom_tls.fail_at) {
        g_vx_oom_tls.triggered++;
        return true;
    }
    return false;
}

static inline void *vx_malloc(size_t sz) {
    return vx_oom_should_fail() ? NULL : malloc(sz);
}
static inline void *vx_calloc(size_t n, size_t sz) {
    return vx_oom_should_fail() ? NULL : calloc(n, sz);
}
static inline void *vx_realloc(void *p, size_t sz) {
    return vx_oom_should_fail() ? NULL : realloc(p, sz);
}
static inline int vx_posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (vx_oom_should_fail()) return ENOMEM;
    return posix_memalign(memptr, alignment, size);
}
static inline void *vx_aligned_alloc64(size_t sz) {
    if (vx_oom_should_fail()) return NULL;
    void *p = NULL;
    return (posix_memalign(&p, 64u, sz) == 0) ? p : NULL;
}
static inline void vx_free(void *p) { free(p); }

#define malloc(sz) vx_malloc(sz)
#define calloc(n, sz) vx_calloc(n, sz)
#define realloc(p, sz) vx_realloc(p, sz)
#define posix_memalign(p, a, s) vx_posix_memalign((p), (a), (s))

#else
#define vx_oom_arm(k)     ((void)(k))
#define vx_oom_disarm()   ((void)0)
#define vx_oom_pause()    ((void)0)
#define vx_oom_resume()   ((void)0)
static inline void *vx_malloc(size_t sz)           { return malloc(sz); }
static inline void *vx_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static inline void *vx_realloc(void *p, size_t sz) { return realloc(p, sz); }
static inline void *vx_aligned_alloc64(size_t sz)  {
    void *p = NULL;
    return (posix_memalign(&p, 64u, sz) == 0) ? p : NULL;
}
static inline void  vx_free(void *p)               { free(p); }
#endif
