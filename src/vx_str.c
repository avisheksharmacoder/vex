#include "vx_str.h"

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(vx_str_t, u.tail) == offsetof(vx_str_t, prefix) + 4,
               "vx_str_t inline bytes must be contiguous across prefix and tail");

bool vx_str_make(vx_arena_t *arena, const char *data, uint32_t len, vx_str_t *out) {
    if (!out || (len > 0 && !data)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->len = len;

    if (len == 0) {
        return true;
    }

    if (len <= VX_STR_INLINE_MAX) {
        uint32_t pre = len < 4U ? len : 4U;
        memcpy(out->prefix, data, pre);
        if (len > 4U) {
            memcpy(out->u.tail, data + 4, len - 4U);
        }
        return true;
    }

    if (!arena) {
        return false;
    }

    char *dst = (char *)vx_arena_alloc(arena, len, 1);
    if (!dst) {
        return false;
    }
    memcpy(dst, data, len);
    memcpy(out->prefix, data, 4);
    out->u.ptr = dst;
    return true;
}

bool vx_str_from_cstr(vx_arena_t *arena, const char *cstr, vx_str_t *out) {
    if (!cstr || !out) {
        return false;
    }
    size_t n = strlen(cstr);
    if (n > UINT32_MAX) {
        return false;
    }
    return vx_str_make(arena, cstr, (uint32_t)n, out);
}

bool vx_str_eq(const vx_str_t *a, const vx_str_t *b) {
    if (a == b) {
        return true;
    }
    if (!a || !b || a->len != b->len) {
        return false;
    }
    if (a->len == 0) {
        return true;
    }

    uint32_t pre = a->len < 4U ? a->len : 4U;
    if (memcmp(a->prefix, b->prefix, pre) != 0) {
        return false;
    }
    if (a->len <= 4U) {
        return true;
    }

    if (a->len <= VX_STR_INLINE_MAX) {
        return memcmp(a->u.tail, b->u.tail, a->len - 4U) == 0;
    }
    return memcmp(a->u.ptr + 4, b->u.ptr + 4, a->len - 4U) == 0;
}

int vx_str_cmp(const vx_str_t *a, const vx_str_t *b) {
    if (a == b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }

    uint32_t min_len = a->len < b->len ? a->len : b->len;
    if (min_len > 0) {
        uint32_t pre = min_len < 4U ? min_len : 4U;
        int c = memcmp(a->prefix, b->prefix, pre);
        if (c != 0) {
            return c;
        }
        if (min_len > 4U) {
            const char *pa = a->len <= VX_STR_INLINE_MAX ? a->u.tail : (a->u.ptr + 4);
            const char *pb = b->len <= VX_STR_INLINE_MAX ? b->u.tail : (b->u.ptr + 4);
            c = memcmp(pa, pb, min_len - 4U);
            if (c != 0) {
                return c;
            }
        }
    }

    if (a->len < b->len) {
        return -1;
    }
    if (a->len > b->len) {
        return 1;
    }
    return 0;
}

uint64_t vx_str_hash(const vx_str_t *s) {
    if (!s) {
        return 0;
    }

    uint64_t h = 14695981039346656037ULL ^ (uint64_t)s->len;
    const uint64_t prime = 1099511628211ULL;

    if (s->len <= VX_STR_INLINE_MAX) {
        uint32_t pre = s->len < 4U ? s->len : 4U;
        for (uint32_t i = 0; i < pre; i++) {
            h ^= (uint8_t)s->prefix[i];
            h *= prime;
        }
        for (uint32_t i = 4U; i < s->len; i++) {
            h ^= (uint8_t)s->u.tail[i - 4U];
            h *= prime;
        }
    } else {
        const uint8_t *p = (const uint8_t *)s->u.ptr;
        for (uint32_t i = 0; i < s->len; i++) {
            h ^= p[i];
            h *= prime;
        }
    }

    /* Finalize avalanche */
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

