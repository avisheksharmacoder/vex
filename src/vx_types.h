#pragma once
#include <stdatomic.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_ALIGN        64u
#define VX_BATCH_CAP    4096u
#define VX_BITMAP_WORDS (VX_BATCH_CAP / 64u) /* 64 uint64_t words = 512 B */

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_status : int32_t {
    VX_OK          =  0,
    VX_ERR_OOM     = -1,
    VX_ERR_MEMLIM  = -2, /* max_memory_bytes exceeded */
    VX_ERR_IO      = -3,
    VX_ERR_CORRUPT = -4,
    VX_ERR_SCHEMA  = -5,
    VX_ERR_UNIQUE  = -6,
    VX_ERR_CANCEL  = -7,
    VX_ERR_INVALID = -8,
    VX_ERR_CONFLICT = -9
} vx_status_t;

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_type : uint8_t {
    VX_TYPE_BOOL = 0,
    VX_TYPE_I64  = 1,
    VX_TYPE_U64  = 2,
    VX_TYPE_F64  = 3,
    VX_TYPE_STR  = 4
} vx_type_t;

#if defined(__GNUC__) && !defined(__clang__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L)
__extension__
#endif
typedef enum vx_encoding : uint8_t {
    VX_ENC_PLAIN    = 0,
    VX_ENC_DICT     = 1, /* M15: low-cardinality strings */
    VX_ENC_FOR_BP   = 2, /* M15: Frame-of-Reference + Bitpacking */
    VX_ENC_RLE      = 3  /* M15: Run-Length Encoding */
} vx_encoding_t;

typedef struct vx_zone {
    union {
        int64_t  i64;
        uint64_t u64;
        double   f64;
        uint8_t  str4[4]; /* 4-byte lexicographic prefix bound */
    } min_val, max_val;
    uint32_t null_count;
    uint8_t  has_bounds; /* 0 if all rows in chunk/segment are null */
    uint8_t  _pad[3];
} vx_zone_t;

