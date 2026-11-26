#pragma once
#include "vx_batch.h"

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_cmp_op : uint8_t {
    VX_CMP_EQ = 0, VX_CMP_NE = 1,
    VX_CMP_LT = 2, VX_CMP_LE = 3,
    VX_CMP_GT = 4, VX_CMP_GE = 5
} vx_cmp_op_t;

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_builtin_id : uint8_t {
    VX_FN_LEN        = 0, /* str -> i64 (UTF-8 codepoints or byte len per V1 semantics) */
    VX_FN_LOWER      = 1, /* str -> str */
    VX_FN_UPPER      = 2, /* str -> str */
    VX_FN_STARTSWITH = 3, /* (str, str) -> bool */
    VX_FN_CONTAINS   = 4, /* (str, str) -> bool */
    VX_FN_COALESCE   = 5, /* (T, T) -> T */
    VX_FN_ABS        = 6, /* i64|f64 -> i64|f64 */
    VX_FN_ROUND      = 7  /* (f64, i64) -> f64 */
} vx_builtin_id_t;

typedef struct vx_agg_minmax_i64 { int64_t  min_v, max_v; uint64_t valid_n; } vx_agg_minmax_i64_t;
typedef struct vx_agg_minmax_u64 { uint64_t min_v, max_v; uint64_t valid_n; } vx_agg_minmax_u64_t;
typedef struct vx_agg_minmax_f64 { double   min_v, max_v; uint64_t valid_n; } vx_agg_minmax_f64_t;

typedef struct vx_simd_ops {
    const char *isa_name; /* "avx2", "neon", or "scalar" */

    /* --- Typed Scalar-vs-Vector Filter Kernels (return new out_sel->count) --- */
    uint32_t (*filter_i64)(const int64_t *col, const uint64_t *validity,
                           const vx_sel_t *in_sel, vx_sel_t *out_sel,
                           vx_cmp_op_t op, int64_t rhs);
    uint32_t (*filter_u64)(const uint64_t *col, const uint64_t *validity,
                           const vx_sel_t *in_sel, vx_sel_t *out_sel,
                           vx_cmp_op_t op, uint64_t rhs);
    uint32_t (*filter_f64)(const double *col, const uint64_t *validity,
                           const vx_sel_t *in_sel, vx_sel_t *out_sel,
                           vx_cmp_op_t op, double rhs);
    uint32_t (*filter_bool)(const uint8_t *col, const uint64_t *validity,
                            const vx_sel_t *in_sel, vx_sel_t *out_sel,
                            vx_cmp_op_t op, uint8_t rhs);

    /* 4B prefix + length SIMD pre-filter for StringView (touches pool only on prefix hit) */
    uint32_t (*filter_str_eq)(const vx_strview_t *col, const vx_strpool_t *pool,
                              const uint64_t *validity, const vx_sel_t *in_sel,
                              vx_sel_t *out_sel, const uint8_t *rhs, int32_t rhs_len,
                              bool negate);

    /* Null predicate filter (== null / != null) */
    uint32_t (*filter_null)(const uint64_t *validity, const vx_sel_t *in_sel,
                            vx_sel_t *out_sel, bool want_null);

    /* --- Vectorized Reductions (exact IEEE-754 parity on f64; overflow-safe i64/u64) --- */
    int64_t             (*sum_i64)(const int64_t *col, const uint64_t *validity,
                                   const vx_sel_t *sel, uint64_t *out_valid_n);
    uint64_t            (*sum_u64)(const uint64_t *col, const uint64_t *validity,
                                   const vx_sel_t *sel, uint64_t *out_valid_n);
    double              (*sum_f64)(const double *col, const uint64_t *validity,
                                   const vx_sel_t *sel, uint64_t *out_valid_n);
    vx_agg_minmax_i64_t (*minmax_i64)(const int64_t *col, const uint64_t *validity, const vx_sel_t *sel);
    vx_agg_minmax_u64_t (*minmax_u64)(const uint64_t *col, const uint64_t *validity, const vx_sel_t *sel);
    vx_agg_minmax_f64_t (*minmax_f64)(const double *col, const uint64_t *validity, const vx_sel_t *sel);
} vx_simd_ops_t;

const vx_simd_ops_t *vx_simd_detect(void);
const vx_simd_ops_t *vx_simd_scalar_ops(void); /* exposed for CI cross-ISA parity tests */

/* Batch-at-a-time scalar built-in dispatcher */
vx_status_t vx_builtin_eval(vx_builtin_id_t fn, const vx_vec_t *args, uint32_t nargs,
                            const vx_sel_t *sel, vx_arena_t *scratch, vx_vec_t *out);

