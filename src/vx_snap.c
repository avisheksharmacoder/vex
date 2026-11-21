#include "vx_snap.h"
#include "vx_internal.h"
#include "vx_wal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VX_SNAP_HDR_SIZE 48U
#define VX_SNAP_VERSION  1U

static const uint8_t k_snap_magic[8] = { 'V', 'X', 'S', 'N', 'A', 'P', '0', '1' };

static inline void sp_w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}
static inline void sp_w32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xFFU);
}
static inline void sp_w64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xFFU);
}
static inline uint16_t sp_r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t sp_r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t sp_r64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i] << (i * 8));
    return v;
}

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} snap_buf_t;

static bool sb_reserve(snap_buf_t *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) return false;
    size_t need = b->len + extra;
    if (need <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 4096U;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2U) { ncap = need; break; }
        ncap *= 2U;
    }
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) return false;
    b->data = nd;
    b->cap = ncap;
    return true;
}

static bool sb_bytes(snap_buf_t *b, const void *src, size_t n) {
    if (!sb_reserve(b, n)) return false;
    if (n > 0) memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

static bool sb_u8(snap_buf_t *b, uint8_t v) { return sb_bytes(b, &v, 1); }

static bool sb_u16(snap_buf_t *b, uint16_t v) {
    uint8_t tmp[2]; sp_w16(tmp, v); return sb_bytes(b, tmp, 2);
}

static bool sb_u32(snap_buf_t *b, uint32_t v) {
    uint8_t tmp[4]; sp_w32(tmp, v); return sb_bytes(b, tmp, 4);
}

static bool sb_u64(snap_buf_t *b, uint64_t v) {
    uint8_t tmp[8]; sp_w64(tmp, v); return sb_bytes(b, tmp, 8);
}

static bool sb_str16(snap_buf_t *b, const char *s) {
    size_t n = strlen(s);
    if (n > UINT16_MAX) return false;
    return sb_u16(b, (uint16_t)n) && sb_bytes(b, s, n);
}

static bool write_all_fd(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static bool read_all_fd(int fd, uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += (size_t)n;
    }
    return true;
}

static void fsync_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    char dir[512];
    if (!slash) {
        snprintf(dir, sizeof(dir), ".");
    } else if (slash == path) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        size_t dlen = (size_t)(slash - path);
        if (dlen >= sizeof(dir)) return;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
    }
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
}

bool vx_snap_write(const char *snap_path, const vx_catalog_t *cat, uint64_t checkpoint_lsn) {
    if (!snap_path || !cat) return false;

    snap_buf_t b = {0};
    if (!sb_reserve(&b, VX_SNAP_HDR_SIZE)) return false;
    memset(b.data, 0, VX_SNAP_HDR_SIZE);
    b.len = VX_SNAP_HDR_SIZE;

    uint64_t grand_total_rows = 0;

    for (uint32_t ti = 0; ti < cat->ntables; ti++) {
        const vx_table_t *t = cat->tables[ti];

        uint64_t live_rows = 0;
        for (uint32_t ci = 0; ci < t->nchunks; ci++) {
            uint32_t nr = t->chunks[ci]->chunk->nrows;
            uint32_t dl = t->delmaps ? vx_delmap_count(t->delmaps[ci]) : 0;
            if (nr > dl) live_rows += (uint64_t)(nr - dl);
        }
        grand_total_rows += live_rows;

        if (!sb_str16(&b, t->name) ||
            !sb_u32(&b, t->table_id) ||
            !sb_u32(&b, t->ncols) ||
            !sb_u32(&b, (uint32_t)t->pk_col) ||
            !sb_u64(&b, live_rows)) {
            free(b.data);
            return false;
        }

        for (uint32_t c = 0; c < t->ncols; c++) {
            const vx_col_schema_t *cs = &t->cols[c];
            uint8_t flags = (cs->is_key ? 1U : 0U) | (cs->has_fk ? 2U : 0U);
            if (!sb_str16(&b, cs->name) ||
                !sb_u8(&b, (uint8_t)cs->type) ||
                !sb_u8(&b, flags)) {
                free(b.data);
                return false;
            }
            if (cs->has_fk) {
                if (!sb_str16(&b, cs->fk_table) || !sb_str16(&b, cs->fk_col)) {
                    free(b.data);
                    return false;
                }
            }
        }

        if (!sb_u32(&b, t->nindexes)) { free(b.data); return false; }
        for (uint32_t ix = 0; ix < t->nindexes; ix++) {
            const vx_index_def_t *id = &t->indexes[ix];
            if (!sb_u8(&b, id->is_unique ? 1U : 0U) || !sb_u32(&b, id->ncols)) {
                free(b.data); return false;
            }
            for (uint32_t k = 0; k < id->ncols; k++) {
                if (!sb_u32(&b, id->col_idxs[k])) { free(b.data); return false; }
            }
        }

        /* Stream only live rows (compacts all tombstones out of the snapshot) */
        for (uint32_t ci = 0; ci < t->nchunks; ci++) {
            const vx_chunk_t *ch = t->chunks[ci]->chunk;
            const vx_delmap_t *dm = t->delmaps ? t->delmaps[ci] : NULL;
            if (dm && vx_delmap_count(dm) >= ch->nrows) continue;

            for (uint32_t r = 0; r < ch->nrows; r++) {
                if (vx_delmap_is_set(dm, r)) continue;
                for (uint32_t c = 0; c < t->ncols; c++) {
                    const vx_col_t *col = &ch->cols[c];
                    if (vx_col_is_null(col, r)) {
                        if (!sb_u8(&b, 1U)) { free(b.data); return false; }
                        continue;
                    }
                    if (!sb_u8(&b, 0U)) { free(b.data); return false; }
                    switch (col->type) {
                        case VX_TYPE_I64:
                            if (!sb_u64(&b, (uint64_t)((const int64_t *)col->data)[r])) {
                                free(b.data); return false;
                            }
                            break;
                        case VX_TYPE_U64:
                            if (!sb_u64(&b, ((const uint64_t *)col->data)[r])) {
                                free(b.data); return false;
                            }
                            break;
                        case VX_TYPE_F64: {
                            uint64_t bits = 0;
                            memcpy(&bits, &((const double *)col->data)[r], sizeof(bits));
                            if (!sb_u64(&b, bits)) { free(b.data); return false; }
                            break;
                        }
                        case VX_TYPE_BOOL:
                            if (!sb_u8(&b, ((const uint8_t *)col->data)[r] ? 1U : 0U)) {
                                free(b.data); return false;
                            }
                            break;
                        case VX_TYPE_STR: {
                            const vx_str_t *s = &((const vx_str_t *)col->data)[r];
                            if (!sb_u32(&b, s->len) || !sb_bytes(&b, vx_str_data(s), s->len)) {
                                free(b.data); return false;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Populate 48-byte header with body CRC32 + header CRC32 */
    uint32_t body_crc = vx_crc32(b.data + VX_SNAP_HDR_SIZE, b.len - VX_SNAP_HDR_SIZE);
    memcpy(b.data + 0, k_snap_magic, 8);
    sp_w32(b.data + 8,  VX_SNAP_VERSION);
    sp_w32(b.data + 12, 0U);
    sp_w64(b.data + 16, checkpoint_lsn);
    sp_w32(b.data + 24, cat->ntables);
    sp_w32(b.data + 28, cat->next_table_id);
    sp_w64(b.data + 32, grand_total_rows);
    sp_w32(b.data + 40, body_crc);
    sp_w32(b.data + 44, vx_crc32(b.data, 44));

    /* Atomic write: <snap_path>.tmp -> fdatasync -> rename -> dir fsync */
    size_t plen = strlen(snap_path);
    char *tmp_path = (char *)malloc(plen + 8U);
    if (!tmp_path) { free(b.data); return false; }
    snprintf(tmp_path, plen + 8U, "%s.tmp", snap_path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(tmp_path); free(b.data); return false; }

    bool ok = write_all_fd(fd, b.data, b.len) && (fdatasync(fd) == 0);
    close(fd);
    free(b.data);

    if (!ok || rename(tmp_path, snap_path) != 0) {
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }

    fsync_parent_dir(snap_path);
    free(tmp_path);
    return true;
}

static bool snap_read_and_verify(const char *snap_path, uint8_t **out_buf,
                                 size_t *out_len, vx_snap_info_t *out_info,
                                 uint32_t *out_next_tid) {
    if (out_info) memset(out_info, 0, sizeof(*out_info));
    if (!snap_path) return false;

    int fd = open(snap_path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)VX_SNAP_HDR_SIZE) {
        close(fd);
        return false;
    }

    size_t fsz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(fsz);
    if (!buf) {
        close(fd);
        return false;
    }

    bool ok = read_all_fd(fd, buf, fsz);
    close(fd);
    if (!ok) {
        free(buf);
        return false;
    }

    if (memcmp(buf, k_snap_magic, 8) != 0 ||
        sp_r32(buf + 8) != VX_SNAP_VERSION ||
        sp_r32(buf + 44) != vx_crc32(buf, 44)) {
        free(buf);
        return false;
    }

    uint32_t stored_body_crc = sp_r32(buf + 40);
    uint32_t actual_body_crc = vx_crc32(buf + VX_SNAP_HDR_SIZE, fsz - VX_SNAP_HDR_SIZE);
    if (stored_body_crc != actual_body_crc) {
        free(buf);
        return false;
    }

    if (out_info) {
        out_info->version = sp_r32(buf + 8);
        out_info->checkpoint_lsn = sp_r64(buf + 16);
        out_info->ntables = sp_r32(buf + 24);
        out_info->total_rows = sp_r64(buf + 32);
        out_info->file_bytes = (uint64_t)fsz;
        out_info->valid_crc = true;
    }
    if (out_next_tid) {
        *out_next_tid = sp_r32(buf + 28);
    }

    if (out_buf) {
        *out_buf = buf;
        *out_len = fsz;
    } else {
        free(buf);
    }
    return true;
}

bool vx_snap_inspect(const char *snap_path, vx_snap_info_t *out) {
    return snap_read_and_verify(snap_path, NULL, NULL, out, NULL);
}

static bool rd_str16(const uint8_t *buf, size_t len, size_t *off, char *dst, size_t dst_cap) {
    if (*off + 2 > len) return false;
    uint16_t n = sp_r16(buf + *off); *off += 2;
    if (n >= dst_cap || n > len - *off) return false;
    if (n > 0) memcpy(dst, buf + *off, n);
    dst[n] = '\0';
    *off += n;
    return true;
}

bool vx_snap_load(const char *snap_path, vx_catalog_t **out_cat, uint64_t *out_lsn,
                  uint32_t chunk_rows, uint64_t *next_chunk_id) {
    if (!snap_path || !out_cat || !out_lsn) return false;

    uint8_t *buf = NULL;
    size_t len = 0;
    vx_snap_info_t info = {0};
    uint32_t next_tid = 0;

    if (!snap_read_and_verify(snap_path, &buf, &len, &info, &next_tid)) {
        return false;
    }

    if (info.ntables > VX_MAX_TABLES) {
        free(buf);
        return false;
    }

    vx_catalog_t *cat = (vx_catalog_t *)calloc(1, sizeof(vx_catalog_t));
    if (!cat) { free(buf); return false; }
    cat->next_table_id = next_tid;
    if (info.ntables > 0) {
        cat->tables = (vx_table_t **)calloc(info.ntables, sizeof(vx_table_t *));
        if (!cat->tables) { free(cat); free(buf); return false; }
    }

    size_t off = VX_SNAP_HDR_SIZE;
    for (uint32_t ti = 0; ti < info.ntables; ti++) {
        vx_table_t *t = (vx_table_t *)calloc(1, sizeof(vx_table_t));
        if (!t) goto fail;
        cat->tables[cat->ntables++] = t;

        if (!rd_str16(buf, len, &off, t->name, sizeof(t->name))) goto fail;
        if (off + 20 > len) goto fail;
        t->table_id = sp_r32(buf + off); off += 4;
        t->ncols    = sp_r32(buf + off); off += 4;
        t->pk_col   = (int32_t)sp_r32(buf + off); off += 4;
        uint64_t live_rows = sp_r64(buf + off); off += 8;

        if (t->ncols == 0 || t->ncols > VX_MAX_COLS) goto fail;
        if (t->pk_col < -1 || t->pk_col >= (int32_t)t->ncols) goto fail;

        for (uint32_t c = 0; c < t->ncols; c++) {
            vx_col_schema_t *cs = &t->cols[c];
            if (!rd_str16(buf, len, &off, cs->name, sizeof(cs->name))) goto fail;
            if (off + 2 > len) goto fail;
            cs->type = (vx_type_t)buf[off++];
            uint8_t flags = buf[off++];
            cs->is_key = (flags & 1U) != 0;
            cs->has_fk = (flags & 2U) != 0;
            if (cs->has_fk) {
                if (!rd_str16(buf, len, &off, cs->fk_table, sizeof(cs->fk_table)) ||
                    !rd_str16(buf, len, &off, cs->fk_col, sizeof(cs->fk_col))) {
                    goto fail;
                }
            }
        }

        if (off + 4 > len) goto fail;
        uint32_t saved_nindexes = sp_r32(buf + off); off += 4;
        if (saved_nindexes > VX_MAX_INDEXES) goto fail;
        for (uint32_t ix = 0; ix < saved_nindexes; ix++) {
            if (off + 5 > len) goto fail;
            t->indexes[ix].is_unique = buf[off++] != 0;
            uint32_t inc = sp_r32(buf + off); off += 4;
            if (inc == 0 || inc > 16U || off + (size_t)inc * 4U > len) goto fail;
            t->indexes[ix].ncols = inc;
            for (uint32_t k = 0; k < inc; k++) {
                uint32_t ci = sp_r32(buf + off); off += 4;
                if (ci >= t->ncols) goto fail;
                t->indexes[ix].col_idxs[k] = ci;
            }
        }
        t->nindexes = 0; /* re-enabled immediately after live rows finish loading */

        if (t->pk_col >= 0 &&
            (t->cols[t->pk_col].type == VX_TYPE_U64 || t->cols[t->pk_col].type == VX_TYPE_I64)) {
            t->pk_index = vx_pk_index_create();
            if (!t->pk_index) goto fail;
        }

        vx_val_t row[VX_MAX_COLS];
        for (uint64_t r = 0; r < live_rows; r++) {
            for (uint32_t c = 0; c < t->ncols; c++) {
                if (off + 1 > len) goto fail;
                memset(&row[c], 0, sizeof(row[c]));
                row[c].type = t->cols[c].type;
                row[c].is_null = buf[off++] != 0;
                if (row[c].is_null) continue;

                switch (row[c].type) {
                    case VX_TYPE_I64:
                        if (off + 8 > len) goto fail;
                        row[c].v.i64 = (int64_t)sp_r64(buf + off); off += 8;
                        break;
                    case VX_TYPE_U64:
                        if (off + 8 > len) goto fail;
                        row[c].v.u64 = sp_r64(buf + off); off += 8;
                        break;
                    case VX_TYPE_F64: {
                        if (off + 8 > len) goto fail;
                        uint64_t bits = sp_r64(buf + off); off += 8;
                        memcpy(&row[c].v.f64, &bits, sizeof(bits));
                        break;
                    }
                    case VX_TYPE_BOOL:
                        if (off + 1 > len) goto fail;
                        row[c].v.b = buf[off++] != 0;
                        break;
                    case VX_TYPE_STR: {
                        if (off + 4 > len) goto fail;
                        uint32_t sl = sp_r32(buf + off); off += 4;
                        if (sl > len - off) goto fail;
                        row[c].v.str.ptr = sl ? (const char *)(buf + off) : "";
                        row[c].v.str.len = sl;
                        off += sl;
                        break;
                    }
                    default:
                        goto fail;
                }
            }
            if (!vx_table_append_row_cow(t, row, chunk_rows, next_chunk_id)) goto fail;
        }
        t->nindexes = saved_nindexes;
    }

    free(buf);
    *out_cat = cat;
    *out_lsn = info.checkpoint_lsn;
    return true;

fail:
    vx_catalog_free(cat);
    free(buf);
    return false;
}

