#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vx_manifest.h"
#include "vx_failpoint.h"

_Static_assert(offsetof(vx_del_hdr_t, file_crc32) == 56, "vx_del_hdr_t crc offset mismatch");
_Static_assert(offsetof(vx_man_hdr_t, file_crc32) == 56, "vx_man_hdr_t crc offset mismatch");

/* ============================================================================
 * 1. CRC32, Alignment & Durable POSIX I/O Primitives
 * ============================================================================ */
static uint32_t       g_man_crc_tab[256];
static pthread_once_t g_man_crc_once = PTHREAD_ONCE_INIT;

static void vx_man_crc_init(void) {
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (uint32_t b = 0; b < 8u; ++b) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_man_crc_tab[i] = c;
    }
}

static uint32_t vx_man_crc32(uint32_t crc, const void *buf, size_t len) {
    pthread_once(&g_man_crc_once, vx_man_crc_init);
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = g_man_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static inline uint64_t vx_align8(uint64_t v) {
    return (v + 7ULL) & ~7ULL;
}

static inline uint32_t vx_popcnt64_m(uint64_t x) {
#if defined(_MSC_VER)
    return (uint32_t)__popcnt64(x);
#else
    return (uint32_t)__builtin_popcountll(x);
#endif
}

static inline bool vx_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) return false;
    *out = a * b;
    return true;
}

static int vx_fdatasync_m(int fd) {
#if defined(__linux__)
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

static vx_status_t vx_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p   = (const uint8_t *)buf;
    size_t         off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return VX_ERR_IO;
        }
        off += (size_t)n;
    }
    return VX_OK;
}

static vx_status_t vx_read_all(int fd, void *buf, size_t len) {
    uint8_t *p   = (uint8_t *)buf;
    size_t   off = 0;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return VX_ERR_IO;
        }
        if (n == 0) return VX_ERR_CORRUPT;
        off += (size_t)n;
    }
    return VX_OK;
}

vx_status_t vx_fsync_dir(const char *dir_path) {
    if (!dir_path) return VX_ERR_INVALID;
    int dfd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return VX_ERR_IO;
    int rc = fsync(dfd);
    int c  = close(dfd);
    return (rc == 0 && c == 0) ? VX_OK : VX_ERR_IO;
}

static vx_status_t vx_join_path(const char *dir, const char *name, char *out, size_t sz) {
    int n = snprintf(out, sz, "%s/%s", dir, name);
    return (n > 0 && (size_t)n < sz) ? VX_OK : VX_ERR_INVALID;
}

vx_status_t vx_seg_format_path(const char *dir_path, uint64_t segment_id,
                               char *out_buf, size_t buf_sz) {
    int n = snprintf(out_buf, buf_sz, "%s/seg_%08" PRIu64 ".vx", dir_path, segment_id);
    return (n > 0 && (size_t)n < buf_sz) ? VX_OK : VX_ERR_INVALID;
}

vx_status_t vx_del_format_path(const char *dir_path, uint64_t segment_id, uint64_t del_gen,
                               char *out_buf, size_t buf_sz) {
    int n = snprintf(out_buf, buf_sz, "%s/seg_%08" PRIu64 "_g%04" PRIu64 ".del",
                     dir_path, segment_id, del_gen);
    return (n > 0 && (size_t)n < buf_sz) ? VX_OK : VX_ERR_INVALID;
}

/* ============================================================================
 * 2. Versioned .del Tombstone Sidecar Writer & Sparse Loader
 * ============================================================================ */
vx_status_t vx_del_write(const char *dir_path, uint32_t table_id, uint64_t segment_id,
                         uint64_t generation, uint32_t nrows, uint32_t nchunks,
                         const vx_delmap_t *const *chunk_delmaps, uint32_t *out_live_rows) {
    if (!dir_path || generation == 0) return VX_ERR_INVALID;

    const uint32_t expected_nc = nrows ? ((nrows + VX_BATCH_CAP - 1u) / VX_BATCH_CAP) : 0u;
    if (nchunks != expected_nc) return VX_ERR_INVALID;

    char final_path[PATH_MAX], tmp_path[PATH_MAX];
    vx_status_t st = vx_del_format_path(dir_path, segment_id, generation,
                                        final_path, sizeof(final_path));
    if (st != VX_OK) return st;
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) return VX_ERR_INVALID;

    uint32_t *chunk_live = NULL;
    if (nchunks > 0) {
        chunk_live = (uint32_t *)calloc(nchunks, sizeof(uint32_t));
        if (!chunk_live) return VX_ERR_OOM;
    }

    uint32_t total_live = 0;
    for (uint32_t k = 0; k < nchunks; ++k) {
        uint32_t c_rows = ((uint64_t)(k + 1u) * VX_BATCH_CAP <= nrows)
                              ? VX_BATCH_CAP : (nrows - k * VX_BATCH_CAP);
        uint32_t live   = c_rows;
        if (chunk_delmaps && chunk_delmaps[k]) {
            live = chunk_delmaps[k]->live_count;
            if (live > c_rows) { free(chunk_live); return VX_ERR_INVALID; }
        }
        chunk_live[k] = live;
        total_live   += live;
    }

    const uint64_t raw_after_live = sizeof(vx_del_hdr_t) + (uint64_t)nchunks * sizeof(uint32_t);
    const uint64_t bits_off       = vx_align64(raw_after_live);
    const size_t   pad_bytes      = (size_t)(bits_off - raw_after_live);
    const uint64_t file_bytes     = bits_off + (uint64_t)nchunks * 512ULL;

    vx_del_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_DEL_MAGIC, 8u);
    hdr.version    = VX_FMT_VERSION;
    hdr.table_id   = table_id;
    hdr.segment_id = segment_id;
    hdr.generation = generation;
    hdr.nrows      = nrows;
    hdr.live_rows  = total_live;
    hdr.nchunks    = nchunks;
    hdr.bits_off   = (uint32_t)bits_off;
    hdr.file_bytes = file_bytes;

    /* Streaming zero-copy CRC32 pass over hdr[0..56) || chunk_live || pad || bitmaps */
    static const uint8_t  k_zero64[64]              = { 0 };
    static const uint64_t k_zero_bm[VX_BITMAP_WORDS] = { 0 };

    uint32_t crc = vx_man_crc32(0u, &hdr, 56u);
    if (nchunks > 0) crc = vx_man_crc32(crc, chunk_live, (size_t)nchunks * sizeof(uint32_t));
    if (pad_bytes > 0) crc = vx_man_crc32(crc, k_zero64, pad_bytes);
    for (uint32_t k = 0; k < nchunks; ++k) {
        const uint64_t *bm = (chunk_delmaps && chunk_delmaps[k])
                                 ? chunk_delmaps[k]->bits : k_zero_bm;
        crc = vx_man_crc32(crc, bm, sizeof(k_zero_bm));
    }
    hdr.file_crc32 = crc;

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(chunk_live); return VX_ERR_IO; }

    st = vx_write_all(fd, &hdr, sizeof(hdr));
    if (st == VX_OK && nchunks > 0) {
        st = vx_write_all(fd, chunk_live, (size_t)nchunks * sizeof(uint32_t));
    }
    if (st == VX_OK && pad_bytes > 0) {
        st = vx_write_all(fd, k_zero64, pad_bytes);
    }
    for (uint32_t k = 0; st == VX_OK && k < nchunks; ++k) {
        const uint64_t *bm = (chunk_delmaps && chunk_delmaps[k])
                                 ? chunk_delmaps[k]->bits : k_zero_bm;
        st = vx_write_all(fd, bm, sizeof(k_zero_bm));
    }
    if (st == VX_OK && vx_fdatasync_m(fd) != 0) st = VX_ERR_IO;
    if (close(fd) != 0 && st == VX_OK) st = VX_ERR_IO;

    VX_FAILPOINT("del_pre_rename");
    if (st == VX_OK && rename(tmp_path, final_path) != 0) st = VX_ERR_IO;
    if (st != VX_OK) (void)unlink(tmp_path);

    free(chunk_live);
    if (st == VX_OK && out_live_rows) *out_live_rows = total_live;
    return st;
}

static void vx_delmap_destroy_fn(vx_owner_t *o) {
    free(o);
}

static vx_delmap_t *vx_delmap_alloc_aligned(uint64_t generation, uint32_t nrows,
                                            uint32_t live_count, const uint64_t *bits) {
    vx_delmap_t *dm = NULL;
    if (posix_memalign((void **)&dm, VX_ALIGN, sizeof(*dm)) != 0 || !dm) return NULL;
    memset(dm, 0, sizeof(*dm));
    vx_owner_init(&dm->hdr, VX_OWN_DELMAP, vx_delmap_destroy_fn);
    dm->generation = generation;
    dm->nrows      = nrows;
    dm->live_count = live_count;
    memcpy(dm->bits, bits, sizeof(dm->bits));
    return dm;
}

vx_status_t vx_del_load(const char *dir_path, uint32_t expected_table_id,
                        uint64_t expected_seg_id, uint64_t expected_gen,
                        uint32_t expected_nrows, uint32_t expected_nchunks,
                        vx_delmap_t ***out_chunk_delmaps, uint32_t *out_live_rows) {
    if (!dir_path || !out_chunk_delmaps || expected_gen == 0) return VX_ERR_INVALID;
    *out_chunk_delmaps = NULL;

    char path[PATH_MAX];
    vx_status_t st = vx_del_format_path(dir_path, expected_seg_id, expected_gen, path, sizeof(path));
    if (st != VX_OK) return st;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return VX_ERR_IO;

    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size < (off_t)sizeof(vx_del_hdr_t)) {
        close(fd);
        return VX_ERR_CORRUPT;
    }

    const uint64_t fsize = (uint64_t)sb.st_size;
    uint8_t       *buf   = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { close(fd); return VX_ERR_OOM; }

    st = vx_read_all(fd, buf, (size_t)fsize);
    close(fd);
    if (st != VX_OK) { free(buf); return st; }

    const vx_del_hdr_t *hdr = (const vx_del_hdr_t *)buf;
    const uint64_t      min_bits_off = vx_align64(sizeof(vx_del_hdr_t) +
                                                  (uint64_t)expected_nchunks * sizeof(uint32_t));
    const uint64_t      expect_bytes = min_bits_off + (uint64_t)expected_nchunks * 512ULL;

    if (memcmp(hdr->magic, VX_DEL_MAGIC, 8u) != 0 ||
        hdr->version != VX_FMT_VERSION ||
        hdr->table_id != expected_table_id ||
        hdr->segment_id != expected_seg_id ||
        hdr->generation != expected_gen ||
        hdr->nrows != expected_nrows ||
        hdr->nchunks != expected_nchunks ||
        hdr->bits_off != min_bits_off ||
        hdr->file_bytes != fsize ||
        fsize != expect_bytes) {
        free(buf);
        return VX_ERR_CORRUPT;
    }

    uint32_t crc = vx_man_crc32(0u, buf, 56u);
    if (fsize > sizeof(vx_del_hdr_t)) {
        crc = vx_man_crc32(crc, buf + sizeof(vx_del_hdr_t), (size_t)(fsize - sizeof(vx_del_hdr_t)));
    }
    if (crc != hdr->file_crc32) {
        free(buf);
        return VX_ERR_CORRUPT;
    }

    vx_delmap_t **arr = NULL;
    if (expected_nchunks > 0) {
        arr = (vx_delmap_t **)calloc(expected_nchunks, sizeof(*arr));
        if (!arr) { free(buf); return VX_ERR_OOM; }
    }

    const uint32_t *chunk_live = (const uint32_t *)(buf + sizeof(vx_del_hdr_t));
    const uint8_t  *bm_base    = buf + hdr->bits_off;
    uint32_t        sum_live   = 0;

    for (uint32_t k = 0; k < expected_nchunks; ++k) {
        const uint32_t  c_rows = ((uint64_t)(k + 1u) * VX_BATCH_CAP <= expected_nrows)
                                     ? VX_BATCH_CAP : (expected_nrows - k * VX_BATCH_CAP);
        const uint64_t *bm     = (const uint64_t *)(bm_base + (size_t)k * 512u);

        uint32_t del_cnt = 0;
        for (uint32_t w = 0; w < VX_BITMAP_WORDS; ++w) {
            uint64_t word = bm[w];
            /* Reject stray set bits past c_rows in final partial chunk */
            uint32_t bit_base = w << 6;
            if (bit_base >= c_rows && word != 0ULL) { st = VX_ERR_CORRUPT; goto fail; }
            if (bit_base < c_rows && bit_base + 64u > c_rows) {
                uint32_t rem = c_rows - bit_base;
                if ((word >> rem) != 0ULL) { st = VX_ERR_CORRUPT; goto fail; }
            }
            del_cnt += vx_popcnt64_m(word);
        }

        const uint32_t live = c_rows - del_cnt;
        if (live != chunk_live[k]) { st = VX_ERR_CORRUPT; goto fail; }
        sum_live += live;

        /* Sparse invariant: leave 100%-live chunks NULL for dense TableScan fast path */
        if (del_cnt > 0) {
            arr[k] = vx_delmap_alloc_aligned(expected_gen, c_rows, live, bm);
            if (!arr[k]) { st = VX_ERR_OOM; goto fail; }
        }
    }

    if (sum_live != hdr->live_rows) { st = VX_ERR_CORRUPT; goto fail; }

    free(buf);
    *out_chunk_delmaps = arr;
    if (out_live_rows) *out_live_rows = sum_live;
    return VX_OK;

fail:
    if (arr) {
        for (uint32_t k = 0; k < expected_nchunks; ++k) {
            if (arr[k]) vx_owner_release(&arr[k]->hdr);
        }
        free(arr);
    }
    free(buf);
    return st;
}

/* ============================================================================
 * 3. Atomic MANIFEST Serializer, Publisher & Loader
 * ============================================================================ */
void vx_manifest_free(vx_manifest_t *man) {
    if (!man) return;
    if (man->tables) {
        for (uint32_t t = 0; t < man->ntables; ++t) {
            vx_man_table_def_t *td = &man->tables[t];
            free(td->name);
            if (td->cols) {
                for (uint32_t c = 0; c < td->ncols; ++c) free(td->cols[c].name);
                free(td->cols);
            }
            if (td->indexes) {
                for (uint32_t i = 0; i < td->n_indexes; ++i) {
                    free(td->indexes[i].name);
                    free(td->indexes[i].col_ids);
                }
                free(td->indexes);
            }
        }
        free(man->tables);
    }
    free(man->segments);
    memset(man, 0, sizeof(*man));
}

vx_status_t vx_manifest_publish(const char *dir_path, vx_manifest_t *man) {
    if (!dir_path || !man) return VX_ERR_INVALID;

    /* Pass 1: measure exact binary size */
    uint64_t total = sizeof(vx_man_hdr_t);
    for (uint32_t t = 0; t < man->ntables; ++t) {
        const vx_man_table_def_t *td = &man->tables[t];
        total += sizeof(vx_man_table_t) + vx_align8(td->name ? strlen(td->name) : 0u);
        for (uint32_t c = 0; c < td->ncols; ++c) {
            total += sizeof(vx_man_col_t) + vx_align8(td->cols[c].name ? strlen(td->cols[c].name) : 0u);
        }
        for (uint32_t i = 0; i < td->n_indexes; ++i) {
            const vx_man_index_def_t *id = &td->indexes[i];
            total += sizeof(vx_man_index_t) +
                     vx_align8(id->name ? strlen(id->name) : 0u) +
                     vx_align8((uint64_t)id->ncols * sizeof(uint32_t));
        }
    }
    total += (uint64_t)man->nsegments * sizeof(vx_man_seg_t);

    uint8_t *buf = (uint8_t *)calloc(1u, (size_t)total);
    if (!buf) return VX_ERR_OOM;

    /* Pass 2: serialize header + tables + segments */
    const uint64_t next_gen = man->manifest_gen + 1ULL;
    vx_man_hdr_t   hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_MAN_MAGIC, 8u);
    hdr.version        = VX_FMT_VERSION;
    hdr.flags          = man->flags;
    hdr.manifest_gen   = next_gen;
    hdr.checkpoint_lsn = man->checkpoint_lsn;
    hdr.next_seg_id    = man->next_seg_id;
    hdr.ntables        = man->ntables;
    hdr.nsegments      = man->nsegments;
    hdr.file_bytes     = total;

    size_t pos = sizeof(vx_man_hdr_t);
    for (uint32_t t = 0; t < man->ntables; ++t) {
        const vx_man_table_def_t *td   = &man->tables[t];
        const uint32_t            nlen = td->name ? (uint32_t)strlen(td->name) : 0u;
        vx_man_table_t            wt   = {
            .table_id   = td->table_id,
            .name_len   = nlen,
            .ncols      = td->ncols,
            .pk_col_idx = td->pk_col_idx,
            .n_indexes  = td->n_indexes,
            ._pad       = 0
        };
        memcpy(buf + pos, &wt, sizeof(wt)); pos += sizeof(wt);
        if (nlen) memcpy(buf + pos, td->name, nlen);
        pos += (size_t)vx_align8(nlen);

        for (uint32_t c = 0; c < td->ncols; ++c) {
            const vx_man_col_def_t *cd   = &td->cols[c];
            const uint32_t          clen = cd->name ? (uint32_t)strlen(cd->name) : 0u;
            vx_man_col_t            wc   = {
                .col_id   = cd->col_id,
                .name_len = clen,
                .type     = (uint8_t)cd->type,
                .nullable = cd->nullable
            };
            memcpy(buf + pos, &wc, sizeof(wc)); pos += sizeof(wc);
            if (clen) memcpy(buf + pos, cd->name, clen);
            pos += (size_t)vx_align8(clen);
        }

        for (uint32_t i = 0; i < td->n_indexes; ++i) {
            const vx_man_index_def_t *id   = &td->indexes[i];
            const uint32_t            ilen = id->name ? (uint32_t)strlen(id->name) : 0u;
            vx_man_index_t            wi   = {
                .index_id = id->index_id,
                .name_len = ilen,
                .ncols    = id->ncols,
                .flags    = id->flags
            };
            memcpy(buf + pos, &wi, sizeof(wi)); pos += sizeof(wi);
            if (ilen) memcpy(buf + pos, id->name, ilen);
            pos += (size_t)vx_align8(ilen);
            if (id->ncols) {
                memcpy(buf + pos, id->col_ids, (size_t)id->ncols * sizeof(uint32_t));
                pos += (size_t)vx_align8((uint64_t)id->ncols * sizeof(uint32_t));
            }
        }
    }

    if (man->nsegments > 0) {
        size_t sbytes = (size_t)man->nsegments * sizeof(vx_man_seg_t);
        memcpy(buf + pos, man->segments, sbytes);
        pos += sbytes;
    }

    memcpy(buf, &hdr, sizeof(hdr));
    uint32_t crc = vx_man_crc32(0u, buf, 56u);
    if (total > sizeof(vx_man_hdr_t)) {
        crc = vx_man_crc32(crc, buf + sizeof(vx_man_hdr_t), (size_t)(total - sizeof(vx_man_hdr_t)));
    }
    hdr.file_crc32 = crc;
    memcpy(buf + 56u, &hdr.file_crc32, sizeof(hdr.file_crc32));

    /* Atomic commit: MANIFEST.tmp -> fdatasync -> rename -> fsync(dir_fd) */
    char tmp_path[PATH_MAX], final_path[PATH_MAX];
    vx_status_t st = vx_join_path(dir_path, "MANIFEST.tmp", tmp_path, sizeof(tmp_path));
    if (st == VX_OK) st = vx_join_path(dir_path, "MANIFEST", final_path, sizeof(final_path));
    if (st != VX_OK) { free(buf); return st; }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); return VX_ERR_IO; }

    st = vx_write_all(fd, buf, (size_t)total);
    VX_FAILPOINT("man_mid_tmp_write");
    if (st == VX_OK && vx_fdatasync_m(fd) != 0) st = VX_ERR_IO;
    if (close(fd) != 0 && st == VX_OK) st = VX_ERR_IO;
    free(buf);

    VX_FAILPOINT("del_pre_rename");
    if (st == VX_OK && rename(tmp_path, final_path) != 0) st = VX_ERR_IO;
    if (st == VX_OK) st = vx_fsync_dir(dir_path);
    if (st != VX_OK) {
        (void)unlink(tmp_path);
        return st;
    }

    man->manifest_gen = next_gen;
    return VX_OK;
}

static bool vx_buf_take(const uint8_t *buf, size_t len, size_t *io_pos, void *dst, size_t n) {
    if (n > len - *io_pos) return false;
    memcpy(dst, buf + *io_pos, n);
    *io_pos += n;
    return true;
}

static vx_status_t vx_buf_take_str8(const uint8_t *buf, size_t len, size_t *io_pos,
                                    uint32_t slen, char **out_str) {
    uint64_t padded = vx_align8(slen);
    if (padded > len - *io_pos) return VX_ERR_CORRUPT;
    char *s = (char *)malloc((size_t)slen + 1u);
    if (!s) return VX_ERR_OOM;
    if (slen) memcpy(s, buf + *io_pos, slen);
    s[slen] = '\0';
    *io_pos += (size_t)padded;
    *out_str = s;
    return VX_OK;
}

vx_status_t vx_manifest_load(const char *dir_path, vx_manifest_t *out_man, bool *out_existed) {
    if (!dir_path || !out_man || !out_existed) return VX_ERR_INVALID;
    memset(out_man, 0, sizeof(*out_man));
    *out_existed = false;

    /* Clean any half-written MANIFEST.tmp left by a prior crash */
    char tmp_path[PATH_MAX], path[PATH_MAX], v1_snap[PATH_MAX];
    vx_status_t st = vx_join_path(dir_path, "MANIFEST.tmp", tmp_path, sizeof(tmp_path));
    if (st == VX_OK) st = vx_join_path(dir_path, "MANIFEST", path, sizeof(path));
    if (st == VX_OK) st = vx_join_path(dir_path, "app.wal.snap", v1_snap, sizeof(v1_snap));
    if (st != VX_OK) return st;

    (void)unlink(tmp_path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) return VX_ERR_IO;
        /* Detect unmigrated V1 checkpoint cleanly instead of silently ignoring it */
        struct stat v1_st;
        if (stat(v1_snap, &v1_st) == 0) return VX_ERR_SCHEMA;
        out_man->next_seg_id = 1ULL;
        return VX_OK;
    }

    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size < (off_t)sizeof(vx_man_hdr_t)) {
        close(fd);
        return VX_ERR_CORRUPT;
    }

    const size_t fsize = (size_t)sb.st_size;
    uint8_t     *buf   = (uint8_t *)malloc(fsize);
    if (!buf) { close(fd); return VX_ERR_OOM; }

    st = vx_read_all(fd, buf, fsize);
    close(fd);
    if (st != VX_OK) { free(buf); return st; }

    const vx_man_hdr_t *hdr = (const vx_man_hdr_t *)buf;
    if (memcmp(hdr->magic, VX_MAN_MAGIC, 8u) != 0 ||
        hdr->version != VX_FMT_VERSION ||
        hdr->file_bytes != (uint64_t)fsize) {
        free(buf);
        return VX_ERR_CORRUPT;
    }

    uint32_t crc = vx_man_crc32(0u, buf, 56u);
    if (fsize > sizeof(vx_man_hdr_t)) {
        crc = vx_man_crc32(crc, buf + sizeof(vx_man_hdr_t), fsize - sizeof(vx_man_hdr_t));
    }
    if (crc != hdr->file_crc32) {
        free(buf);
        return VX_ERR_CORRUPT;
    }

    out_man->manifest_gen   = hdr->manifest_gen;
    out_man->checkpoint_lsn = hdr->checkpoint_lsn;
    out_man->next_seg_id    = hdr->next_seg_id ? hdr->next_seg_id : 1ULL;
    out_man->flags          = hdr->flags;

    size_t pos = sizeof(vx_man_hdr_t);

    if (hdr->ntables > 0) {
        out_man->tables = (vx_man_table_def_t *)calloc(hdr->ntables, sizeof(*out_man->tables));
        if (!out_man->tables) { st = VX_ERR_OOM; goto fail; }
        out_man->ntables = hdr->ntables;
    }

    for (uint32_t t = 0; t < hdr->ntables; ++t) {
        vx_man_table_t wt;
        if (!vx_buf_take(buf, fsize, &pos, &wt, sizeof(wt))) { st = VX_ERR_CORRUPT; goto fail; }
        vx_man_table_def_t *td = &out_man->tables[t];
        td->table_id   = wt.table_id;
        td->pk_col_idx = wt.pk_col_idx;

        st = vx_buf_take_str8(buf, fsize, &pos, wt.name_len, &td->name);
        if (st != VX_OK) goto fail;

        if (wt.ncols > 0) {
            td->cols = (vx_man_col_def_t *)calloc(wt.ncols, sizeof(*td->cols));
            if (!td->cols) { st = VX_ERR_OOM; goto fail; }
            td->ncols = wt.ncols;
        }
        for (uint32_t c = 0; c < wt.ncols; ++c) {
            vx_man_col_t wc;
            if (!vx_buf_take(buf, fsize, &pos, &wc, sizeof(wc)) || wc.type > VX_TYPE_STR) {
                st = VX_ERR_CORRUPT;
                goto fail;
            }
            td->cols[c].col_id   = wc.col_id;
            td->cols[c].type     = (vx_type_t)wc.type;
            td->cols[c].nullable = wc.nullable;
            st = vx_buf_take_str8(buf, fsize, &pos, wc.name_len, &td->cols[c].name);
            if (st != VX_OK) goto fail;
        }

        if (wt.n_indexes > 0) {
            td->indexes = (vx_man_index_def_t *)calloc(wt.n_indexes, sizeof(*td->indexes));
            if (!td->indexes) { st = VX_ERR_OOM; goto fail; }
            td->n_indexes = wt.n_indexes;
        }
        for (uint32_t i = 0; i < wt.n_indexes; ++i) {
            vx_man_index_t wi;
            if (!vx_buf_take(buf, fsize, &pos, &wi, sizeof(wi))) { st = VX_ERR_CORRUPT; goto fail; }
            td->indexes[i].index_id = wi.index_id;
            td->indexes[i].flags    = wi.flags;
            st = vx_buf_take_str8(buf, fsize, &pos, wi.name_len, &td->indexes[i].name);
            if (st != VX_OK) goto fail;

            uint64_t raw_cbytes = 0;
            if (!vx_mul_u64(wi.ncols, sizeof(uint32_t), &raw_cbytes)) {
                st = VX_ERR_CORRUPT;
                goto fail;
            }
            uint64_t pad_cbytes = vx_align8(raw_cbytes);
            if (pad_cbytes > fsize - pos) { st = VX_ERR_CORRUPT; goto fail; }
            if (wi.ncols > 0) {
                td->indexes[i].col_ids = (uint32_t *)malloc((size_t)raw_cbytes);
                if (!td->indexes[i].col_ids) { st = VX_ERR_OOM; goto fail; }
                memcpy(td->indexes[i].col_ids, buf + pos, (size_t)raw_cbytes);
                td->indexes[i].ncols = wi.ncols;
            }
            pos += (size_t)pad_cbytes;
        }
    }

    uint64_t seg_bytes = 0;
    if (!vx_mul_u64(hdr->nsegments, sizeof(vx_man_seg_t), &seg_bytes) ||
        seg_bytes != (uint64_t)(fsize - pos)) {
        st = VX_ERR_CORRUPT;
        goto fail;
    }
    if (hdr->nsegments > 0) {
        out_man->segments = (vx_man_seg_t *)malloc((size_t)seg_bytes);
        if (!out_man->segments) { st = VX_ERR_OOM; goto fail; }
        memcpy(out_man->segments, buf + pos, (size_t)seg_bytes);
        out_man->nsegments = hdr->nsegments;
    }

    free(buf);
    *out_existed = true;
    return VX_OK;

fail:
    free(buf);
    vx_manifest_free(out_man);
    return st;
}

/* ============================================================================
 * 4. Strict Orphan Sweeper (O(S log S) lookup against active MANIFEST)
 * ============================================================================ */
typedef struct vx_live_seg_ref {
    uint64_t segment_id;
    uint64_t del_gen;
} vx_live_seg_ref_t;

static int vx_cmp_live_ref(const void *a, const void *b) {
    uint64_t sa = ((const vx_live_seg_ref_t *)a)->segment_id;
    uint64_t sb = ((const vx_live_seg_ref_t *)b)->segment_id;
    return (sa < sb) ? -1 : (sa > sb ? 1 : 0);
}

static const vx_live_seg_ref_t *vx_find_live_ref(const vx_live_seg_ref_t *refs,
                                                 uint32_t n, uint64_t seg_id) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (refs[mid].segment_id == seg_id) return &refs[mid];
        if (refs[mid].segment_id < seg_id) lo = mid + 1u;
        else hi = mid;
    }
    return NULL;
}

/* Parses an unsigned base-10 integer consuming >= 1 digits until *end_out. */
static bool vx_parse_digits_u64(const char *s, uint64_t *out, const char **end_out) {
    if (*s < '0' || *s > '9') return false;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10ULL) return false;
        v = v * 10ULL + d;
        ++s;
    }
    *out     = v;
    *end_out = s;
    return true;
}

typedef enum vx_fname_kind {
    VX_FN_OTHER = 0,
    VX_FN_TMP,
    VX_FN_SEG_VX,
    VX_FN_DEL,
    VX_FN_IDX
} vx_fname_kind_t;

static vx_fname_kind_t vx_classify_filename(const char *name, uint64_t *out_seg_id,
                                            uint64_t *out_del_gen, uint64_t *out_idx_id) {
    if (strcmp(name, "MANIFEST.tmp") == 0) return VX_FN_TMP;
    if (strncmp(name, "seg_", 4u) != 0)    return VX_FN_OTHER;

    const char *p = name + 4u;
    uint64_t    seg_id = 0, gen = 0, idx_id = 0;
    if (!vx_parse_digits_u64(p, &seg_id, &p)) return VX_FN_OTHER;

    if (strcmp(p, ".vx") == 0) {
        *out_seg_id = seg_id;
        return VX_FN_SEG_VX;
    }
    if (strcmp(p, ".vx.tmp") == 0) {
        return VX_FN_TMP;
    }
    if (strncmp(p, "_g", 2u) == 0) {
        p += 2u;
        if (!vx_parse_digits_u64(p, &gen, &p)) return VX_FN_OTHER;
        if (strcmp(p, ".del") == 0) {
            *out_seg_id  = seg_id;
            *out_del_gen = gen;
            return VX_FN_DEL;
        }
        if (strcmp(p, ".del.tmp") == 0) {
            return VX_FN_TMP;
        }
    }
    if (strncmp(p, "_i", 2u) == 0) {
        p += 2u;
        if (!vx_parse_digits_u64(p, &idx_id, &p)) return VX_FN_OTHER;
        if (strcmp(p, ".idx") == 0) {
            *out_seg_id = seg_id;
            if (out_idx_id) *out_idx_id = idx_id;
            return VX_FN_IDX;
        }
        if (strcmp(p, ".idx.tmp") == 0) {
            return VX_FN_TMP;
        }
    }
    return VX_FN_OTHER;
}

vx_status_t vx_manifest_sweep_orphans(const char *dir_path, const vx_manifest_t *man,
                                      uint32_t *out_removed) {
    if (!dir_path || !man) return VX_ERR_INVALID;
    if (out_removed) *out_removed = 0;

    vx_live_seg_ref_t *refs = NULL;
    if (man->nsegments > 0) {
        refs = (vx_live_seg_ref_t *)malloc((size_t)man->nsegments * sizeof(*refs));
        if (!refs) return VX_ERR_OOM;
        for (uint32_t i = 0; i < man->nsegments; ++i) {
            refs[i].segment_id = man->segments[i].segment_id;
            refs[i].del_gen    = man->segments[i].del_gen;
        }
        qsort(refs, man->nsegments, sizeof(*refs), vx_cmp_live_ref);
    }

    DIR *dp = opendir(dir_path);
    if (!dp) { free(refs); return VX_ERR_IO; }

    uint32_t       removed = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        uint64_t        seg_id = 0, gen = 0, idx_id = 0;
        vx_fname_kind_t kind   = vx_classify_filename(ent->d_name, &seg_id, &gen, &idx_id);
        bool            sweep  = false;

        if (kind == VX_FN_TMP) {
            sweep = true;
        } else if (kind == VX_FN_SEG_VX) {
            sweep = (vx_find_live_ref(refs, man->nsegments, seg_id) == NULL);
        } else if (kind == VX_FN_DEL) {
            const vx_live_seg_ref_t *r = vx_find_live_ref(refs, man->nsegments, seg_id);
            sweep = (r == NULL || r->del_gen == 0 || r->del_gen != gen);
        } else if (kind == VX_FN_IDX) {
            const vx_live_seg_ref_t *r = vx_find_live_ref(refs, man->nsegments, seg_id);
            if (!r) {
                sweep = true;
            } else {
                bool found_idx = false;
                for (uint32_t t = 0; t < man->ntables; ++t) {
                    for (uint32_t i = 0; i < man->tables[t].n_indexes; ++i) {
                        if (man->tables[t].indexes[i].index_id == (uint32_t)idx_id) {
                            found_idx = true;
                            break;
                        }
                    }
                    if (found_idx) break;
                }
                if (man->ntables > 0 && !found_idx) {
                    sweep = true;
                }
            }
        }

        if (sweep) {
            char full[PATH_MAX];
            if (vx_join_path(dir_path, ent->d_name, full, sizeof(full)) == VX_OK) {
                if (unlink(full) == 0) removed++;
            }
        }
    }

    closedir(dp);
    free(refs);

    if (out_removed) *out_removed = removed;
    return (removed > 0) ? vx_fsync_dir(dir_path) : VX_OK;
}

/* ============================================================================
 * 5. Full vex.open() Crash-Recovery State Machine (vx_recover_open)
 * ============================================================================ */
void vx_recovery_state_destroy(vx_recovery_state_t *rs) {
    if (!rs) return;
    if (rs->mounted) {
        for (uint32_t i = 0; i < rs->nmounted; ++i) {
            vx_mounted_seg_t *ms = &rs->mounted[i];
            if (ms->chunk_delmaps) {
                for (uint32_t k = 0; k < ms->nchunks; ++k) {
                    if (ms->chunk_delmaps[k]) vx_owner_release(&ms->chunk_delmaps[k]->hdr);
                }
                free(ms->chunk_delmaps);
            }
            if (ms->seg) vx_owner_release(&ms->seg->hdr);
        }
        free(rs->mounted);
    }
    vx_manifest_free(&rs->manifest);
    if (rs->wal_fd >= 0) {
        (void)flock(rs->wal_fd, LOCK_UN);
        close(rs->wal_fd);
    }
    memset(rs, 0, sizeof(*rs));
    rs->wal_fd = -1;
}

vx_status_t vx_recover_open(const char *dir_path, vx_wal_next_fn wal_next, void *wal_ctx,
                            vx_wal_apply_fn wal_apply, void *apply_ctx,
                            vx_recovery_state_t *out_state) {
    if (!dir_path || !out_state) return VX_ERR_INVALID;
    memset(out_state, 0, sizeof(*out_state));
    out_state->wal_fd = -1;

    /* Step 1: Open app.wal and acquire exclusive non-blocking OS flock */
    char wal_path[PATH_MAX];
    vx_status_t st = vx_join_path(dir_path, "app.wal", wal_path, sizeof(wal_path));
    if (st != VX_OK) return st;

    int wal_fd = open(wal_path, O_RDWR | O_CREAT, 0644);
    if (wal_fd < 0) return VX_ERR_IO;
    if (flock(wal_fd, LOCK_EX | LOCK_NB) != 0) {
        close(wal_fd);
        return VX_ERR_IO; /* database already locked by another process */
    }
    out_state->wal_fd = wal_fd;

    /* Step 2: Load and validate MANIFEST */
    bool existed = false;
    st = vx_manifest_load(dir_path, &out_state->manifest, &existed);
    if (st != VX_OK) goto fail;
    out_state->max_lsn = out_state->manifest.checkpoint_lsn;

    /* Step 3: Mount active .vx segments & versioned .del sidecars BEFORE sweeping orphans */
    const uint32_t nseg = out_state->manifest.nsegments;
    if (nseg > 0) {
        out_state->mounted = (vx_mounted_seg_t *)calloc(nseg, sizeof(*out_state->mounted));
        if (!out_state->mounted) { st = VX_ERR_OOM; goto fail; }
        out_state->nmounted = nseg;
    }

    for (uint32_t i = 0; i < nseg; ++i) {
        const vx_man_seg_t *ms = &out_state->manifest.segments[i];
        vx_mounted_seg_t   *mt = &out_state->mounted[i];
        mt->meta = *ms;

        char seg_path[PATH_MAX];
        st = vx_seg_format_path(dir_path, ms->segment_id, seg_path, sizeof(seg_path));
        if (st != VX_OK) goto fail;

        st = vx_seg_open(seg_path, &mt->seg);
        if (st != VX_OK) goto fail;

        /* Cross-check mounted .vx header against MANIFEST record */
        const vx_seg_hdr_t *sh = mt->seg->file_hdr;
        if (sh->table_id != ms->table_id ||
            sh->segment_id != ms->segment_id ||
            sh->nrows != ms->nrows ||
            mt->seg->footer->file_bytes != ms->file_bytes) {
            st = VX_ERR_CORRUPT;
            goto fail;
        }
        mt->nchunks = mt->seg->nchunks;

        if (ms->del_gen > 0) {
            uint32_t loaded_live = 0;
            st = vx_del_load(dir_path, ms->table_id, ms->segment_id, ms->del_gen,
                             ms->nrows, mt->nchunks, &mt->chunk_delmaps, &loaded_live);
            if (st != VX_OK) goto fail;
            if (loaded_live != ms->live_rows) { st = VX_ERR_CORRUPT; goto fail; }
        } else if (mt->nchunks > 0) {
            /* All NULL entries => 100% live fast path */
            mt->chunk_delmaps = (vx_delmap_t **)calloc(mt->nchunks, sizeof(vx_delmap_t *));
            if (!mt->chunk_delmaps) { st = VX_ERR_OOM; goto fail; }
        }
    }

    /* Step 4: Sweep unreferenced .tmp, retired .vx, and superseded .del files */
    st = vx_manifest_sweep_orphans(dir_path, &out_state->manifest, &out_state->orphans_swept);
    if (st != VX_OK) goto fail;

    /* Step 5: Incremental WAL Replay (skip lsn <= checkpoint_lsn; auto-heal torn tail) */
    uint64_t wal_off = 0;
    if (wal_next) {
        for (;;) {
            vx_wal_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            uint64_t frame_start = wal_off;
            bool     eof         = false;

            vx_status_t wr = wal_next(wal_ctx, wal_fd, &wal_off, &frame, &eof);
            if (wr == VX_ERR_CORRUPT) {
                /* Torn-write auto-healing: truncate cleanly to last valid CRC32 boundary */
                if (ftruncate(wal_fd, (off_t)frame_start) != 0 || vx_fdatasync_m(wal_fd) != 0) {
                    st = VX_ERR_IO;
                    goto fail;
                }
                wal_off                    = frame_start;
                out_state->wal_healed_tail = true;
                break;
            }
            if (wr != VX_OK) { st = wr; goto fail; }
            if (eof) break;

            if (frame.lsn <= out_state->manifest.checkpoint_lsn) {
                out_state->wal_skipped++;
                continue;
            }
            if (wal_apply) {
                st = wal_apply(apply_ctx, &frame);
                if (st != VX_OK) goto fail;
            }
            out_state->wal_replayed++;
            if (frame.lsn > out_state->max_lsn) out_state->max_lsn = frame.lsn;
        }
    }

    out_state->wal_valid_len = wal_off;
    if (lseek(wal_fd, (off_t)wal_off, SEEK_SET) < 0) { st = VX_ERR_IO; goto fail; }
    return VX_OK;

fail:
    vx_recovery_state_destroy(out_state);
    return st;
}
