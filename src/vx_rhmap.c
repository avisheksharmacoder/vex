#include "vx_rhmap.h"

#include <stdlib.h>
#include <string.h>

static inline bool is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

static inline uint64_t rh_hash(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static bool rh_alloc_slots(size_t cap, vx_rh_slot_t **out) {
    void *mem = NULL;
    size_t bytes = cap * sizeof(vx_rh_slot_t);
    if (posix_memalign(&mem, 64U, bytes) != 0 || !mem) {
        return false;
    }
    memset(mem, 0, bytes);
    *out = (vx_rh_slot_t *)mem;
    return true;
}

bool vx_rhmap_init(vx_rhmap_t *m, size_t initial_cap_pow2) {
    if (!m || initial_cap_pow2 < 2 || !is_pow2(initial_cap_pow2)) {
        return false;
    }
    vx_rh_slot_t *slots = NULL;
    if (!rh_alloc_slots(initial_cap_pow2, &slots)) {
        return false;
    }
    m->slots = slots;
    m->cap = initial_cap_pow2;
    m->mask = initial_cap_pow2 - 1;
    m->len = 0;
    return true;
}

void vx_rhmap_destroy(vx_rhmap_t *m) {
    if (!m) {
        return;
    }
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->mask = 0;
    m->len = 0;
}

vx_rhmap_t *vx_rhmap_clone(const vx_rhmap_t *src) {
    if (!src || !src->slots || src->cap == 0) {
        return NULL;
    }
    vx_rhmap_t *dst = (vx_rhmap_t *)calloc(1, sizeof(vx_rhmap_t));
    if (!dst) {
        return NULL;
    }
    if (!rh_alloc_slots(src->cap, &dst->slots)) {
        free(dst);
        return NULL;
    }
    memcpy(dst->slots, src->slots, src->cap * sizeof(vx_rh_slot_t));
    dst->cap = src->cap;
    dst->mask = src->mask;
    dst->len = src->len;
    return dst;
}

static void rh_insert_raw(vx_rhmap_t *m, uint64_t key, uint64_t val) {
    vx_rh_slot_t cur = { .key = key, .val = val, .psl = 1U, ._pad = 0U };
    size_t idx = (size_t)rh_hash(key) & m->mask;

    for (;;) {
        vx_rh_slot_t *slot = &m->slots[idx];
        if (slot->psl == 0U) {
            *slot = cur;
            m->len++;
            return;
        }
        if (slot->key == cur.key) {
            slot->val = cur.val;
            return;
        }
        if (slot->psl < cur.psl) {
            vx_rh_slot_t tmp = *slot;
            *slot = cur;
            cur = tmp;
        }
        idx = (idx + 1U) & m->mask;
        cur.psl++;
    }
}

static bool rh_grow(vx_rhmap_t *m) {
    size_t new_cap = m->cap * 2U;
    if (new_cap < m->cap) {
        return false;
    }
    vx_rh_slot_t *new_slots = NULL;
    if (!rh_alloc_slots(new_cap, &new_slots)) {
        return false;
    }

    vx_rh_slot_t *old_slots = m->slots;
    size_t old_cap = m->cap;

    m->slots = new_slots;
    m->cap = new_cap;
    m->mask = new_cap - 1U;
    m->len = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_slots[i].psl != 0U) {
            rh_insert_raw(m, old_slots[i].key, old_slots[i].val);
        }
    }
    free(old_slots);
    return true;
}

bool vx_rhmap_put(vx_rhmap_t *m, uint64_t key, uint64_t val) {
    if (!m || !m->slots) {
        return false;
    }
    if ((m->len + 1U) * 4U > m->cap * 3U) {
        if (!rh_grow(m)) {
            return false;
        }
    }
    rh_insert_raw(m, key, val);
    return true;
}

bool vx_rhmap_get(const vx_rhmap_t *m, uint64_t key, uint64_t *out_val) {
    if (!m || !m->slots || m->len == 0) {
        return false;
    }
    size_t idx = (size_t)rh_hash(key) & m->mask;
    uint32_t psl = 1U;

    for (;;) {
        const vx_rh_slot_t *slot = &m->slots[idx];
        if (slot->psl < psl) {
            return false;
        }
        if (slot->key == key) {
            if (out_val) {
                *out_val = slot->val;
            }
            return true;
        }
        idx = (idx + 1U) & m->mask;
        psl++;
    }
}

bool vx_rhmap_del(vx_rhmap_t *m, uint64_t key, uint64_t *out_old_val) {
    if (!m || !m->slots || m->len == 0) {
        return false;
    }
    size_t idx = (size_t)rh_hash(key) & m->mask;
    uint32_t psl = 1U;

    for (;;) {
        vx_rh_slot_t *slot = &m->slots[idx];
        if (slot->psl < psl) {
            return false;
        }
        if (slot->key == key) {
            if (out_old_val) {
                *out_old_val = slot->val;
            }
            /* Backward-shift deletion preserves Robin Hood invariant with zero tombstones */
            size_t next = (idx + 1U) & m->mask;
            while (m->slots[next].psl > 1U) {
                m->slots[idx] = m->slots[next];
                m->slots[idx].psl--;
                idx = next;
                next = (next + 1U) & m->mask;
            }
            m->slots[idx].key = 0;
            m->slots[idx].val = 0;
            m->slots[idx].psl = 0;
            m->len--;
            return true;
        }
        idx = (idx + 1U) & m->mask;
        psl++;
    }
}

size_t vx_rhmap_len(const vx_rhmap_t *m) {
    return m ? m->len : 0;
}

size_t vx_rhmap_cap(const vx_rhmap_t *m) {
    return m ? m->cap : 0;
}

uint32_t vx_rhmap_max_psl(const vx_rhmap_t *m) {
    if (!m || !m->slots) {
        return 0;
    }
    uint32_t best = 0;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].psl > best) {
            best = m->slots[i].psl;
        }
    }
    return best;
}

