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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vx_idx.h"

static inline uint64_t vx_idx_align8(uint64_t v) {
    return (v + 7ULL) & ~7ULL;
}

static inline int vx_key_cmp(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen) {
    uint32_t min_len = alen < blen ? alen : blen;
    if (min_len > 0) {
        int c = memcmp(a, b, (size_t)min_len);
        if (c != 0) return c;
    }
    return (alen < blen) ? -1 : (alen > blen ? 1 : 0);
}

/* 16-byte Robin Hood slot for secondary hash index */
typedef struct vx_sec_hash_slot {
    uint64_t hash;
    uint32_t row_or_post_off;
    uint16_t nrows;           /* 0 = empty */
    uint16_t psl;
} vx_sec_hash_slot_t;
_Static_assert(sizeof(vx_sec_hash_slot_t) == 16, "vx_sec_hash_slot_t must be 16 bytes");

static uint64_t vx_sec_hash_key(const uint8_t *key, uint32_t len) {
    uint64_t h = 14695981039346656037ULL ^ (uint64_t)len;
    for (uint32_t i = 0; i < len; ++i) {
        h ^= (uint64_t)key[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    return h ^ (h >> 33);
}

/* ============================================================================
 * 1. Companion .idx File Lifecycle (mmap open, close, destroy)
 * ============================================================================ */
static void vx_idx_destroy_fn(vx_owner_t *o) {
    vx_idx_file_t *idx = (vx_idx_file_t *)o;
    if (!idx) return;

    if (idx->mmap_base && idx->mmap_len > 0) {
        munmap((void *)(uintptr_t)idx->mmap_base, idx->mmap_len);
    }
    if (idx->fd >= 0) {
        close(idx->fd);
    }
    if (atomic_load_explicit(&idx->unlink_on_destroy, memory_order_acquire)) {
        if (idx->file_path) (void)unlink(idx->file_path);
    }
    free(idx->file_path);
    free(idx);
}

vx_status_t vx_idx_open(const char *path, vx_idx_file_t **out_idx) {
    if (!path || !out_idx) return VX_ERR_INVALID;
    *out_idx = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return VX_ERR_IO;

    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size < (off_t)sizeof(vx_idx_hdr_t)) {
        close(fd);
        return VX_ERR_CORRUPT;
    }

    size_t fsize = (size_t)sb.st_size;
    void *base = mmap(NULL, fsize, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return VX_ERR_IO;
    }

    const vx_idx_hdr_t *hdr = (const vx_idx_hdr_t *)base;
    if (memcmp(hdr->magic, VX_IDX_MAGIC, 8u) != 0 ||
        hdr->version != VX_IDX_VERSION ||
        hdr->file_bytes != (uint64_t)fsize) {
        munmap(base, fsize);
        close(fd);
        return VX_ERR_CORRUPT;
    }

    /* If ART, delegate to vx_art_open */
    if (hdr->kind == VX_IDX_KIND_ART) {
        munmap(base, fsize);
        close(fd);
        return vx_art_open(path, out_idx);
    }

    /* CRC32 check: hdr[0..56) || file[64..file_bytes) */
    uint32_t crc = vx_crc32_update(0u, base, 56u);
    if (fsize > sizeof(vx_idx_hdr_t)) {
        crc = vx_crc32_update(crc, (const uint8_t *)base + sizeof(vx_idx_hdr_t),
                              fsize - sizeof(vx_idx_hdr_t));
    }
    if (crc != hdr->file_crc32) {
        munmap(base, fsize);
        close(fd);
        return VX_ERR_CORRUPT;
    }

    vx_idx_file_t *idx = (vx_idx_file_t *)calloc(1, sizeof(vx_idx_file_t));
    if (!idx) {
        munmap(base, fsize);
        close(fd);
        return VX_ERR_OOM;
    }

    vx_owner_init(&idx->hdr, VX_OWN_INDEX, vx_idx_destroy_fn);
    idx->fd        = fd;
    idx->mmap_base = (const uint8_t *)base;
    idx->mmap_len  = fsize;
    idx->file_path = strdup(path);
    if (!idx->file_path) {
        vx_owner_release(&idx->hdr);
        return VX_ERR_OOM;
    }
    idx->file_hdr  = hdr;
    atomic_init(&idx->unlink_on_destroy, false);

    *out_idx = idx;
    return VX_OK;
}

/* ============================================================================
 * 2. Point Equality Lookup (Hash & ART)
 * ============================================================================ */
vx_status_t vx_idx_lookup_eq(const vx_idx_file_t *idx, const vx_delmap_t *const *chunk_delmaps,
                             const uint8_t *enc_key, uint32_t key_len,
                             vx_idx_hit_fn on_hit, void *ctx) {
    if (!idx || !enc_key || !on_hit) return VX_ERR_INVALID;
    const vx_idx_hdr_t *hdr = idx->file_hdr;

    if (hdr->kind == VX_IDX_KIND_ART) {
        return vx_art_lookup_eq(idx, chunk_delmaps, enc_key, key_len, on_hit, ctx);
    }

    if (hdr->kind == VX_IDX_KIND_HASH) {
        uint32_t cap = hdr->aux_param;
        if (cap == 0 || (cap & (cap - 1u)) != 0) return VX_ERR_CORRUPT;
        const vx_sec_hash_slot_t *slots =
            (const vx_sec_hash_slot_t *)(idx->mmap_base + hdr->root_off);
        const uint32_t mask = cap - 1u;
        const uint64_t h    = vx_sec_hash_key(enc_key, key_len);
        uint32_t       pos  = (uint32_t)h & mask;

        for (uint32_t p = 0; p < cap; ++p) {
            const vx_sec_hash_slot_t *s = &slots[(pos + p) & mask];
            if (s->nrows == 0 || p > s->psl) break; /* Robin Hood early termination */
            if (s->hash == h) {
                if (s->nrows == 1) {
                    uint32_t r = s->row_or_post_off;
                    const vx_delmap_t *dm = chunk_delmaps ? chunk_delmaps[r >> 12] : NULL;
                    if (!dm || !vx_del_is_deleted(dm, r)) {
                        bool stop = false;
                        return on_hit(ctx, r, &stop);
                    }
                } else {
                    const uint32_t *rows = (const uint32_t *)(idx->mmap_base + s->row_or_post_off);
                    for (uint16_t i = 0; i < s->nrows; ++i) {
                        uint32_t r = rows[i];
                        const vx_delmap_t *dm = chunk_delmaps ? chunk_delmaps[r >> 12] : NULL;
                        if (!dm || !vx_del_is_deleted(dm, r)) {
                            bool stop = false;
                            vx_status_t st = on_hit(ctx, r, &stop);
                            if (st != VX_OK || stop) return st;
                        }
                    }
                }
                return VX_OK;
            }
        }
        return VX_OK;
    }

    return VX_ERR_INVALID;
}

static int vx_hash_entry_cmp(const void *pa, const void *pb) {
    const vx_art_entry_t *a = (const vx_art_entry_t *)pa;
    const vx_art_entry_t *b = (const vx_art_entry_t *)pb;
    int c = vx_key_cmp(a->key, a->key_len, b->key, b->key_len);
    if (c != 0) return c;
    return (a->row_idx < b->row_idx) ? -1 : (a->row_idx > b->row_idx ? 1 : 0);
}

/* Dynamic byte buffer for file writing */
typedef struct vx_buf_writer {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} vx_buf_writer_t;

static vx_status_t vx_bw_append(vx_buf_writer_t *bw, const void *src, size_t n, size_t *out_off) {
    if (out_off) *out_off = bw->len;
    if (bw->len + n > bw->cap) {
        size_t ncap = bw->cap ? bw->cap * 2 : 4096;
        while (ncap < bw->len + n) ncap *= 2;
        uint8_t *nd = (uint8_t *)realloc(bw->data, ncap);
        if (!nd) return VX_ERR_OOM;
        bw->data = nd;
        bw->cap  = ncap;
    }
    memcpy(bw->data + bw->len, src, n);
    bw->len += n;
    return VX_OK;
}

static vx_status_t vx_bw_pad8(vx_buf_writer_t *bw) {
    size_t rem = bw->len & 7u;
    if (rem > 0) {
        uint8_t zeros[8] = { 0 };
        return vx_bw_append(bw, zeros, 8u - rem, NULL);
    }
    return VX_OK;
}

/* Extracts order-preserving key bytes for a single row from segment column views */
static vx_status_t vx_extract_seg_row_key(const vx_seg_t *seg, const vx_man_index_def_t *idx_def,
                                          uint32_t r, uint8_t *out_key, uint32_t *out_len,
                                          bool *out_has_null) {
    *out_len = 0;
    *out_has_null = false;

    uint32_t k  = r >> 12;
    uint32_t ro = r & (VX_BATCH_CAP - 1u);
    if (k >= seg->nchunks) return VX_ERR_INVALID;
    const vx_chunk_t *ch = seg->chunk_views[k];

    size_t total_len = 0;
    for (uint32_t c = 0; c < idx_def->ncols; ++c) {
        uint32_t tc = idx_def->col_ids[c];
        const vx_col_chunk_t *col = &ch->cols[tc];
        if (col->has_nulls && col->validity &&
            ((col->validity[ro >> 6] >> (ro & 63u)) & 1ULL) == 0ULL) {
            *out_has_null = true;
            return VX_OK;
        }

        switch (col->type) {
        case VX_TYPE_U64: {
            uint64_t v = ((const uint64_t *)col->data)[ro];
            vx_key_encode_u64(v, out_key + total_len);
            total_len += 8;
            break;
        }
        case VX_TYPE_I64: {
            int64_t v = ((const int64_t *)col->data)[ro];
            vx_key_encode_i64(v, out_key + total_len);
            total_len += 8;
            break;
        }
        case VX_TYPE_F64: {
            double v = ((const double *)col->data)[ro];
            vx_key_encode_f64(v, out_key + total_len);
            total_len += 8;
            break;
        }
        case VX_TYPE_BOOL: {
            uint8_t v = ((const uint8_t *)col->data)[ro];
            out_key[total_len++] = v ? 1 : 0;
            break;
        }
        case VX_TYPE_STR: {
            vx_strview_t sv = ((const vx_strview_t *)col->data)[ro];
            const uint8_t *ptr = vx_strview_ptr(&sv, col->strpool);
            total_len += vx_key_encode_str(ptr, sv.inl.len, out_key + total_len);
            break;
        }
        }
    }
    *out_len = (uint32_t)total_len;
    return VX_OK;
}

vx_status_t vx_idx_build_from_seg(const char *dir_path, const vx_seg_t *seg,
                                  const vx_man_index_def_t *idx_def,
                                  vx_idx_file_t **out_idx) {
    if (!dir_path || !seg || !idx_def || !out_idx) return VX_ERR_INVALID;
    *out_idx = NULL;

    const uint32_t nrows = seg->file_hdr->nrows;
    vx_art_entry_t *entries = NULL;
    uint32_t n_indexed = 0;

    if (nrows > 0) {
        entries = (vx_art_entry_t *)calloc(nrows, sizeof(*entries));
        if (!entries) return VX_ERR_OOM;

        uint8_t key_buf[1024];
        for (uint32_t r = 0; r < nrows; ++r) {
            uint32_t klen = 0;
            bool has_null = false;
            vx_status_t st = vx_extract_seg_row_key(seg, idx_def, r, key_buf, &klen, &has_null);
            if (st != VX_OK || has_null) continue;

            uint8_t *kd = (uint8_t *)malloc(klen ? klen : 1u);
            if (!kd) {
                for (uint32_t j = 0; j < n_indexed; ++j) free((void *)entries[j].key);
                free(entries);
                return VX_ERR_OOM;
            }
            if (klen > 0) memcpy(kd, key_buf, klen);
            entries[n_indexed].key     = kd;
            entries[n_indexed].key_len = klen;
            entries[n_indexed].row_idx = r;
            n_indexed++;
        }
    }

    bool is_art = (idx_def->flags & VX_IDX_KIND_ART) != 0 || (idx_def->flags == 0);
    uint8_t is_uniq  = (idx_def->flags & 4u) ? 1u : 0u;

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/seg_%lu_i%u.idx",
             dir_path, (unsigned long)seg->file_hdr->segment_id, idx_def->index_id);

    if (is_art) {
        vx_status_t st = vx_art_build_file(final_path, seg->file_hdr->table_id, idx_def->index_id,
                                           seg->file_hdr->segment_id, (uint16_t)idx_def->ncols,
                                           is_uniq, entries, n_indexed);
        for (uint32_t j = 0; j < n_indexed; ++j) free((void *)entries[j].key);
        free(entries);
        if (st != VX_OK) return st;

        return vx_art_open(final_path, out_idx);
    }

    /* VX_IDX_KIND_HASH: Robin Hood slot array */
    /* Sort items by key order first */
    if (n_indexed > 1) {
        qsort(entries, n_indexed, sizeof(vx_art_entry_t), vx_hash_entry_cmp);
    }

    /* Check unique constraint violation if unique index */
    if (is_uniq) {
        for (uint32_t i = 1; i < n_indexed; ++i) {
            if (entries[i - 1].key_len == entries[i].key_len &&
                memcmp(entries[i - 1].key, entries[i].key, entries[i].key_len) == 0) {
                for (uint32_t j = 0; j < n_indexed; ++j) free((void *)entries[j].key);
                free(entries);
                return VX_ERR_UNIQUE;
            }
        }
    }

    vx_buf_writer_t bw;
    memset(&bw, 0, sizeof(bw));

    /* Reserve 64B header */
    vx_idx_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_IDX_MAGIC, 8u);
    hdr.version       = VX_IDX_VERSION;
    hdr.kind          = VX_IDX_KIND_HASH;
    hdr.is_unique     = is_uniq;
    hdr.ncols         = (uint16_t)idx_def->ncols;
    hdr.table_id      = seg->file_hdr->table_id;
    hdr.index_id      = idx_def->index_id;
    hdr.segment_id    = seg->file_hdr->segment_id;
    hdr.nrows_indexed = n_indexed;

    vx_bw_append(&bw, &hdr, sizeof(hdr), NULL);

    uint32_t cap = 16;
    while (cap < (uint32_t)((double)n_indexed / 0.7) + 1u) cap *= 2;
    vx_sec_hash_slot_t *slots = (vx_sec_hash_slot_t *)calloc(cap, sizeof(*slots));
    if (!slots) {
        for (uint32_t j = 0; j < n_indexed; ++j) free((void *)entries[j].key);
        free(entries);
        return VX_ERR_OOM;
    }

    const uint32_t mask = cap - 1u;
    for (uint32_t i = 0; i < n_indexed; ++i) {
        uint64_t h = vx_sec_hash_key(entries[i].key, entries[i].key_len);
        uint32_t pos = (uint32_t)h & mask;
        vx_sec_hash_slot_t entry = {
            .hash = h,
            .row_or_post_off = entries[i].row_idx,
            .nrows = 1,
            .psl = 0
        };
        for (;;) {
            if (slots[pos].nrows == 0) {
                slots[pos] = entry;
                break;
            }
            if (slots[pos].hash == entry.hash && !is_uniq) {
                /* Duplicate key in non-unique index */
                slots[pos].nrows++;
                break;
            }
            if (entry.psl > slots[pos].psl) {
                vx_sec_hash_slot_t tmp = slots[pos];
                slots[pos] = entry;
                entry = tmp;
            }
            pos = (pos + 1u) & mask;
            entry.psl++;
        }
    }

    vx_bw_pad8(&bw);
    size_t slots_off = 0;
    vx_bw_append(&bw, slots, (size_t)cap * sizeof(*slots), &slots_off);
    free(slots);
    hdr.root_off  = (uint64_t)slots_off;
    hdr.aux_param = cap;

    for (uint32_t j = 0; j < n_indexed; ++j) free((void *)entries[j].key);
    free(entries);

    vx_bw_pad8(&bw);
    hdr.file_bytes = bw.len;

    /* Compute CRC32 */
    uint32_t crc = vx_crc32_update(0u, &hdr, 56u);
    if (bw.len > sizeof(vx_idx_hdr_t)) {
        crc = vx_crc32_update(crc, bw.data + sizeof(vx_idx_hdr_t), bw.len - sizeof(vx_idx_hdr_t));
    }
    hdr.file_crc32 = crc;
    memcpy(bw.data, &hdr, sizeof(hdr));

    /* Write to <final_path>.tmp -> fsync -> rename */
    char tmp_path[530];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    int fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { free(bw.data); return VX_ERR_IO; }

    size_t written = 0;
    while (written < bw.len) {
        ssize_t n = write(fd, bw.data + written, bw.len - written);
        if (n <= 0) { close(fd); (void)unlink(tmp_path); free(bw.data); return VX_ERR_IO; }
        written += (size_t)n;
    }
    fdatasync(fd);
    close(fd);
    free(bw.data);

    if (rename(tmp_path, final_path) != 0) {
        (void)unlink(tmp_path);
        return VX_ERR_IO;
    }

    return vx_idx_open(final_path, out_idx);
}
