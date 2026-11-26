#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vx_seg.h"
#include "vx_failpoint.h"

_Static_assert(offsetof(vx_seg_footer_t, meta_crc32) == 48, "meta_crc32 offset mismatch");
_Static_assert(offsetof(vx_seg_footer_t, file_crc32) == 52, "file_crc32 offset mismatch");

#define VX_SEG_IO_BUF_SZ (64u * 1024u)

/* ============================================================================
 * 1. IEEE 802.3 CRC32 & Low-Level I/O Helpers
 * ============================================================================ */
static uint32_t       g_crc32_tab[256];
static pthread_once_t g_crc32_once = PTHREAD_ONCE_INIT;

static void vx_crc32_init_tab(void) {
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (uint32_t b = 0; b < 8u; ++b) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc32_tab[i] = c;
    }
}

uint32_t vx_crc32_update(uint32_t crc, const void *buf, size_t len) {
    pthread_once(&g_crc32_once, vx_crc32_init_tab);
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = g_crc32_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static int vx_fdatasync_fd(int fd) {
#if defined(__linux__)
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

static vx_status_t vx_write_all_fd(int fd, const uint8_t *buf, size_t len) {
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

static inline size_t vx_seg_elem_size(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return sizeof(uint8_t);
    case VX_TYPE_I64:  return sizeof(int64_t);
    case VX_TYPE_U64:  return sizeof(uint64_t);
    case VX_TYPE_F64:  return sizeof(double);
    case VX_TYPE_STR:  return sizeof(vx_strview_t);
    }
    return 0;
}


static inline bool vx_col_is_valid(const vx_col_chunk_t *cc, uint32_t r) {
    return !cc->has_nulls || !cc->validity ||
           (((cc->validity[r >> 6] >> (r & 63u)) & 1ULL) != 0);
}

static inline uint32_t vx_next_pow2_u32(uint64_t x) {
    if (x <= 1ULL) return 1u;
    uint32_t p = 1u;
    while ((uint64_t)p < x && p < (1u << 31)) p <<= 1;
    return p;
}

/* Buffered sequential writer maintaining live file_crc32 and meta_crc32 */
typedef struct vx_wbuf {
    int      fd;
    uint64_t pos;
    uint32_t file_crc;
    uint32_t meta_crc;
    bool     hash_file;
    bool     hash_meta;
    size_t   used;
    uint8_t  buf[VX_SEG_IO_BUF_SZ];
} vx_wbuf_t;

static vx_status_t vx_wbuf_flush(vx_wbuf_t *wb) {
    if (wb->used == 0) return VX_OK;
    vx_status_t st = vx_write_all_fd(wb->fd, wb->buf, wb->used);
    wb->used = 0;
    return st;
}

static vx_status_t vx_wbuf_write(vx_wbuf_t *wb, const void *data, size_t len) {
    if (len == 0) return VX_OK;
    if (wb->hash_file) wb->file_crc = vx_crc32_update(wb->file_crc, data, len);
    if (wb->hash_meta) wb->meta_crc = vx_crc32_update(wb->meta_crc, data, len);

    const uint8_t *p = (const uint8_t *)data;
    wb->pos += (uint64_t)len;

    while (len > 0) {
        size_t space = sizeof(wb->buf) - wb->used;
        if (space == 0) {
            vx_status_t st = vx_wbuf_flush(wb);
            if (st != VX_OK) return st;
            space = sizeof(wb->buf);
        }
        size_t take = (len < space) ? len : space;
        memcpy(wb->buf + wb->used, p, take);
        wb->used += take;
        p        += take;
        len      -= take;
    }
    return VX_OK;
}

static vx_status_t vx_wbuf_pad64(vx_wbuf_t *wb) {
    static const uint8_t zeros[64] = { 0 };
    uint64_t rem = wb->pos & 63ULL;
    return rem ? vx_wbuf_write(wb, zeros, (size_t)(64ULL - rem)) : VX_OK;
}

/* ============================================================================
 * 2. Unlinked Temp Spool for Out-of-Line Arrow StringView Buffers
 * ============================================================================ */
typedef struct vx_str_spool {
    int       fd;
    uint32_t  nbufs;
    uint32_t  cap_bufs;
    uint64_t *buf_lens; /* exact byte length of each <= INT32_MAX buffer */
    size_t    w_used;
    uint8_t   w_buf[VX_SEG_IO_BUF_SZ];
} vx_str_spool_t;

static vx_status_t vx_spool_open(vx_str_spool_t *sp, const char *spill_dir) {
    memset(sp, 0, sizeof(*sp));
    sp->fd = -1;
    const char *dir = (spill_dir && spill_dir[0]) ? spill_dir : "/tmp";
    char        tmpl[512];
    int         n = snprintf(tmpl, sizeof(tmpl), "%s/vex_strspool_XXXXXX", dir);
    if (n <= 0 || (size_t)n >= sizeof(tmpl)) return VX_ERR_INVALID;

    sp->fd = mkstemp(tmpl);
    if (sp->fd < 0) return VX_ERR_IO;
    (void)unlink(tmpl); /* OS reclaims automatically on close or crash */
    return VX_OK;
}

static void vx_spool_close(vx_str_spool_t *sp) {
    if (sp->fd >= 0) close(sp->fd);
    free(sp->buf_lens);
    memset(sp, 0, sizeof(*sp));
    sp->fd = -1;
}

static vx_status_t vx_spool_flush(vx_str_spool_t *sp) {
    if (sp->w_used == 0) return VX_OK;
    vx_status_t st = vx_write_all_fd(sp->fd, sp->w_buf, sp->w_used);
    sp->w_used = 0;
    return st;
}

static vx_status_t vx_spool_append(vx_str_spool_t *sp, const uint8_t *bytes, int32_t len,
                                   int32_t *out_buf_idx, int32_t *out_offset) {
    if (sp->nbufs == 0 || sp->buf_lens[sp->nbufs - 1u] + (uint64_t)len > (uint64_t)INT32_MAX) {
        if (sp->nbufs == sp->cap_bufs) {
            uint32_t  ncap = sp->cap_bufs ? sp->cap_bufs * 2u : 4u;
            uint64_t *nl   = (uint64_t *)realloc(sp->buf_lens, (size_t)ncap * sizeof(*nl));
            if (!nl) return VX_ERR_OOM;
            sp->buf_lens = nl;
            sp->cap_bufs = ncap;
        }
        sp->buf_lens[sp->nbufs++] = 0;
    }

    uint32_t bi  = sp->nbufs - 1u;
    int32_t  off = (int32_t)sp->buf_lens[bi];
    sp->buf_lens[bi] += (uint64_t)len;

    size_t rem = (size_t)len;
    while (rem > 0) {
        size_t space = sizeof(sp->w_buf) - sp->w_used;
        if (space == 0) {
            vx_status_t st = vx_spool_flush(sp);
            if (st != VX_OK) return st;
            space = sizeof(sp->w_buf);
        }
        size_t take = (rem < space) ? rem : space;
        memcpy(sp->w_buf + sp->w_used, bytes, take);
        sp->w_used += take;
        bytes      += take;
        rem        -= take;
    }

    *out_buf_idx = (int32_t)bi;
    *out_offset  = off;
    return VX_OK;
}

/* ============================================================================
 * 3. Zone-Map Accumulators & Immutable Robin Hood PK Builder
 * ============================================================================ */
static void vx_zone_update_scalar(vx_zone_t *z, vx_type_t type, const void *val_ptr) {
    if (!z->has_bounds) {
        z->has_bounds = 1u;
        switch (type) {
        case VX_TYPE_BOOL: {
            uint64_t v = *(const uint8_t *)val_ptr ? 1ULL : 0ULL;
            z->min_val.u64 = z->max_val.u64 = v;
            break;
        }
        case VX_TYPE_I64:
            z->min_val.i64 = z->max_val.i64 = *(const int64_t *)val_ptr;
            break;
        case VX_TYPE_U64:
            z->min_val.u64 = z->max_val.u64 = *(const uint64_t *)val_ptr;
            break;
        case VX_TYPE_F64: {
            double d = *(const double *)val_ptr;
            if (isnan(d)) {
                /* Widen bounds on NaN so `!=` predicates never falsely prune this chunk */
                z->min_val.f64 = -(double)INFINITY;
                z->max_val.f64 =  (double)INFINITY;
            } else {
                z->min_val.f64 = z->max_val.f64 = d;
            }
            break;
        }
        case VX_TYPE_STR: {
            const vx_strview_t *sv = (const vx_strview_t *)val_ptr;
            uint8_t p4[4] = { 0, 0, 0, 0 };
            if (vx_strview_is_inline(*sv)) {
                int32_t m = (sv->inl.len < 4) ? sv->inl.len : 4;
                if (m > 0) memcpy(p4, sv->inl.data, (size_t)m);
            } else {
                memcpy(p4, sv->out.prefix, 4u);
            }
            memcpy(z->min_val.str4, p4, 4u);
            memcpy(z->max_val.str4, p4, 4u);
            break;
        }
        }
        return;
    }

    switch (type) {
    case VX_TYPE_BOOL: {
        uint64_t v = *(const uint8_t *)val_ptr ? 1ULL : 0ULL;
        if (v < z->min_val.u64) z->min_val.u64 = v;
        if (v > z->max_val.u64) z->max_val.u64 = v;
        break;
    }
    case VX_TYPE_I64: {
        int64_t v = *(const int64_t *)val_ptr;
        if (v < z->min_val.i64) z->min_val.i64 = v;
        if (v > z->max_val.i64) z->max_val.i64 = v;
        break;
    }
    case VX_TYPE_U64: {
        uint64_t v = *(const uint64_t *)val_ptr;
        if (v < z->min_val.u64) z->min_val.u64 = v;
        if (v > z->max_val.u64) z->max_val.u64 = v;
        break;
    }
    case VX_TYPE_F64: {
        double d = *(const double *)val_ptr;
        if (isnan(d)) {
            z->min_val.f64 = -(double)INFINITY;
            z->max_val.f64 =  (double)INFINITY;
        } else {
            if (d < z->min_val.f64) z->min_val.f64 = d;
            if (d > z->max_val.f64) z->max_val.f64 = d;
        }
        break;
    }
    case VX_TYPE_STR: {
        const vx_strview_t *sv = (const vx_strview_t *)val_ptr;
        uint8_t p4[4] = { 0, 0, 0, 0 };
        if (vx_strview_is_inline(*sv)) {
            int32_t m = (sv->inl.len < 4) ? sv->inl.len : 4;
            if (m > 0) memcpy(p4, sv->inl.data, (size_t)m);
        } else {
            memcpy(p4, sv->out.prefix, 4u);
        }
        if (memcmp(p4, z->min_val.str4, 4u) < 0) memcpy(z->min_val.str4, p4, 4u);
        if (memcmp(p4, z->max_val.str4, 4u) > 0) memcpy(z->max_val.str4, p4, 4u);
        break;
    }
    }
}

static vx_status_t vx_rh_insert(vx_seg_pk_slot_t *slots, uint32_t cap_mask,
                                uint32_t seed, uint64_t pk, uint32_t row_idx,
                                uint32_t *io_max_psl) {
    vx_seg_pk_slot_t cur = { .pk = pk, .row_idx = row_idx, .psl = 0 };
    uint32_t         pos = (uint32_t)vx_pk_hash64(pk, seed) & cap_mask;

    for (;;) {
        if (slots[pos].row_idx == VX_PK_SLOT_EMPTY) {
            slots[pos] = cur;
            if (cur.psl > *io_max_psl) *io_max_psl = cur.psl;
            return VX_OK;
        }
        if (slots[pos].pk == cur.pk) {
            return VX_ERR_UNIQUE;
        }
        if (cur.psl > slots[pos].psl) {
            vx_seg_pk_slot_t tmp = slots[pos];
            slots[pos] = cur;
            if (cur.psl > *io_max_psl) *io_max_psl = cur.psl;
            cur = tmp;
        }
        pos = (pos + 1u) & cap_mask;
        cur.psl++;
    }
}

/* ============================================================================
 * 4. Streaming .vx Segment Writer (vx_seg_write)
 * ============================================================================ */
vx_status_t vx_seg_write(const char *final_path, const vx_seg_write_cfg_t *cfg,
                         vx_seg_write_info_t *out_info) {
    if (!final_path || !cfg || cfg->ncols == 0 || !cfg->col_types) {
        return VX_ERR_INVALID;
    }

    const bool has_pk = (cfg->pk_col_idx != UINT32_MAX);
    if (has_pk && (cfg->pk_col_idx >= cfg->ncols ||
                   cfg->col_types[cfg->pk_col_idx] != VX_TYPE_U64)) {
        return VX_ERR_SCHEMA;
    }

    /* Pre-pass: exact live row count & [min_pk, max_pk] so header writes once at offset 0 */
    uint64_t total_rows = 0;
    uint64_t min_pk     = UINT64_MAX;
    uint64_t max_pk     = 0;
    bool     has_str    = false;

    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        if (cfg->col_types[c] == VX_TYPE_STR) has_str = true;
    }

    for (uint32_t ci = 0; ci < cfg->nchunks; ++ci) {
        const vx_chunk_t  *ch = cfg->chunks[ci];
        const vx_delmap_t *dm = cfg->delmaps ? cfg->delmaps[ci] : NULL;
        if (!ch || ch->nrows == 0) continue;

        if (!has_pk && (!dm || dm->live_count == ch->nrows)) {
            total_rows += ch->nrows;
        } else {
            const vx_col_chunk_t *pkc = has_pk ? &ch->cols[cfg->pk_col_idx] : NULL;
            const uint64_t       *pks = has_pk ? (const uint64_t *)pkc->data : NULL;
            for (uint32_t r = 0; r < ch->nrows; ++r) {
                if (vx_del_is_deleted(dm, r)) continue;
                total_rows++;
                if (has_pk) {
                    if (!vx_col_is_valid(pkc, r)) return VX_ERR_SCHEMA; /* PK cannot be NULL */
                    uint64_t pk = pks[r];
                    if (pk < min_pk) min_pk = pk;
                    if (pk > max_pk) max_pk = pk;
                }
            }
        }
    }

    if (total_rows >= (uint64_t)UINT32_MAX) return VX_ERR_OOM;
    if (total_rows == 0) { min_pk = 0; max_pk = 0; }

    const uint32_t out_nchunks = total_rows
        ? (uint32_t)((total_rows + VX_BATCH_CAP - 1ULL) / VX_BATCH_CAP) : 0u;

    /* Prepare <final_path>.tmp */
    size_t path_len = strlen(final_path);
    char  *tmp_path = (char *)malloc(path_len + 5u);
    if (!tmp_path) return VX_ERR_OOM;
    memcpy(tmp_path, final_path, path_len);
    memcpy(tmp_path + path_len, ".tmp", 5u);

    vx_status_t           st         = VX_OK;
    int                   fd         = -1;
    vx_str_spool_t        spool;
    bool                  spool_open = false;
    vx_seg_col_desc_t    *col_dir    = NULL;
    vx_seg_chunk_meta_t  *chunk_meta = NULL;
    vx_seg_strbuf_desc_t *str_dir    = NULL;
    uint8_t              *stage_buf  = NULL;
    uint64_t             *val_buf    = NULL;
    uint32_t             *bloom_blks = NULL;
    vx_seg_pk_slot_t     *rh_slots   = NULL;

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { st = VX_ERR_IO; goto cleanup; }

    if (has_str && total_rows > 0) {
        st = vx_spool_open(&spool, cfg->spill_dir);
        if (st != VX_OK) goto cleanup;
        spool_open = true;
    }

    col_dir = (vx_seg_col_desc_t *)calloc(cfg->ncols, sizeof(*col_dir));
    if (out_nchunks > 0) {
        chunk_meta = (vx_seg_chunk_meta_t *)calloc((size_t)cfg->ncols * out_nchunks,
                                                   sizeof(*chunk_meta));
        stage_buf  = (uint8_t *)malloc((size_t)VX_BATCH_CAP * sizeof(vx_strview_t));
        val_buf    = (uint64_t *)malloc((size_t)out_nchunks * VX_BITMAP_WORDS * sizeof(uint64_t));
    }
    if (!col_dir || (out_nchunks > 0 && (!chunk_meta || !stage_buf || !val_buf))) {
        st = VX_ERR_OOM;
        goto cleanup;
    }

    vx_wbuf_t wb;
    memset(&wb, 0, sizeof(wb));
    wb.fd        = fd;
    wb.hash_file = true;
    wb.hash_meta = false;

    /* --- Section 1: 64-byte Header --- */
    vx_seg_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, VX_SEG_MAGIC, 8u);
    hdr.version    = VX_SEG_VERSION;
    hdr.flags      = (has_pk && total_rows > 0) ? VX_SEG_FLAG_PK : 0u;
    hdr.table_id   = cfg->table_id;
    hdr.ncols      = cfg->ncols;
    hdr.segment_id = cfg->segment_id;
    hdr.nrows      = total_rows;
    hdr.lsn_max    = cfg->lsn_max;
    hdr.min_pk     = min_pk;
    hdr.max_pk     = max_pk;

    st = vx_wbuf_write(&wb, &hdr, sizeof(hdr));
    if (st != VX_OK) goto cleanup;
    /* meta_crc32 begins with header[0..64) */
    wb.meta_crc = vx_crc32_update(0u, &hdr, sizeof(hdr));

    /* --- Section 2: Column Data Pages + Validity Bitmaps --- */
    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        const vx_type_t type    = cfg->col_types[c];
        const size_t    elem_sz = vx_seg_elem_size(type);

        vx_seg_col_desc_t *cd = &col_dir[c];
        cd->col_id   = c;
        cd->type     = (uint8_t)type;
        cd->encoding = VX_ENC_PLAIN;

        if (total_rows == 0) continue;

        st = vx_wbuf_pad64(&wb);
        if (st != VX_OK) goto cleanup;
        cd->data_off = wb.pos;

        memset(val_buf, 0, (size_t)out_nchunks * VX_BITMAP_WORDS * sizeof(uint64_t));

        uint32_t  out_ci       = 0;
        uint32_t  out_ri       = 0;
        uint32_t  col_data_crc = 0;
        uint64_t  col_nulls    = 0;
        vx_zone_t ch_zone;
        memset(&ch_zone, 0, sizeof(ch_zone));

        for (uint32_t ci = 0; ci < cfg->nchunks; ++ci) {
            const vx_chunk_t  *ch = cfg->chunks[ci];
            const vx_delmap_t *dm = cfg->delmaps ? cfg->delmaps[ci] : NULL;
            if (!ch || ch->nrows == 0) continue;

            const vx_col_chunk_t *cc = &ch->cols[c];
            for (uint32_t r = 0; r < ch->nrows; ++r) {
                if (vx_del_is_deleted(dm, r)) continue;

                uint64_t *cval = val_buf + (size_t)out_ci * VX_BITMAP_WORDS;
                if (!vx_col_is_valid(cc, r)) {
                    memset(stage_buf + (size_t)out_ri * elem_sz, 0, elem_sz);
                    ch_zone.null_count++;
                    cd->seg_zone.null_count++;
                    col_nulls++;
                } else {
                    cval[out_ri >> 6] |= (1ULL << (out_ri & 63u));
                    if (type == VX_TYPE_STR) {
                        vx_strview_t sv = ((const vx_strview_t *)cc->data)[r];
                        if (!vx_strview_is_inline(sv)) {
                            const uint8_t *p = vx_strview_ptr(&sv, cc->strpool);
                            int32_t bidx = 0, boff = 0;
                            st = vx_spool_append(&spool, p, sv.inl.len, &bidx, &boff);
                            if (st != VX_OK) goto cleanup;
                            sv.out.buf_idx = bidx;
                            sv.out.offset  = boff;
                        }
                        ((vx_strview_t *)stage_buf)[out_ri] = sv;
                        vx_zone_update_scalar(&ch_zone, type, &sv);
                        vx_zone_update_scalar(&cd->seg_zone, type, &sv);
                    } else {
                        const uint8_t *src_p = (const uint8_t *)cc->data + (size_t)r * elem_sz;
                        memcpy(stage_buf + (size_t)out_ri * elem_sz, src_p, elem_sz);
                        vx_zone_update_scalar(&ch_zone, type, src_p);
                        vx_zone_update_scalar(&cd->seg_zone, type, src_p);
                    }
                }

                out_ri++;
                if (out_ri == VX_BATCH_CAP) {
                    size_t               bytes = (size_t)out_ri * elem_sz;
                    vx_seg_chunk_meta_t *cm    = &chunk_meta[(size_t)c * out_nchunks + out_ci];
                    cm->zone       = ch_zone;
                    cm->page_off   = wb.pos - cd->data_off;
                    cm->page_len   = (uint32_t)bytes;
                    cm->page_crc32 = vx_crc32_update(0u, stage_buf, bytes);
                    col_data_crc   = vx_crc32_update(col_data_crc, stage_buf, bytes);

                    st = vx_wbuf_write(&wb, stage_buf, bytes);
                    if (st != VX_OK) goto cleanup;

                    out_ci++;
                    out_ri = 0;
                    memset(&ch_zone, 0, sizeof(ch_zone));
                }
            }
        }

        /* Flush final partial chunk */
        if (out_ri > 0) {
            size_t               bytes = (size_t)out_ri * elem_sz;
            vx_seg_chunk_meta_t *cm    = &chunk_meta[(size_t)c * out_nchunks + out_ci];
            cm->zone       = ch_zone;
            cm->page_off   = wb.pos - cd->data_off;
            cm->page_len   = (uint32_t)bytes;
            cm->page_crc32 = vx_crc32_update(0u, stage_buf, bytes);
            col_data_crc   = vx_crc32_update(col_data_crc, stage_buf, bytes);

            st = vx_wbuf_write(&wb, stage_buf, bytes);
            if (st != VX_OK) goto cleanup;
            out_ci++;
        }

        cd->data_len = wb.pos - cd->data_off;

        if (col_nulls > 0) {
            st = vx_wbuf_pad64(&wb);
            if (st != VX_OK) goto cleanup;
            size_t val_bytes = (size_t)out_nchunks * VX_BITMAP_WORDS * sizeof(uint64_t);
            cd->has_nulls    = 1u;
            cd->validity_off = wb.pos;
            cd->validity_len = (uint64_t)val_bytes;
            col_data_crc     = vx_crc32_update(col_data_crc, val_buf, val_bytes);

            st = vx_wbuf_write(&wb, val_buf, val_bytes);
            if (st != VX_OK) goto cleanup;
        }
        cd->data_crc32 = col_data_crc;
    }

    /* --- Section 3: String Data Buffers (copied sequentially from unlinked spool) --- */
    uint32_t n_strbufs = 0;
    if (spool_open && spool.nbufs > 0) {
        st = vx_spool_flush(&spool);
        if (st != VX_OK) goto cleanup;
        if (lseek(spool.fd, 0, SEEK_SET) != 0) { st = VX_ERR_IO; goto cleanup; }

        n_strbufs = spool.nbufs;
        str_dir   = (vx_seg_strbuf_desc_t *)calloc(n_strbufs, sizeof(*str_dir));
        if (!str_dir) { st = VX_ERR_OOM; goto cleanup; }

        uint8_t copy_buf[VX_SEG_IO_BUF_SZ];
        for (uint32_t b = 0; b < n_strbufs; ++b) {
            st = vx_wbuf_pad64(&wb);
            if (st != VX_OK) goto cleanup;

            str_dir[b].file_off = wb.pos;
            str_dir[b].byte_len = spool.buf_lens[b];

            uint64_t rem = spool.buf_lens[b];
            while (rem > 0) {
                size_t  want = (rem < sizeof(copy_buf)) ? (size_t)rem : sizeof(copy_buf);
                ssize_t got  = read(spool.fd, copy_buf, want);
                if (got < 0) {
                    if (errno == EINTR) continue;
                    st = VX_ERR_IO;
                    goto cleanup;
                }
                if (got == 0) { st = VX_ERR_IO; goto cleanup; }
                st = vx_wbuf_write(&wb, copy_buf, (size_t)got);
                if (st != VX_OK) goto cleanup;
                rem -= (uint64_t)got;
            }
        }
        vx_spool_close(&spool);
        spool_open = false;
    }

    /* --- Section 4: Per-Segment PK Index (Split-Block Bloom + Robin Hood Table) --- */
    vx_seg_pk_hdr_t pk_hdr;
    memset(&pk_hdr, 0, sizeof(pk_hdr));

    if (has_pk && total_rows > 0) {
        uint64_t need_blks = (total_rows * 10ULL + 255ULL) / 256ULL;
        uint32_t nblocks   = vx_next_pow2_u32(need_blks ? need_blks : 1ULL);
        uint64_t need_rh   = (total_rows * 10ULL + 6ULL) / 7ULL;
        uint32_t rh_cap    = vx_next_pow2_u32(need_rh < 16ULL ? 16ULL : need_rh);
        uint32_t seed      = (uint32_t)(cfg->segment_id ^ 0x9e3779b9ULL) | 1u;

        bloom_blks = (uint32_t *)calloc((size_t)nblocks * 8u, sizeof(uint32_t));
        rh_slots   = (vx_seg_pk_slot_t *)malloc((size_t)rh_cap * sizeof(*rh_slots));
        if (!bloom_blks || !rh_slots) { st = VX_ERR_OOM; goto cleanup; }

        for (uint32_t s = 0; s < rh_cap; ++s) {
            rh_slots[s].pk      = 0;
            rh_slots[s].row_idx = VX_PK_SLOT_EMPTY;
            rh_slots[s].psl     = 0;
        }

        uint32_t max_psl = 0;
        uint32_t seg_row = 0;
        for (uint32_t ci = 0; ci < cfg->nchunks; ++ci) {
            const vx_chunk_t  *ch = cfg->chunks[ci];
            const vx_delmap_t *dm = cfg->delmaps ? cfg->delmaps[ci] : NULL;
            if (!ch || ch->nrows == 0) continue;

            const uint64_t *pks = (const uint64_t *)ch->cols[cfg->pk_col_idx].data;
            for (uint32_t r = 0; r < ch->nrows; ++r) {
                if (vx_del_is_deleted(dm, r)) continue;
                uint64_t pk = pks[r];
                vx_bloom_insert(bloom_blks, nblocks - 1u, seed, pk);
                st = vx_rh_insert(rh_slots, rh_cap - 1u, seed, pk, seg_row, &max_psl);
                if (st != VX_OK) goto cleanup;
                seg_row++;
            }
        }

        st = vx_wbuf_pad64(&wb);
        if (st != VX_OK) goto cleanup;
        pk_hdr.bloom_off     = wb.pos;
        pk_hdr.bloom_nblocks = nblocks;
        pk_hdr.bloom_seed    = seed;
        st = vx_wbuf_write(&wb, bloom_blks, (size_t)nblocks * 32u);
        if (st != VX_OK) goto cleanup;

        st = vx_wbuf_pad64(&wb);
        if (st != VX_OK) goto cleanup;
        pk_hdr.rh_off     = wb.pos;
        pk_hdr.rh_cap     = rh_cap;
        pk_hdr.rh_max_psl = max_psl;
        st = vx_wbuf_write(&wb, rh_slots, (size_t)rh_cap * sizeof(*rh_slots));
        if (st != VX_OK) goto cleanup;

        free(bloom_blks); bloom_blks = NULL;
        free(rh_slots);   rh_slots   = NULL;
    }

    /* --- Section 5: Metadata Block (all bytes covered by footer.meta_crc32) --- */
    st = vx_wbuf_pad64(&wb);
    if (st != VX_OK) goto cleanup;

    const uint64_t meta_start_off = wb.pos;
    wb.hash_meta = true;

    const uint64_t col_dir_off     = wb.pos;
    const uint64_t chunk_meta_base = col_dir_off + (uint64_t)cfg->ncols * sizeof(vx_seg_col_desc_t);
    for (uint32_t c = 0; c < cfg->ncols; ++c) {
        col_dir[c].chunk_meta_off = out_nchunks
            ? (chunk_meta_base + (uint64_t)c * out_nchunks * sizeof(vx_seg_chunk_meta_t)) : 0ULL;
    }

    st = vx_wbuf_write(&wb, col_dir, (size_t)cfg->ncols * sizeof(*col_dir));
    if (st != VX_OK) goto cleanup;

    if (out_nchunks > 0) {
        st = vx_wbuf_write(&wb, chunk_meta,
                           (size_t)cfg->ncols * out_nchunks * sizeof(*chunk_meta));
        if (st != VX_OK) goto cleanup;
    }

    uint64_t strbuf_dir_off = 0;
    if (n_strbufs > 0) {
        strbuf_dir_off = wb.pos;
        st = vx_wbuf_write(&wb, str_dir, (size_t)n_strbufs * sizeof(*str_dir));
        if (st != VX_OK) goto cleanup;
    }

    uint64_t pk_hdr_off = 0;
    if (has_pk && total_rows > 0) {
        pk_hdr_off = wb.pos;
        st = vx_wbuf_write(&wb, &pk_hdr, sizeof(pk_hdr));
        if (st != VX_OK) goto cleanup;
    }

    /* --- Section 6: 64-byte Footer (meta_crc32 at +48, file_crc32 at +52) --- */
    vx_seg_footer_t foot;
    memset(&foot, 0, sizeof(foot));
    foot.col_dir_off    = col_dir_off;
    foot.strbuf_dir_off = strbuf_dir_off;
    foot.pk_hdr_off     = pk_hdr_off;
    foot.meta_start_off = meta_start_off;
    foot.file_bytes     = wb.pos + sizeof(foot);
    foot.nchunks        = out_nchunks;
    foot.n_strbufs      = n_strbufs;
    memcpy(foot.magic, VX_SEG_MAGIC, 8u);

    /* Write bytes [0..48) -> updates both file_crc and meta_crc */
    st = vx_wbuf_write(&wb, &foot, 48u);
    if (st != VX_OK) goto cleanup;

    foot.meta_crc32 = wb.meta_crc;
    wb.hash_meta    = false;

    /* Write meta_crc32 [48..52) -> updates file_crc */
    st = vx_wbuf_write(&wb, &foot.meta_crc32, sizeof(foot.meta_crc32));
    if (st != VX_OK) goto cleanup;

    foot.file_crc32 = wb.file_crc;
    wb.hash_file    = false;

    /* Write file_crc32 + magic [52..64) */
    st = vx_wbuf_write(&wb, &foot.file_crc32, 12u);
    if (st != VX_OK) goto cleanup;

    st = vx_wbuf_flush(&wb);
    if (st != VX_OK) goto cleanup;

    if (vx_fdatasync_fd(fd) != 0) { st = VX_ERR_IO; goto cleanup; }
    if (close(fd) != 0) { fd = -1; st = VX_ERR_IO; goto cleanup; }
    fd = -1;

    VX_FAILPOINT("seg_pre_rename");
    if (rename(tmp_path, final_path) != 0) { st = VX_ERR_IO; goto cleanup; }

    if (out_info) {
        out_info->nrows      = total_rows;
        out_info->file_bytes = foot.file_bytes;
        out_info->min_pk     = min_pk;
        out_info->max_pk     = max_pk;
        out_info->flags      = hdr.flags;
        out_info->nchunks    = out_nchunks;
    }

cleanup:
    if (fd >= 0) close(fd);
    if (st != VX_OK && tmp_path) (void)unlink(tmp_path);
    if (spool_open) vx_spool_close(&spool);
    free(tmp_path);
    free(col_dir);
    free(chunk_meta);
    free(str_dir);
    free(stage_buf);
    free(val_buf);
    free(bloom_blks);
    free(rh_slots);
    return st;
}

/* ============================================================================
 * 5. Zero-Deserialization mmap Reader (vx_seg_open / verify / destroy)
 * ============================================================================ */
static inline bool vx_range_in(uint64_t off, uint64_t len, uint64_t lo, uint64_t hi) {
    return off >= lo && off <= hi && len <= (hi - off);
}

static inline bool vx_mul_u64_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) return false;
    *out = a * b;
    return true;
}

static inline bool vx_is_pow2_u32(uint32_t x) {
    return x != 0u && (x & (x - 1u)) == 0u;
}

static inline void vx_madvise_aligned(const void *ptr, size_t len, int advice) {
#if defined(MADV_RANDOM)
    if (!ptr || len == 0) return;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return;
    uintptr_t mask = (uintptr_t)ps - 1u;
    uintptr_t base = (uintptr_t)ptr & ~mask;
    size_t    adj  = len + (size_t)((uintptr_t)ptr - base);
    (void)madvise((void *)base, adj, advice);
#else
    (void)ptr; (void)len; (void)advice;
#endif
}

/* Chunk views live inside vx_seg_t's slab; backing (&seg->hdr) owns lifetime. */
static void vx_seg_chunk_view_noop(vx_owner_t *o) { (void)o; }

static void vx_seg_destroy(vx_owner_t *owner) {
    vx_seg_t *seg = (vx_seg_t *)owner;
    if (!seg) return;

    if (seg->chunk_views) {
        if (seg->nchunks > 0) {
            free(seg->chunk_views[0]->cols); /* contiguous slab of nchunks * ncols */
            free(seg->chunk_views[0]);       /* contiguous slab of nchunks vx_chunk_t */
        }
        free(seg->chunk_views);
    }

    /* strpool points directly into mmap_base; owners is NULL */
    free((void *)seg->strpool.ptrs);
    free(seg->strpool.lens);

    if (seg->mmap_base && seg->mmap_base != MAP_FAILED && seg->mmap_len > 0) {
        (void)munmap((void *)seg->mmap_base, seg->mmap_len);
    }
    if (seg->fd >= 0) close(seg->fd);

    if (seg->file_path) {
        if (atomic_load_explicit(&seg->unlink_on_destroy, memory_order_acquire)) {
            (void)unlink(seg->file_path);
        }
        free(seg->file_path);
    }
    free(seg);
}

vx_status_t vx_seg_open(const char *path, vx_seg_t **out_seg) {
    if (!path || !out_seg) return VX_ERR_INVALID;
    *out_seg = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return VX_ERR_IO;

    struct stat st_buf;
    if (fstat(fd, &st_buf) != 0) { close(fd); return VX_ERR_IO; }

    const uint64_t file_size = (uint64_t)st_buf.st_size;
    if (st_buf.st_size < 0 || file_size < sizeof(vx_seg_hdr_t) + sizeof(vx_seg_footer_t)) {
        close(fd);
        return VX_ERR_CORRUPT;
    }

    void *mapped = mmap(NULL, (size_t)file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) { close(fd); return VX_ERR_IO; }

    vx_seg_t *seg = (vx_seg_t *)calloc(1u, sizeof(*seg));
    if (!seg) {
        munmap(mapped, (size_t)file_size);
        close(fd);
        return VX_ERR_OOM;
    }
    vx_owner_init(&seg->hdr, VX_OWN_SEGMENT, vx_seg_destroy);
    atomic_init(&seg->unlink_on_destroy, false);
    seg->fd        = fd;
    seg->mmap_base = (const uint8_t *)mapped;
    seg->mmap_len  = (size_t)file_size;

    size_t plen = strlen(path);
    seg->file_path = (char *)malloc(plen + 1u);
    if (!seg->file_path) { vx_owner_release(&seg->hdr); return VX_ERR_OOM; }
    memcpy(seg->file_path, path, plen + 1u);

    /* --- Step 1: Validate Header, Footer & meta_crc32 --- */
    const uint64_t         footer_off = file_size - sizeof(vx_seg_footer_t);
    const vx_seg_hdr_t    *hdr        = (const vx_seg_hdr_t *)seg->mmap_base;
    const vx_seg_footer_t *foot       = (const vx_seg_footer_t *)(seg->mmap_base + footer_off);

    if (memcmp(hdr->magic, VX_SEG_MAGIC, 8u) != 0 ||
        memcmp(foot->magic, VX_SEG_MAGIC, 8u) != 0 ||
        hdr->version != VX_SEG_VERSION ||
        hdr->ncols == 0 ||
        hdr->nrows >= (uint64_t)UINT32_MAX ||
        foot->file_bytes != file_size) {
        vx_owner_release(&seg->hdr);
        return VX_ERR_CORRUPT;
    }

    const uint32_t expected_nchunks = hdr->nrows
        ? (uint32_t)((hdr->nrows + VX_BATCH_CAP - 1ULL) / VX_BATCH_CAP) : 0u;
    if (foot->nchunks != expected_nchunks ||
        foot->meta_start_off < sizeof(vx_seg_hdr_t) ||
        foot->meta_start_off > footer_off ||
        (foot->meta_start_off & 63ULL) != 0ULL) {
        vx_owner_release(&seg->hdr);
        return VX_ERR_CORRUPT;
    }

    /* Fast startup checksum: header[0..64) || metadata[meta_start_off .. footer_off + 48) */
    uint32_t mcrc = vx_crc32_update(0u, seg->mmap_base, sizeof(vx_seg_hdr_t));
    mcrc = vx_crc32_update(mcrc, seg->mmap_base + foot->meta_start_off,
                           (size_t)((footer_off + 48ULL) - foot->meta_start_off));
    if (mcrc != foot->meta_crc32) {
        vx_owner_release(&seg->hdr);
        return VX_ERR_CORRUPT;
    }

    seg->file_hdr = hdr;
    seg->footer   = foot;
    seg->nchunks  = foot->nchunks;

    /* --- Step 2: Validate Column Directory, Chunk Meta & Data Bounds --- */
    uint64_t col_dir_bytes = 0;
    if (!vx_mul_u64_ok(hdr->ncols, sizeof(vx_seg_col_desc_t), &col_dir_bytes) ||
        !vx_range_in(foot->col_dir_off, col_dir_bytes, foot->meta_start_off, footer_off)) {
        vx_owner_release(&seg->hdr);
        return VX_ERR_CORRUPT;
    }
    seg->col_dir = (const vx_seg_col_desc_t *)(seg->mmap_base + foot->col_dir_off);

    for (uint32_t c = 0; c < hdr->ncols; ++c) {
        const vx_seg_col_desc_t *cd = &seg->col_dir[c];
        if (cd->type > VX_TYPE_STR || cd->encoding > VX_ENC_RLE) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_CORRUPT;
        }
        if (hdr->nrows == 0) continue;

        if ((cd->data_off & 63ULL) != 0ULL ||
            !vx_range_in(cd->data_off, cd->data_len, sizeof(vx_seg_hdr_t), foot->meta_start_off)) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_CORRUPT;
        }
        if (cd->encoding == VX_ENC_PLAIN) {
            uint64_t need_bytes = 0;
            if (!vx_mul_u64_ok(hdr->nrows, vx_seg_elem_size((vx_type_t)cd->type), &need_bytes) ||
                cd->data_len != need_bytes) {
                vx_owner_release(&seg->hdr);
                return VX_ERR_CORRUPT;
            }
        }
        if (cd->has_nulls) {
            uint64_t need_val = (uint64_t)foot->nchunks * VX_BITMAP_WORDS * sizeof(uint64_t);
            if ((cd->validity_off & 63ULL) != 0ULL ||
                cd->validity_len != need_val ||
                !vx_range_in(cd->validity_off, cd->validity_len,
                             sizeof(vx_seg_hdr_t), foot->meta_start_off)) {
                vx_owner_release(&seg->hdr);
                return VX_ERR_CORRUPT;
            }
        }
        uint64_t cm_bytes = 0;
        if (!vx_mul_u64_ok(foot->nchunks, sizeof(vx_seg_chunk_meta_t), &cm_bytes) ||
            !vx_range_in(cd->chunk_meta_off, cm_bytes, foot->meta_start_off, footer_off)) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_CORRUPT;
        }
    }

    /* --- Step 3: Bind Arrow StringView Buffer Table directly into mmap_base --- */
    if (foot->n_strbufs > 0) {
        uint64_t sd_bytes = 0;
        if (!vx_mul_u64_ok(foot->n_strbufs, sizeof(vx_seg_strbuf_desc_t), &sd_bytes) ||
            !vx_range_in(foot->strbuf_dir_off, sd_bytes, foot->meta_start_off, footer_off)) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_CORRUPT;
        }
        const vx_seg_strbuf_desc_t *sd =
            (const vx_seg_strbuf_desc_t *)(seg->mmap_base + foot->strbuf_dir_off);

        seg->strpool.ptrs = (const uint8_t **)calloc(foot->n_strbufs, sizeof(uint8_t *));
        seg->strpool.lens = (int64_t *)calloc(foot->n_strbufs, sizeof(int64_t));
        if (!seg->strpool.ptrs || !seg->strpool.lens) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_OOM;
        }
        seg->strpool.nbufs  = foot->n_strbufs;
        seg->strpool.cap    = foot->n_strbufs;
        seg->strpool.owners = NULL; /* pinned by chunk->backing == &seg->hdr */

        for (uint32_t b = 0; b < foot->n_strbufs; ++b) {
            if ((sd[b].file_off & 63ULL) != 0ULL ||
                sd[b].byte_len > (uint64_t)INT32_MAX ||
                !vx_range_in(sd[b].file_off, sd[b].byte_len,
                             sizeof(vx_seg_hdr_t), foot->meta_start_off)) {
                vx_owner_release(&seg->hdr);
                return VX_ERR_CORRUPT;
            }
            seg->strpool.ptrs[b] = seg->mmap_base + sd[b].file_off;
            seg->strpool.lens[b] = (int64_t)sd[b].byte_len;
        }
    }

    /* --- Step 4: Bind Per-Segment PK Bloom Filter + Immutable Robin Hood Index --- */
    if (hdr->flags & VX_SEG_FLAG_PK) {
        if (!vx_range_in(foot->pk_hdr_off, sizeof(vx_seg_pk_hdr_t),
                         foot->meta_start_off, footer_off)) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_CORRUPT;
        }
        const vx_seg_pk_hdr_t *pkh = (const vx_seg_pk_hdr_t *)(seg->mmap_base + foot->pk_hdr_off);
        uint64_t bloom_bytes = 0, rh_bytes = 0;

        if (!vx_is_pow2_u32(pkh->bloom_nblocks) || !vx_is_pow2_u32(pkh->rh_cap) ||
            pkh->rh_max_psl >= pkh->rh_cap ||
            (pkh->bloom_off & 63ULL) != 0ULL || (pkh->rh_off & 63ULL) != 0ULL ||
            !vx_mul_u64_ok(pkh->bloom_nblocks, 32ULL, &bloom_bytes) ||
            !vx_mul_u64_ok(pkh->rh_cap, sizeof(vx_seg_pk_slot_t), &rh_bytes) ||
            !vx_range_in(pkh->bloom_off, bloom_bytes, sizeof(vx_seg_hdr_t), foot->meta_start_off) ||
            !vx_range_in(pkh->rh_off, rh_bytes, sizeof(vx_seg_hdr_t), foot->meta_start_off)) {
            vx_owner_release(&seg->hdr);
            return VX_ERR_CORRUPT;
        }

        seg->pk_hdr       = pkh;
        seg->bloom_blocks = (const uint32_t *)(seg->mmap_base + pkh->bloom_off);
        seg->pk_slots     = (const vx_seg_pk_slot_t *)(seg->mmap_base + pkh->rh_off);

#if defined(MADV_RANDOM)
        vx_madvise_aligned(seg->bloom_blocks, (size_t)bloom_bytes, MADV_RANDOM);
        vx_madvise_aligned(seg->pk_slots, (size_t)rh_bytes, MADV_RANDOM);
#endif
    }

    /* --- Step 5: Build Lightweight vx_chunk_t Views for M12 VX_SOURCE_TABLE_SCAN --- */
    if (foot->nchunks > 0) {
        seg->chunk_views   = (vx_chunk_t **)calloc(foot->nchunks, sizeof(vx_chunk_t *));
        vx_chunk_t     *cs = (vx_chunk_t *)calloc(foot->nchunks, sizeof(vx_chunk_t));
        vx_col_chunk_t *cc = (vx_col_chunk_t *)calloc((size_t)foot->nchunks * hdr->ncols,
                                                      sizeof(vx_col_chunk_t));
        if (!seg->chunk_views || !cs || !cc) {
            free(cc);
            free(cs);
            free(seg->chunk_views);
            seg->chunk_views = NULL;
            vx_owner_release(&seg->hdr);
            return VX_ERR_OOM;
        }

        for (uint32_t k = 0; k < foot->nchunks; ++k) {
            vx_chunk_t *cv = &cs[k];
            vx_owner_init(&cv->hdr, VX_OWN_CHUNK, vx_seg_chunk_view_noop);
            cv->chunk_id    = (hdr->segment_id << 32) | (uint64_t)k;
            cv->row_id_base = (uint64_t)k * VX_BATCH_CAP;
            uint64_t rem    = hdr->nrows - cv->row_id_base;
            cv->nrows       = (rem > VX_BATCH_CAP) ? VX_BATCH_CAP : (uint32_t)rem;
            cv->ncols       = hdr->ncols;
            cv->tier        = VX_TIER_COLD_MMAP;
            cv->backing     = &seg->hdr; /* non-owning ptr; catalog/scan pins &seg->hdr */
            cv->cols        = &cc[(size_t)k * hdr->ncols];

            for (uint32_t c = 0; c < hdr->ncols; ++c) {
                const vx_seg_col_desc_t   *cd = &seg->col_dir[c];
                const vx_seg_chunk_meta_t *cm =
                    (const vx_seg_chunk_meta_t *)(seg->mmap_base + cd->chunk_meta_off) + k;

                if (!vx_range_in(cm->page_off, cm->page_len, 0ULL, cd->data_len)) {
                    seg->chunk_views[0] = cs; /* ensure vx_seg_destroy frees slabs cleanly */
                    cs[0].cols          = cc;
                    vx_owner_release(&seg->hdr);
                    return VX_ERR_CORRUPT;
                }

                vx_col_chunk_t *col = &cv->cols[c];
                col->type      = (vx_type_t)cd->type;
                col->encoding  = (vx_encoding_t)cd->encoding;
                col->has_nulls = cd->has_nulls;
                col->data      = seg->mmap_base + cd->data_off + cm->page_off;
                col->validity  = cd->has_nulls
                    ? (const uint64_t *)(seg->mmap_base + cd->validity_off +
                                         (size_t)k * VX_BITMAP_WORDS * sizeof(uint64_t))
                    : NULL;
                col->strpool   = (col->type == VX_TYPE_STR && seg->strpool.nbufs > 0)
                    ? &seg->strpool : NULL;
                col->zone      = cm->zone;
            }
            seg->chunk_views[k] = cv;
        }
    }

    *out_seg = seg;
    return VX_OK;
}

vx_status_t vx_seg_verify_full_crc(const vx_seg_t *seg) {
    if (!seg || !seg->mmap_base || !seg->footer) return VX_ERR_INVALID;
    const size_t crc_len = seg->mmap_len - 12u; /* file[0 .. footer_off + 52) */
    uint32_t     calc    = vx_crc32_update(0u, seg->mmap_base, crc_len);
    return (calc == seg->footer->file_crc32) ? VX_OK : VX_ERR_CORRUPT;
}
