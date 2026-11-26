#pragma once
#include "vx_owner.h"

#define VX_STR_INLINE_MAX 12

typedef union vx_strview {
    struct {
        int32_t len;                    /* 0..12 */
        uint8_t data[VX_STR_INLINE_MAX];/* trailing unused bytes MUST be zeroed */
    } inl;
    struct {
        int32_t len;                    /* > 12, capped at INT32_MAX */
        uint8_t prefix[4];              /* first 4 bytes of UTF-8 payload */
        int32_t buf_idx;                /* index into vx_strpool_t.bufs[] */
        int32_t offset;                 /* byte offset within bufs[buf_idx] */
    } out;
    struct {
        uint64_t lo;
        uint64_t hi;
    } raw;                              /* enables 128-bit equality on inline strings */
} vx_strview_t;

_Static_assert(sizeof(vx_strview_t) == 16, "vx_strview_t must match Arrow 16B StringView");

/* Immutable refcounted byte block (Hot slab or M13 mmap slice) */
typedef struct vx_strbuf {
    vx_owner_t     hdr;
    const uint8_t *data;
    uint32_t       len;
    uint32_t       cap;
    vx_owner_t    *backing; /* NULL if self-owned heap buffer; &seg->hdr if mmap'd */
} vx_strbuf_t;

/* Variadic buffer table: maps 1:1 to Arrow StringView buffers[2..2+nbufs-1] */
typedef struct vx_strpool {
    const uint8_t **ptrs;   /* ptrs[buf_idx] -> base pointer */
    int64_t        *lens;   /* Arrow expects int64_t buffer lengths in metadata */
    vx_owner_t    **owners; /* owners[buf_idx] pinned for lifetime of pool */
    uint32_t        nbufs;
    uint32_t        cap;
} vx_strpool_t;

/* Construction & resolution */
static inline bool vx_strview_is_inline(vx_strview_t v) {
    return v.inl.len <= VX_STR_INLINE_MAX;
}

static inline const uint8_t *vx_strview_ptr(const vx_strview_t *v, const vx_strpool_t *pool) {
    return vx_strview_is_inline(*v)
        ? v->inl.data
        : pool->ptrs[v->out.buf_idx] + v->out.offset;
}

vx_strview_t vx_strview_inline(const uint8_t *bytes, int32_t len);
vx_status_t  vx_strpool_init(vx_strpool_t *p, uint32_t init_cap);
void         vx_strpool_destroy(vx_strpool_t *p);
vx_status_t  vx_strpool_append(vx_strpool_t *p, const uint8_t *bytes, int32_t len, vx_strview_t *out);
vx_status_t  vx_strpool_attach_external(vx_strpool_t *p, const uint8_t *base, int64_t len,
                                        vx_owner_t *owner, int32_t *out_buf_idx);

/* Comparisons (fast-path 4B prefix + inline raw.lo/raw.hi check before touching pool) */
bool vx_strview_eq(vx_strview_t a, const vx_strpool_t *pa, vx_strview_t b, const vx_strpool_t *pb);
int  vx_strview_cmp(vx_strview_t a, const vx_strpool_t *pa, vx_strview_t b, const vx_strpool_t *pb);

