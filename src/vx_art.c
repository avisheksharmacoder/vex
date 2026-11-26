#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vx_idx.h"
#include "vx_failpoint.h"

extern uint32_t vx_crc32_update(uint32_t crc, const void *buf, size_t len);

#define VX_ART_MAX_DEPTH_GUARD 4096u

static inline uint64_t vx_art_align8(uint64_t v) {
    return (v + 7ULL) & ~7ULL;
}

static inline bool vx_art_range_ok(uint64_t off, uint64_t len, size_t total) {
    return off <= (uint64_t)total && len <= ((uint64_t)total - off);
}

static inline bool vx_art_is_deleted(const vx_delmap_t *const *chunk_delmaps, uint32_t r) {
    if (!chunk_delmaps) return false;
    const vx_delmap_t *dm = chunk_delmaps[r >> 12];
    if (!dm) return false;
    uint32_t lr = r & (VX_BATCH_CAP - 1u);
    return ((dm->bits[lr >> 6] >> (lr & 63u)) & 1ULL) != 0;
}

static int vx_art_cmp_bytes(const uint8_t *a, uint32_t la, const uint8_t *b, uint32_t lb) {
    uint32_t m = (la < lb) ? la : lb;
    if (m > 0) {
        int c = memcmp(a, b, m);
        if (c != 0) return c;
    }
    return (la < lb) ? -1 : (la > lb ? 1 : 0);
}

static int vx_art_fdatasync(int fd) {
#if defined(__linux__)
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

static vx_status_t vx_art_write_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return VX_ERR_IO;
        }
        off += (size_t)n;
    }
    return VX_OK;
}

/* ============================================================================
 * 1. Bottom-Up Immutable ART Serializer (vx_art_build_file)
 * ============================================================================ */
typedef struct vx_art_group {
    const uint8_t *key;
    uint32_t       key_len;
    uint32_t       start; /* slice into sorted vx_art_entry_t[] */
    uint32_t       count; /* >= 1 */
} vx_art_group_t;

typedef struct vx_abuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} vx_abuf_t;

static vx_status_t vx_abuf_reserve(vx_abuf_t *b, size_t add) {
    if (add > SIZE_MAX - b->len) return VX_ERR_OOM;
    size_t need = b->len + add;
    if (need <= b->cap) return VX_OK;
    size_t ncap = b->cap ? b->cap * 2u : (64u * 1024u);
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2u) { ncap = need; break; }
        ncap *= 2u;
    }
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) return VX_ERR_OOM;
    b->data = nd;
    b->cap  = ncap;
    return VX_OK;
}

/* Appends 8-byte-aligned zero-initialized block and returns its file offset. */
static vx_status_t vx_abuf_alloc8(vx_abuf_t *b, size_t raw_len, uint64_t *out_off) {
    size_t aligned = (size_t)vx_art_align8((uint64_t)raw_len);
    vx_status_t st = vx_abuf_reserve(b, aligned);
    if (st != VX_OK) return st;
    *out_off = (uint64_t)b->len;
    memset(b->data + b->len, 0, aligned);
    b->len += aligned;
    return VX_OK;
}

static int vx_art_entry_cmp(const void *pa, const void *pb) {
    const vx_art_entry_t *a = (const vx_art_entry_t *)pa;
    const vx_art_entry_t *b = (const vx_art_entry_t *)pb;
    int c = vx_art_cmp_bytes(a->key, a->key_len, b->key, b->key_len);
    if (c != 0) return c;
    return (a->row_idx < b->row_idx) ? -1 : (a->row_idx > b->row_idx ? 1 : 0);
}

static vx_status_t vx_art_emit_leaf_rec(vx_abuf_t *buf, const vx_art_entry_t *sorted,
                                        const vx_art_group_t *grp, uint64_t *out_ref) {
    uint64_t row_or_post = 0;
    if (grp->count == 1u) {
        row_or_post = (uint64_t)sorted[grp->start].row_idx;
    } else {
        uint64_t post_off = 0;
        size_t   pbytes   = (size_t)grp->count * sizeof(uint32_t);
        vx_status_t st    = vx_abuf_alloc8(buf, pbytes, &post_off);
        if (st != VX_OK) return st;
        uint32_t *dst = (uint32_t *)(buf->data + post_off);
        for (uint32_t i = 0; i < grp->count; ++i) {
            dst[i] = sorted[grp->start + i].row_idx;
        }
        row_or_post = post_off;
    }

    uint64_t    leaf_off = 0;
    vx_status_t st = vx_abuf_alloc8(buf, sizeof(vx_art_leaf_t) + grp->key_len, &leaf_off);
    if (st != VX_OK) return st;

    vx_art_leaf_t leaf = {
        .key_len         = grp->key_len,
        .nrows           = grp->count,
        .row_or_post_off = row_or_post
    };
    memcpy(buf->data + leaf_off, &leaf, sizeof(leaf));
    if (grp->key_len > 0) {
        memcpy(buf->data + leaf_off + sizeof(leaf), grp->key, grp->key_len);
    }
    *out_ref = leaf_off | VX_ART_LEAF_TAG;
    return VX_OK;
}

static vx_status_t vx_art_build_subtree(vx_abuf_t *buf, const vx_art_entry_t *sorted,
                                        const vx_art_group_t *groups, uint32_t lo, uint32_t hi,
                                        uint32_t depth, uint32_t cur_level, uint32_t *io_max_depth,
                                        uint64_t *out_ref) {
    if (cur_level > *io_max_depth) *io_max_depth = cur_level;
    if (cur_level > VX_ART_MAX_DEPTH_GUARD) return VX_ERR_INVALID;

    if (hi - lo == 1u) {
        return vx_art_emit_leaf_rec(buf, sorted, &groups[lo], out_ref);
    }

    /* Sorted range invariant: LCP of [lo, hi) equals LCP(groups[lo], groups[hi - 1]) */
    const vx_art_group_t *first = &groups[lo];
    const vx_art_group_t *last  = &groups[hi - 1u];
    uint32_t min_len = (first->key_len < last->key_len) ? first->key_len : last->key_len;
    uint32_t lcp     = 0;
    while (depth + lcp < min_len && first->key[depth + lcp] == last->key[depth + lcp]) {
        lcp++;
    }

    const uint32_t split_depth = depth + lcp;
    uint8_t        c_keys[256];
    uint64_t       c_refs[256];
    uint32_t       n_children = 0;

    uint32_t pos = lo;
    while (pos < hi) {
        uint8_t  b     = (split_depth < groups[pos].key_len) ? groups[pos].key[split_depth] : 0u;
        uint32_t start = pos++;
        while (pos < hi) {
            uint8_t nb = (split_depth < groups[pos].key_len) ? groups[pos].key[split_depth] : 0u;
            if (nb != b) break;
            pos++;
        }

        uint64_t    cref = VX_ART_EMPTY_REF;
        vx_status_t st   = vx_art_build_subtree(buf, sorted, groups, start, pos,
                                                split_depth + 1u, cur_level + 1u,
                                                io_max_depth, &cref);
        if (st != VX_OK) return st;

        c_keys[n_children] = b;
        c_refs[n_children] = cref;
        n_children++;
    }

    vx_art_node_hdr_t nh;
    memset(&nh, 0, sizeof(nh));
    nh.prefix_len = (lcp > UINT16_MAX) ? UINT16_MAX : (uint16_t)lcp;
    uint32_t copy_p = (lcp < VX_ART_MAX_PREFIX) ? lcp : VX_ART_MAX_PREFIX;
    if (copy_p > 0) memcpy(nh.prefix, first->key + depth, copy_p);

    uint64_t    node_off = 0;
    vx_status_t st       = VX_OK;

    if (n_children <= 4u) {
        nh.kind  = VX_ART_NODE4;
        nh.count = (uint8_t)n_children;
        st = vx_abuf_alloc8(buf, sizeof(vx_art_node4_t), &node_off);
        if (st != VX_OK) return st;
        vx_art_node4_t *n4 = (vx_art_node4_t *)(buf->data + node_off);
        n4->hdr = nh;
        for (uint32_t i = 0; i < n_children; ++i) {
            n4->keys[i]     = c_keys[i];
            n4->children[i] = c_refs[i];
        }
    } else if (n_children <= 16u) {
        nh.kind  = VX_ART_NODE16;
        nh.count = (uint8_t)n_children;
        st = vx_abuf_alloc8(buf, sizeof(vx_art_node16_t), &node_off);
        if (st != VX_OK) return st;
        vx_art_node16_t *n16 = (vx_art_node16_t *)(buf->data + node_off);
        n16->hdr = nh;
        for (uint32_t i = 0; i < n_children; ++i) {
            n16->keys[i]     = c_keys[i];
            n16->children[i] = c_refs[i];
        }
    } else if (n_children <= 48u) {
        nh.kind  = VX_ART_NODE48;
        nh.count = (uint8_t)n_children;
        st = vx_abuf_alloc8(buf, sizeof(vx_art_node48_t), &node_off);
        if (st != VX_OK) return st;
        vx_art_node48_t *n48 = (vx_art_node48_t *)(buf->data + node_off);
        n48->hdr = nh;
        memset(n48->child_idx, VX_ART_N48_EMPTY, sizeof(n48->child_idx));
        for (uint32_t i = 0; i < n_children; ++i) {
            n48->child_idx[c_keys[i]] = (uint8_t)i;
            n48->children[i]          = c_refs[i];
        }
    } else {
        nh.kind  = VX_ART_NODE256;
        nh.count = (uint8_t)(n_children & 0xFFu); /* 256 wraps to 0 */
        st = vx_abuf_alloc8(buf, sizeof(vx_art_node256_t), &node_off);
        if (st != VX_OK) return st;
        vx_art_node256_t *n256 = (vx_art_node256_t *)(buf->data + node_off);
        n256->hdr = nh;
        for (uint32_t i = 0; i < n_children; ++i) {
            n256->children[c_keys[i]] = c_refs[i];
        }
    }

    *out_ref = node_off; /* bit 63 == 0 => internal node */
    return VX_OK;
}

/* Serializes an immutable ART .idx file atomically (<final_path>.tmp -> fdatasync -> rename). */
vx_status_t vx_art_build_file(const char *final_path, uint32_t table_id, uint32_t index_id,
                              uint64_t segment_id, uint16_t ncols, bool is_unique,
                              vx_art_entry_t *entries, uint32_t n_entries) {
    if (!final_path) return VX_ERR_INVALID;

    vx_art_group_t *groups  = NULL;
    uint32_t        ngroups = 0;
    vx_abuf_t       buf;
    memset(&buf, 0, sizeof(buf));

    /* Reserve 64-byte header at offset 0 */
    uint64_t    hdr_off = 0;
    vx_status_t st      = vx_abuf_alloc8(&buf, sizeof(vx_idx_hdr_t), &hdr_off);
    if (st != VX_OK) return st;

    uint64_t root_ref  = VX_ART_EMPTY_REF;
    uint32_t max_depth = 0;

    if (n_entries > 0) {
        qsort(entries, n_entries, sizeof(*entries), vx_art_entry_cmp);

        groups = (vx_art_group_t *)malloc((size_t)n_entries * sizeof(*groups));
        if (!groups) { st = VX_ERR_OOM; goto cleanup; }

        uint32_t i = 0;
        while (i < n_entries) {
            uint32_t start = i++;
            while (i < n_entries &&
                   vx_art_cmp_bytes(entries[start].key, entries[start].key_len,
                                    entries[i].key, entries[i].key_len) == 0) {
                i++;
            }
            uint32_t cnt = i - start;
            if (is_unique && cnt > 1u) {
                st = VX_ERR_UNIQUE;
                goto cleanup;
            }
            groups[ngroups++] = (vx_art_group_t){
                .key     = entries[start].key,
                .key_len = entries[start].key_len,
                .start   = start,
                .count   = cnt
            };
        }

        st = vx_art_build_subtree(&buf, entries, groups, 0u, ngroups, 0u, 1u,
                                  &max_depth, &root_ref);
        if (st != VX_OK) goto cleanup;
    }

    vx_idx_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_IDX_MAGIC, 8u);
    hdr.version       = VX_IDX_VERSION;
    hdr.kind          = VX_IDX_KIND_ART;
    hdr.is_unique     = is_unique ? 1u : 0u;
    hdr.ncols         = ncols;
    hdr.table_id      = table_id;
    hdr.index_id      = index_id;
    hdr.segment_id    = segment_id;
    hdr.nrows_indexed = n_entries;
    hdr.root_off      = root_ref;
    hdr.file_bytes    = (uint64_t)buf.len;
    hdr.aux_param     = max_depth;

    memcpy(buf.data, &hdr, sizeof(hdr));
    uint32_t crc = vx_crc32_update(0u, buf.data, 56u);
    if (buf.len > sizeof(hdr)) {
        crc = vx_crc32_update(crc, buf.data + sizeof(hdr), buf.len - sizeof(hdr));
    }
    hdr.file_crc32 = crc;
    memcpy(buf.data + 56u, &hdr.file_crc32, sizeof(hdr.file_crc32));

    char tmp_path[PATH_MAX];
    int  tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) { st = VX_ERR_INVALID; goto cleanup; }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { st = VX_ERR_IO; goto cleanup; }

    st = vx_art_write_all(fd, buf.data, buf.len);
    if (st == VX_OK && vx_art_fdatasync(fd) != 0) st = VX_ERR_IO;
    if (close(fd) != 0 && st == VX_OK) st = VX_ERR_IO;

    if (st == VX_OK && rename(tmp_path, final_path) != 0) st = VX_ERR_IO;
    if (st == VX_OK) VX_FAILPOINT("idx_post_rename_pre_man");
    if (st != VX_OK) (void)unlink(tmp_path);

cleanup:
    free(groups);
    free(buf.data);
    return st;
}

/* ============================================================================
 * 2. Zero-Deserialization mmap Loader (vx_art_open)
 * ============================================================================ */
static void vx_idx_file_destroy(vx_owner_t *owner) {
    vx_idx_file_t *f = (vx_idx_file_t *)owner;
    if (!f) return;
    if (f->mmap_base && f->mmap_base != MAP_FAILED && f->mmap_len > 0) {
        (void)munmap((void *)(uintptr_t)f->mmap_base, f->mmap_len);
    }
    if (f->fd >= 0) close(f->fd);
    if (f->file_path) {
        if (atomic_load_explicit(&f->unlink_on_destroy, memory_order_acquire)) {
            (void)unlink(f->file_path);
        }
        free(f->file_path);
    }
    free(f);
}

vx_status_t vx_art_open(const char *path, vx_idx_file_t **out_idx) {
    if (!path || !out_idx) return VX_ERR_INVALID;
    *out_idx = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return VX_ERR_IO;

    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size < (off_t)sizeof(vx_idx_hdr_t)) {
        close(fd);
        return VX_ERR_CORRUPT;
    }

    const size_t fsize = (size_t)sb.st_size;
    void        *map   = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return VX_ERR_IO; }

    const vx_idx_hdr_t *hdr = (const vx_idx_hdr_t *)map;
    if (memcmp(hdr->magic, VX_IDX_MAGIC, 8u) != 0 ||
        hdr->version != VX_IDX_VERSION ||
        hdr->kind != VX_IDX_KIND_ART ||
        hdr->file_bytes != (uint64_t)fsize ||
        hdr->aux_param > VX_ART_MAX_DEPTH_GUARD) {
        munmap(map, fsize);
        close(fd);
        return VX_ERR_CORRUPT;
    }

    uint32_t crc = vx_crc32_update(0u, map, 56u);
    if (fsize > sizeof(vx_idx_hdr_t)) {
        crc = vx_crc32_update(crc, (const uint8_t *)map + sizeof(vx_idx_hdr_t),
                              fsize - sizeof(vx_idx_hdr_t));
    }
    if (crc != hdr->file_crc32) {
        munmap(map, fsize);
        close(fd);
        return VX_ERR_CORRUPT;
    }

    vx_idx_file_t *idx = (vx_idx_file_t *)calloc(1u, sizeof(*idx));
    if (!idx) { munmap(map, fsize); close(fd); return VX_ERR_OOM; }

    vx_owner_init(&idx->hdr, VX_OWN_INDEX, vx_idx_file_destroy);
    atomic_init(&idx->unlink_on_destroy, false);
    idx->fd        = fd;
    idx->mmap_base = (const uint8_t *)map;
    idx->mmap_len  = fsize;
    idx->file_hdr  = hdr;

    size_t plen = strlen(path);
    idx->file_path = (char *)malloc(plen + 1u);
    if (!idx->file_path) { vx_owner_release(&idx->hdr); return VX_ERR_OOM; }
    memcpy(idx->file_path, path, plen + 1u);

    *out_idx = idx;
    return VX_OK;
}

/* ============================================================================
 * 3. Fuzz-Safe Node Helpers, Point Lookup & Ordered Range Scan
 * ============================================================================ */
static const vx_art_leaf_t *vx_art_get_leaf(const vx_idx_file_t *idx, uint64_t ref,
                                            const uint8_t **out_key) {
    uint64_t off = ref & VX_ART_REF_MASK;
    if (!vx_art_range_ok(off, sizeof(vx_art_leaf_t), idx->mmap_len)) return NULL;
    const vx_art_leaf_t *leaf = (const vx_art_leaf_t *)(idx->mmap_base + off);
    if (!vx_art_range_ok(off + sizeof(vx_art_leaf_t), leaf->key_len, idx->mmap_len)) return NULL;
    if (out_key) *out_key = idx->mmap_base + off + sizeof(vx_art_leaf_t);
    return leaf;
}

static const vx_art_node_hdr_t *vx_art_get_node(const vx_idx_file_t *idx, uint64_t ref) {
    if (!vx_art_range_ok(ref, sizeof(vx_art_node_hdr_t), idx->mmap_len)) return NULL;
    const vx_art_node_hdr_t *nh = (const vx_art_node_hdr_t *)(idx->mmap_base + ref);
    size_t need = 0;
    switch (nh->kind) {
    case VX_ART_NODE4:   need = sizeof(vx_art_node4_t);   break;
    case VX_ART_NODE16:  need = sizeof(vx_art_node16_t);  break;
    case VX_ART_NODE48:  need = sizeof(vx_art_node48_t);  break;
    case VX_ART_NODE256: need = sizeof(vx_art_node256_t); break;
    default: return NULL;
    }
    return vx_art_range_ok(ref, need, idx->mmap_len) ? nh : NULL;
}

static uint64_t vx_art_find_child(const vx_art_node_hdr_t *nh, uint8_t b) {
    switch (nh->kind) {
    case VX_ART_NODE4: {
        const vx_art_node4_t *n4 = (const vx_art_node4_t *)nh;
        uint32_t cnt = (n4->hdr.count <= 4u) ? n4->hdr.count : 4u;
        for (uint32_t i = 0; i < cnt; ++i) {
            if (n4->keys[i] == b) return n4->children[i];
        }
        return VX_ART_EMPTY_REF;
    }
    case VX_ART_NODE16: {
        const vx_art_node16_t *n16 = (const vx_art_node16_t *)nh;
        uint32_t lo = 0, hi = (n16->hdr.count <= 16u) ? n16->hdr.count : 16u;
        while (lo < hi) {
            uint32_t mid = (lo + hi) >> 1;
            if (n16->keys[mid] == b) return n16->children[mid];
            if (n16->keys[mid] < b)  lo = mid + 1u;
            else                     hi = mid;
        }
        return VX_ART_EMPTY_REF;
    }
    case VX_ART_NODE48: {
        const vx_art_node48_t *n48 = (const vx_art_node48_t *)nh;
        uint8_t ci = n48->child_idx[b];
        return (ci < 48u) ? n48->children[ci] : VX_ART_EMPTY_REF;
    }
    case VX_ART_NODE256:
        return ((const vx_art_node256_t *)nh)->children[b];
    }
    return VX_ART_EMPTY_REF;
}

/* Descends to leftmost leaf to resolve full prefix when prefix_len > VX_ART_MAX_PREFIX (12). */
static const vx_art_leaf_t *vx_art_min_leaf(const vx_idx_file_t *idx, uint64_t ref,
                                            const uint8_t **out_key) {
    uint32_t steps = 0;
    while (ref != VX_ART_EMPTY_REF && steps++ <= idx->file_hdr->aux_param + 1u) {
        if (ref & VX_ART_LEAF_TAG) return vx_art_get_leaf(idx, ref, out_key);
        const vx_art_node_hdr_t *nh = vx_art_get_node(idx, ref);
        if (!nh) return NULL;
        uint64_t nxt = VX_ART_EMPTY_REF;
        if (nh->kind == VX_ART_NODE4) {
            nxt = ((const vx_art_node4_t *)nh)->children[0];
        } else if (nh->kind == VX_ART_NODE16) {
            nxt = ((const vx_art_node16_t *)nh)->children[0];
        } else if (nh->kind == VX_ART_NODE48) {
            const vx_art_node48_t *n48 = (const vx_art_node48_t *)nh;
            for (uint32_t b = 0; b < 256u; ++b) {
                if (n48->child_idx[b] < 48u) { nxt = n48->children[n48->child_idx[b]]; break; }
            }
        } else {
            const vx_art_node256_t *n256 = (const vx_art_node256_t *)nh;
            for (uint32_t b = 0; b < 256u; ++b) {
                if (n256->children[b] != VX_ART_EMPTY_REF) { nxt = n256->children[b]; break; }
            }
        }
        ref = nxt;
    }
    return NULL;
}

/* Emits all live (non-deleted) row_idx values stored in a leaf or its posting list. */
static vx_status_t vx_art_emit_leaf(const vx_idx_file_t *idx,
                                    const vx_delmap_t *const *chunk_delmaps,
                                    const vx_art_leaf_t *leaf,
                                    vx_idx_hit_fn on_hit, void *ctx, bool *io_stop) {
    if (leaf->nrows == 0) return VX_OK;
    if (leaf->nrows == 1u) {
        uint32_t r = (uint32_t)leaf->row_or_post_off;
        if (!vx_art_is_deleted(chunk_delmaps, r)) {
            return on_hit(ctx, r, io_stop);
        }
        return VX_OK;
    }

    uint64_t pbytes = (uint64_t)leaf->nrows * sizeof(uint32_t);
    if (!vx_art_range_ok(leaf->row_or_post_off, pbytes, idx->mmap_len)) {
        return VX_ERR_CORRUPT;
    }
    const uint32_t *rows = (const uint32_t *)(idx->mmap_base + leaf->row_or_post_off);
    for (uint32_t i = 0; i < leaf->nrows && !(*io_stop); ++i) {
        uint32_t r = rows[i];
        if (!vx_art_is_deleted(chunk_delmaps, r)) {
            vx_status_t st = on_hit(ctx, r, io_stop);
            if (st != VX_OK) return st;
        }
    }
    return VX_OK;
}

/* --- Point Equality Lookup on ART --- */
vx_status_t vx_art_lookup_eq(const vx_idx_file_t *idx, const vx_delmap_t *const *chunk_delmaps,
                             const uint8_t *enc_key, uint32_t key_len,
                             vx_idx_hit_fn on_hit, void *ctx) {
    if (!idx || !on_hit) return VX_ERR_INVALID;
    uint64_t ref = idx->file_hdr->root_off;
    if (ref == VX_ART_EMPTY_REF) return VX_OK;

    uint32_t depth = 0;
    uint32_t steps = 0;

    while (ref != VX_ART_EMPTY_REF) {
        if (steps++ > idx->file_hdr->aux_param + 1u) return VX_ERR_CORRUPT;

        if (ref & VX_ART_LEAF_TAG) {
            const uint8_t       *lkey = NULL;
            const vx_art_leaf_t *leaf = vx_art_get_leaf(idx, ref, &lkey);
            if (!leaf) return VX_ERR_CORRUPT;
            if (leaf->key_len == key_len &&
                (key_len == 0 || memcmp(lkey, enc_key, key_len) == 0)) {
                bool stop = false;
                return vx_art_emit_leaf(idx, chunk_delmaps, leaf, on_hit, ctx, &stop);
            }
            return VX_OK;
        }

        const vx_art_node_hdr_t *nh = vx_art_get_node(idx, ref);
        if (!nh) return VX_ERR_CORRUPT;

        uint32_t plen = nh->prefix_len;
        if (depth + plen > key_len) return VX_OK;
        uint32_t chk = (plen < VX_ART_MAX_PREFIX) ? plen : VX_ART_MAX_PREFIX;
        if (chk > 0 && memcmp(nh->prefix, enc_key + depth, chk) != 0) return VX_OK;

        depth += plen;
        uint8_t b = (depth < key_len) ? enc_key[depth] : 0u;
        ref       = vx_art_find_child(nh, b);
        depth++;
    }
    return VX_OK;
}

/* --- Ordered Range Scan on ART --- */
static vx_status_t vx_art_range_dfs(const vx_idx_file_t *idx,
                                    const vx_delmap_t *const *chunk_delmaps,
                                    uint64_t ref, uint32_t depth, uint32_t level,
                                    vx_art_bound_t lo, vx_art_bound_t hi,
                                    bool gt_lo, bool lt_hi,
                                    vx_idx_hit_fn on_hit, void *ctx, bool *io_stop) {
    if (*io_stop || ref == VX_ART_EMPTY_REF) return VX_OK;
    if (level > idx->file_hdr->aux_param + 1u) return VX_ERR_CORRUPT;

    if (ref & VX_ART_LEAF_TAG) {
        const uint8_t       *lkey = NULL;
        const vx_art_leaf_t *leaf = vx_art_get_leaf(idx, ref, &lkey);
        if (!leaf) return VX_ERR_CORRUPT;

        if (!gt_lo && !lo.unbounded) {
            int c = vx_art_cmp_bytes(lkey, leaf->key_len, lo.key, lo.len);
            if (c < 0 || (c == 0 && !lo.inclusive)) return VX_OK;
        }
        if (!lt_hi && !hi.unbounded) {
            int c = vx_art_cmp_bytes(lkey, leaf->key_len, hi.key, hi.len);
            if (c > 0 || (c == 0 && !hi.inclusive)) {
                *io_stop = true; /* in-order DFS: all following keys also exceed hi */
                return VX_OK;
            }
        }
        return vx_art_emit_leaf(idx, chunk_delmaps, leaf, on_hit, ctx, io_stop);
    }

    const vx_art_node_hdr_t *nh = vx_art_get_node(idx, ref);
    if (!nh) return VX_ERR_CORRUPT;

    /* Check compressed prefix against lo / hi bounds */
    const uint32_t plen = nh->prefix_len;
    if (plen > 0 && ((!gt_lo && !lo.unbounded) || (!lt_hi && !hi.unbounded))) {
        const uint8_t *pbytes = nh->prefix;
        if (plen > VX_ART_MAX_PREFIX) {
            const uint8_t       *mkey  = NULL;
            const vx_art_leaf_t *mleaf = vx_art_min_leaf(idx, ref, &mkey);
            if (!mleaf || depth + plen > mleaf->key_len) return VX_ERR_CORRUPT;
            pbytes = mkey + depth;
        }

        if (!gt_lo && !lo.unbounded) {
            uint32_t avail = (depth < lo.len) ? (lo.len - depth) : 0u;
            uint32_t cmp_n = (plen < avail) ? plen : avail;
            int      c     = cmp_n ? memcmp(pbytes, lo.key + depth, cmp_n) : 0;
            if (c < 0) return VX_OK;          /* entire subtree < lo */
            if (c > 0 || plen > avail) gt_lo = true; /* entire subtree > lo */
        }
        if (!lt_hi && !hi.unbounded) {
            uint32_t avail = (depth < hi.len) ? (hi.len - depth) : 0u;
            uint32_t cmp_n = (plen < avail) ? plen : avail;
            int      c     = cmp_n ? memcmp(pbytes, hi.key + depth, cmp_n) : 0;
            if (c > 0 || (c == 0 && plen > avail)) {
                *io_stop = true;              /* entire subtree > hi */
                return VX_OK;
            }
            if (c < 0) lt_hi = true;          /* entire subtree < hi */
        }
    }

    const uint32_t split_depth = depth + plen;
    uint32_t min_b = 0u;
    uint32_t max_b = 255u;

    if (!gt_lo && !lo.unbounded) {
        if (split_depth < lo.len) min_b = lo.key[split_depth];
        else                      gt_lo = true; /* subtree keys are longer => strictly > lo */
    }
    if (!lt_hi && !hi.unbounded) {
        if (split_depth < hi.len) max_b = hi.key[split_depth];
        else { *io_stop = true; return VX_OK; } /* hi ended earlier => all children > hi */
    }
    if (min_b > max_b) return VX_OK;

    /* Iterate active children in ascending byte order within [min_b, max_b] */
    switch (nh->kind) {
    case VX_ART_NODE4: {
        const vx_art_node4_t *n4  = (const vx_art_node4_t *)nh;
        uint32_t              cnt = (n4->hdr.count <= 4u) ? n4->hdr.count : 4u;
        for (uint32_t i = 0; i < cnt && !(*io_stop); ++i) {
            uint32_t b = n4->keys[i];
            if (b < min_b) continue;
            if (b > max_b) { *io_stop = !lt_hi; break; }
            vx_status_t st = vx_art_range_dfs(idx, chunk_delmaps, n4->children[i],
                                              split_depth + 1u, level + 1u, lo, hi,
                                              gt_lo || (b > min_b), lt_hi || (b < max_b),
                                              on_hit, ctx, io_stop);
            if (st != VX_OK) return st;
        }
        break;
    }
    case VX_ART_NODE16: {
        const vx_art_node16_t *n16 = (const vx_art_node16_t *)nh;
        uint32_t               cnt = (n16->hdr.count <= 16u) ? n16->hdr.count : 16u;
        for (uint32_t i = 0; i < cnt && !(*io_stop); ++i) {
            uint32_t b = n16->keys[i];
            if (b < min_b) continue;
            if (b > max_b) { *io_stop = !lt_hi; break; }
            vx_status_t st = vx_art_range_dfs(idx, chunk_delmaps, n16->children[i],
                                              split_depth + 1u, level + 1u, lo, hi,
                                              gt_lo || (b > min_b), lt_hi || (b < max_b),
                                              on_hit, ctx, io_stop);
            if (st != VX_OK) return st;
        }
        break;
    }
    case VX_ART_NODE48: {
        const vx_art_node48_t *n48 = (const vx_art_node48_t *)nh;
        for (uint32_t b = min_b; b <= max_b && !(*io_stop); ++b) {
            uint8_t ci = n48->child_idx[b];
            if (ci >= 48u) continue;
            vx_status_t st = vx_art_range_dfs(idx, chunk_delmaps, n48->children[ci],
                                              split_depth + 1u, level + 1u, lo, hi,
                                              gt_lo || (b > min_b), lt_hi || (b < max_b),
                                              on_hit, ctx, io_stop);
            if (st != VX_OK) return st;
        }
        break;
    }
    case VX_ART_NODE256: {
        const vx_art_node256_t *n256 = (const vx_art_node256_t *)nh;
        for (uint32_t b = min_b; b <= max_b && !(*io_stop); ++b) {
            if (n256->children[b] == VX_ART_EMPTY_REF) continue;
            vx_status_t st = vx_art_range_dfs(idx, chunk_delmaps, n256->children[b],
                                              split_depth + 1u, level + 1u, lo, hi,
                                              gt_lo || (b > min_b), lt_hi || (b < max_b),
                                              on_hit, ctx, io_stop);
            if (st != VX_OK) return st;
        }
        break;
    }
    }
    return VX_OK;
}

vx_status_t vx_art_scan_range(const vx_idx_file_t *idx, const vx_delmap_t *const *chunk_delmaps,
                              vx_art_bound_t lo, vx_art_bound_t hi,
                              vx_idx_hit_fn on_hit, void *ctx) {
    if (!idx || !on_hit) return VX_ERR_INVALID;
    if (idx->file_hdr->root_off == VX_ART_EMPTY_REF) return VX_OK;

    bool stop = false;
    return vx_art_range_dfs(idx, chunk_delmaps, idx->file_hdr->root_off, 0u, 1u,
                            lo, hi, lo.unbounded, hi.unbounded, on_hit, ctx, &stop);
}

