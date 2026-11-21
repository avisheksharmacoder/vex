#ifndef VX_SIMD_H
#define VX_SIMD_H

#include "vx_chunk.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VX_SIMD_SCALAR = 0,
    VX_SIMD_AVX2   = 1,
    VX_SIMD_NEON   = 2
} vx_simd_level_t;

/* Hardware-detected best SIMD backend (cached once at startup) */
vx_simd_level_t vx_simd_detect(void);

/* Currently active backend used by vx_filter_* kernels */
vx_simd_level_t vx_simd_active(void);

/* Human-readable backend name: "scalar", "avx2", or "neon" */
const char     *vx_simd_name(vx_simd_level_t lvl);

/* Overrides active backend for parity testing; returns false if unsupported on CPU */
bool            vx_simd_force_level(vx_simd_level_t lvl);

/* Internal AVX2 translation unit entrypoint (compiled with -mavx2 on x86_64) */
uint32_t vx_filter_f64_avx2_dispatch(const double *col, uint32_t n,
                                     vx_cmp_op_t op, double val, uint32_t *sel_out);

#endif /* VX_SIMD_H */
