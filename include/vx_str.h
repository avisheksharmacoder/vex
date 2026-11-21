#ifndef VX_STR_H
#define VX_STR_H

#include "vx_arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_STR_INLINE_MAX 12U

/* Umbra-style 16-byte "German String":
 *   len <= 12 -> bytes stored inline across prefix[4] + tail[8] (zero heap touch)
 *   len >  12 -> prefix[4] caches first 4 bytes; ptr points to full len bytes in arena
 */
typedef struct {
    uint32_t len;
    char     prefix[4];
    union {
        char        tail[8];
        const char *ptr;
    } u;
} vx_str_t;

_Static_assert(sizeof(vx_str_t) == 16, "vx_str_t must be exactly 16 bytes");

static inline bool vx_str_is_inline(const vx_str_t *s) {
    return s && s->len <= VX_STR_INLINE_MAX;
}

/* Pointer to raw bytes (binary-safe, not guaranteed NUL-terminated) */
static inline const char *vx_str_data(const vx_str_t *s) {
    if (!s || s->len == 0) {
        return "";
    }
    return s->len <= VX_STR_INLINE_MAX ? s->prefix : s->u.ptr;
}

/* arena may be NULL when len <= VX_STR_INLINE_MAX */
bool     vx_str_make(vx_arena_t *arena, const char *data, uint32_t len, vx_str_t *out);
bool     vx_str_from_cstr(vx_arena_t *arena, const char *cstr, vx_str_t *out);
bool     vx_str_eq(const vx_str_t *a, const vx_str_t *b);
int      vx_str_cmp(const vx_str_t *a, const vx_str_t *b);
uint64_t vx_str_hash(const vx_str_t *s);

#endif /* VX_STR_H */

