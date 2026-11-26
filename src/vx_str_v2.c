#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "vx_platform_win32.h"
#include "vx_str.h"

#define VX_STRBUF_INIT_CAP (64u * 1024u)        /* 64 KiB */
#define VX_STRBUF_MAX_CAP  (4u * 1024u * 1024u) /* 4 MiB  */

vx_strview_t vx_strview_inline(const uint8_t *bytes, int32_t len) {
    vx_strview_t v;
    memset(&v, 0, sizeof(v)); /* guarantees trailing unused bytes are 0 for 128-bit eq */
    if (len < 0) len = 0;
    if (len > VX_STR_INLINE_MAX) len = VX_STR_INLINE_MAX;
    v.inl.len = len;
    if (len > 0 && bytes) {
        memcpy(v.inl.data, bytes, (size_t)len);
    }
    return v;
}

static void vx_strbuf_destroy(vx_owner_t *owner) {
    vx_strbuf_t *sb = (vx_strbuf_t *)owner;
    if (!sb) return;
    if (sb->backing) {
        vx_owner_release(sb->backing);
    } else if (sb->data) {
        vx_aligned_free((void *)sb->data);
    }
    free(sb);
}

static vx_strbuf_t *vx_strbuf_create(uint32_t cap) {
    if (cap == 0u || cap > (uint32_t)INT32_MAX) return NULL;
    vx_strbuf_t *sb = (vx_strbuf_t *)calloc(1u, sizeof(*sb));
    if (!sb) return NULL;

    void *raw = NULL;
    if (vx_aligned_alloc_posix(&raw, VX_ALIGN, (size_t)cap) != 0 || !raw) {
        free(sb);
        return NULL;
    }
    vx_owner_init(&sb->hdr, VX_OWN_STRBUF, vx_strbuf_destroy);
    sb->data    = (const uint8_t *)raw;
    sb->len     = 0u;
    sb->cap     = cap;
    sb->backing = NULL;
    return sb;
}

static vx_status_t vx_strpool_grow(vx_strpool_t *p, uint32_t min_cap) {
    if (p->cap >= min_cap) return VX_OK;
    uint32_t ncap = p->cap ? p->cap : 4u;
    while (ncap < min_cap) {
        if (ncap > (uint32_t)INT32_MAX / 2u) { ncap = min_cap; break; }
        ncap *= 2u;
    }
    if (ncap > (uint32_t)INT32_MAX) return VX_ERR_OOM;

    const uint8_t **nptrs = (const uint8_t **)realloc((void *)p->ptrs, (size_t)ncap * sizeof(*nptrs));
    if (!nptrs) return VX_ERR_OOM;
    p->ptrs = nptrs;

    int64_t *nlens = (int64_t *)realloc(p->lens, (size_t)ncap * sizeof(*nlens));
    if (!nlens) return VX_ERR_OOM;
    p->lens = nlens;

    vx_owner_t **nown = (vx_owner_t **)realloc(p->owners, (size_t)ncap * sizeof(*nown));
    if (!nown) return VX_ERR_OOM;
    p->owners = nown;

    p->cap = ncap;
    return VX_OK;
}

vx_status_t vx_strpool_init(vx_strpool_t *p, uint32_t init_cap) {
    if (!p) return VX_ERR_INVALID;
    memset(p, 0, sizeof(*p));
    if (init_cap > 0u) {
        vx_status_t st = vx_strpool_grow(p, init_cap);
        if (st != VX_OK) {
            vx_strpool_destroy(p);
            return st;
        }
    }
    return VX_OK;
}

void vx_strpool_destroy(vx_strpool_t *p) {
    if (!p) return;
    if (p->owners) {
        for (uint32_t i = 0; i < p->nbufs; ++i) {
            vx_owner_release(p->owners[i]);
        }
    }
    free((void *)p->ptrs);
    free(p->lens);
    free(p->owners);
    memset(p, 0, sizeof(*p));
}

vx_status_t vx_strpool_attach_external(vx_strpool_t *p, const uint8_t *base, int64_t len,
                                       vx_owner_t *owner, int32_t *out_buf_idx) {
    if (!p || len < 0 || len > (int64_t)INT32_MAX || (len > 0 && !base)) {
        return VX_ERR_INVALID;
    }
    if (p->nbufs >= (uint32_t)INT32_MAX) return VX_ERR_OOM;

    vx_status_t st = vx_strpool_grow(p, p->nbufs + 1u);
    if (st != VX_OK) return st;

    uint32_t idx  = p->nbufs++;
    p->ptrs[idx]   = base;
    p->lens[idx]   = len;
    p->owners[idx] = vx_owner_retain(owner);

    if (out_buf_idx) *out_buf_idx = (int32_t)idx;
    return VX_OK;
}

vx_status_t vx_strpool_append(vx_strpool_t *p, const uint8_t *bytes, int32_t len,
                              vx_strview_t *out) {
    if (!p || !out || len < 0 || (len > 0 && !bytes)) return VX_ERR_INVALID;

    /* Inline fast path (<= 12B): zero buffer pool allocation */
    if (len <= VX_STR_INLINE_MAX) {
        *out = vx_strview_inline(bytes, len);
        return VX_OK;
    }

    const uint32_t ulen = (uint32_t)len;

    /* Reuse tail buffer ONLY if it is a private unshared vx_strbuf_t with enough space */
    if (p->nbufs > 0u) {
        uint32_t    last = p->nbufs - 1u;
        vx_owner_t *own  = p->owners ? p->owners[last] : NULL;
        if (own && own->kind == VX_OWN_STRBUF &&
            atomic_load_explicit(&own->refcnt, memory_order_acquire) == 1u) {
            vx_strbuf_t *sb = (vx_strbuf_t *)own;
            if (!sb->backing && ulen <= sb->cap - sb->len) {
                int32_t off = (int32_t)sb->len;
                memcpy((uint8_t *)sb->data + off, bytes, (size_t)ulen);
                sb->len      += ulen;
                p->lens[last] = (int64_t)sb->len;

                out->out.len     = len;
                memcpy(out->out.prefix, bytes, 4u);
                out->out.buf_idx = (int32_t)last;
                out->out.offset  = off;
                return VX_OK;
            }
        }
    }

    if (p->nbufs >= (uint32_t)INT32_MAX) return VX_ERR_OOM;

    /* Compute next slab capacity (geometric up to 4 MiB, or exact rounded size if > 4 MiB) */
    uint32_t next_cap = VX_STRBUF_INIT_CAP;
    if (p->nbufs > 0u) {
        int64_t prev_len = p->lens[p->nbufs - 1u];
        if (prev_len >= (int64_t)(VX_STRBUF_MAX_CAP / 2u)) {
            next_cap = VX_STRBUF_MAX_CAP;
        } else if (prev_len > (int64_t)VX_STRBUF_INIT_CAP) {
            next_cap = (uint32_t)prev_len * 2u;
        }
    }
    if (next_cap < ulen) {
        uint64_t rounded = ((uint64_t)ulen + 63ULL) & ~63ULL;
        next_cap = (rounded <= (uint64_t)INT32_MAX) ? (uint32_t)rounded : ulen;
    }

    vx_status_t st = vx_strpool_grow(p, p->nbufs + 1u);
    if (st != VX_OK) return st;

    vx_strbuf_t *sb = vx_strbuf_create(next_cap);
    if (!sb) return VX_ERR_OOM;

    memcpy((uint8_t *)sb->data, bytes, (size_t)ulen);
    sb->len = ulen;

    uint32_t idx   = p->nbufs++;
    p->ptrs[idx]   = sb->data;
    p->lens[idx]   = (int64_t)ulen;
    p->owners[idx] = &sb->hdr; /* steals initial refcnt == 1 */

    out->out.len     = len;
    memcpy(out->out.prefix, bytes, 4u);
    out->out.buf_idx = (int32_t)idx;
    out->out.offset  = 0;
    return VX_OK;
}

bool vx_strview_eq(vx_strview_t a, const vx_strpool_t *pa,
                   vx_strview_t b, const vx_strpool_t *pb) {
    /* raw.lo packs (len[0..3], first_4_bytes[4..7]) identically for inline & out-of-line */
    if (a.raw.lo != b.raw.lo) return false;
    if (vx_strview_is_inline(a)) return a.raw.hi == b.raw.hi;

    /* Out-of-line: len > 12 and bytes [0..3] already matched via raw.lo */
    const uint8_t *sa = pa->ptrs[a.out.buf_idx] + a.out.offset;
    const uint8_t *sb = pb->ptrs[b.out.buf_idx] + b.out.offset;
    return memcmp(sa + 4, sb + 4, (size_t)(a.out.len - 4)) == 0;
}

int vx_strview_cmp(vx_strview_t a, const vx_strpool_t *pa,
                   vx_strview_t b, const vx_strpool_t *pb) {
    const int32_t la      = a.inl.len;
    const int32_t lb      = b.inl.len;
    const int32_t min_len = (la < lb) ? la : lb;

    /* Fast path 1: compare up to first 4 bytes without dereferencing strpool */
    const int32_t pref_n = (min_len < 4) ? min_len : 4;
    if (pref_n > 0) {
        const uint8_t *p4a = vx_strview_is_inline(a) ? a.inl.data : a.out.prefix;
        const uint8_t *p4b = vx_strview_is_inline(b) ? b.inl.data : b.out.prefix;
        int            c   = memcmp(p4a, p4b, (size_t)pref_n);
        if (c != 0) return c;
    }

    if (min_len <= 4) {
        return (la < lb) ? -1 : (la > lb ? 1 : 0);
    }

    /* Fast path 2: both inline (5..12 bytes), still zero strpool dereferences */
    if (vx_strview_is_inline(a) && vx_strview_is_inline(b)) {
        int c = memcmp(a.inl.data + 4, b.inl.data + 4, (size_t)(min_len - 4));
        if (c != 0) return c;
        return (la < lb) ? -1 : (la > lb ? 1 : 0);
    }

    /* General path: resolve payload pointer(s) and compare bytes [4 .. min_len) */
    const uint8_t *sa = vx_strview_ptr(&a, pa);
    const uint8_t *sb = vx_strview_ptr(&b, pb);
    int            c  = memcmp(sa + 4, sb + 4, (size_t)(min_len - 4));
    if (c != 0) return c;
    return (la < lb) ? -1 : (la > lb ? 1 : 0);
}

