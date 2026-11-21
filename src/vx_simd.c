#include "vx_simd.h"

#include <pthread.h>
#include <stdatomic.h>

static pthread_once_t  g_simd_once = PTHREAD_ONCE_INIT;
static vx_simd_level_t g_simd_detected = VX_SIMD_SCALAR;
static atomic_int      g_simd_active = VX_SIMD_SCALAR;

static void simd_detect_init(void) {
    vx_simd_level_t lvl = VX_SIMD_SCALAR;

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) && !defined(__clang__)
    __builtin_cpu_init();
#endif
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx2")) {
        lvl = VX_SIMD_AVX2;
    }
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    lvl = VX_SIMD_NEON;
#endif

    g_simd_detected = lvl;
    atomic_store_explicit(&g_simd_active, (int)lvl, memory_order_release);
}

vx_simd_level_t vx_simd_detect(void) {
    pthread_once(&g_simd_once, simd_detect_init);
    return g_simd_detected;
}

vx_simd_level_t vx_simd_active(void) {
    pthread_once(&g_simd_once, simd_detect_init);
    return (vx_simd_level_t)atomic_load_explicit(&g_simd_active, memory_order_acquire);
}

const char *vx_simd_name(vx_simd_level_t lvl) {
    switch (lvl) {
        case VX_SIMD_AVX2:   return "avx2";
        case VX_SIMD_NEON:   return "neon";
        case VX_SIMD_SCALAR:
        default:             return "scalar";
    }
}

bool vx_simd_force_level(vx_simd_level_t lvl) {
    vx_simd_level_t det = vx_simd_detect();
    if (lvl != VX_SIMD_SCALAR && lvl != det) {
        return false;
    }
    atomic_store_explicit(&g_simd_active, (int)lvl, memory_order_release);
    return true;
}

