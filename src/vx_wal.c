#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "vx_wal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define VX_WAL_FILE_HDR_SIZE  16U
#define VX_WAL_REC_HDR_SIZE   28U
#define VX_WAL_VERSION        1U
#define VX_WAL_REC_MAGIC      0x56585245U /* "VXRE" */

static const uint8_t k_wal_magic[8] = { 'V', 'X', 'W', 'A', 'L', '0', '0', '1' };

struct vx_wal {
    int      fd;
    uint64_t last_lsn;
    uint64_t write_offset;
    uint8_t *scratch;
    size_t   scratch_cap;
};

static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
static uint32_t       g_crc_table[256];

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    pthread_once(&g_crc_once, crc32_init_table);
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = g_crc_table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

uint32_t vx_crc32(const void *data, size_t len) {
    if (!data && len > 0) {
        return 0;
    }
    return crc32_update(0xFFFFFFFFU, data, len) ^ 0xFFFFFFFFU;
}

static inline void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static inline void put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (i * 8)) & 0xFFU);
    }
}

static inline uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t get_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i] << (i * 8));
    }
    return v;
}

static bool pwrite_all(int fd, const uint8_t *buf, size_t len, uint64_t off) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = pwrite(fd, buf + written, len - written, (off_t)(off + written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

static bool pread_all(int fd, uint8_t *buf, size_t len, uint64_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static bool write_file_header(int fd) {
    uint8_t hdr[VX_WAL_FILE_HDR_SIZE];
    memcpy(hdr, k_wal_magic, 8);
    put_le32(hdr + 8, VX_WAL_VERSION);
    put_le32(hdr + 12, vx_crc32(hdr, 12));
    return pwrite_all(fd, hdr, sizeof(hdr), 0) && (fdatasync(fd) == 0);
}

static bool wal_scan_fd(int fd, vx_wal_replay_fn cb, void *user_ctx,
                        vx_wal_recovery_t *out_info) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return false;
    }

    vx_wal_recovery_t info = {0};
    uint64_t fsize = st.st_size > 0 ? (uint64_t)st.st_size : 0;

    if (fsize == 0) {
        if (out_info) *out_info = info;
        return true;
    }

    if (fsize < VX_WAL_FILE_HDR_SIZE) {
        info.had_corrupt_tail = true;
        info.valid_end_offset = 0;
        info.truncated_bytes = fsize;
        if (out_info) *out_info = info;
        return true;
    }

    uint8_t fhdr[VX_WAL_FILE_HDR_SIZE];
    if (!pread_all(fd, fhdr, sizeof(fhdr), 0)) {
        return false;
    }
    if (memcmp(fhdr, k_wal_magic, 8) != 0 ||
        get_le32(fhdr + 8) != VX_WAL_VERSION ||
        get_le32(fhdr + 12) != vx_crc32(fhdr, 12)) {
        info.had_corrupt_tail = true;
        info.valid_end_offset = 0;
        info.truncated_bytes = fsize;
        if (out_info) *out_info = info;
        return true;
    }

    uint64_t off = VX_WAL_FILE_HDR_SIZE;
    uint64_t prev_lsn = 0;
    uint8_t *pbuf = NULL;
    size_t pcap = 0;

    while (off < fsize) {
        if (fsize - off < VX_WAL_REC_HDR_SIZE) {
            info.had_corrupt_tail = true;
            break;
        }

        uint8_t rhdr[VX_WAL_REC_HDR_SIZE];
        if (!pread_all(fd, rhdr, sizeof(rhdr), off)) {
            free(pbuf);
            return false;
        }

        uint32_t magic = get_le32(rhdr + 0);
        uint32_t stored_crc = get_le32(rhdr + 4);
        uint64_t lsn = get_le64(rhdr + 8);
        uint32_t table_id = get_le32(rhdr + 16);
        uint16_t op_type = get_le16(rhdr + 20);
        uint16_t flags = get_le16(rhdr + 22);
        uint32_t payload_len = get_le32(rhdr + 24);

        if (magic != VX_WAL_REC_MAGIC ||
            payload_len > VX_WAL_MAX_PAYLOAD ||
            lsn <= prev_lsn ||
            (uint64_t)payload_len > (fsize - off - VX_WAL_REC_HDR_SIZE)) {
            info.had_corrupt_tail = true;
            break;
        }

        if (payload_len > pcap) {
            uint8_t *np = (uint8_t *)realloc(pbuf, payload_len);
            if (!np) {
                free(pbuf);
                return false;
            }
            pbuf = np;
            pcap = payload_len;
        }

        if (payload_len > 0 && !pread_all(fd, pbuf, payload_len, off + VX_WAL_REC_HDR_SIZE)) {
            free(pbuf);
            return false;
        }

        uint32_t crc = crc32_update(0xFFFFFFFFU, rhdr + 8, VX_WAL_REC_HDR_SIZE - 8U);
        if (payload_len > 0) {
            crc = crc32_update(crc, pbuf, payload_len);
        }
        crc ^= 0xFFFFFFFFU;

        if (crc != stored_crc) {
            info.had_corrupt_tail = true;
            break;
        }

        if (cb) {
            vx_wal_rec_t rec = {
                .lsn = lsn,
                .table_id = table_id,
                .op_type = op_type,
                .flags = flags,
                .payload_len = payload_len,
                .payload = payload_len ? pbuf : NULL
            };
            if (!cb(&rec, user_ctx)) {
                free(pbuf);
                return false;
            }
        }

        prev_lsn = lsn;
        info.records_replayed++;
        info.last_valid_lsn = lsn;
        off += VX_WAL_REC_HDR_SIZE + (uint64_t)payload_len;
    }

    free(pbuf);
    info.valid_end_offset = off;
    info.truncated_bytes = fsize - off;
    if (out_info) {
        *out_info = info;
    }
    return true;
}

vx_wal_t *vx_wal_open(const char *path, vx_wal_recovery_t *out_recovery) {
    if (!path) {
        return NULL;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return NULL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }

    vx_wal_recovery_t rec = {0};
    uint64_t write_off = VX_WAL_FILE_HDR_SIZE;

    if (st.st_size == 0) {
        if (!write_file_header(fd)) {
            close(fd);
            return NULL;
        }
        rec.valid_end_offset = VX_WAL_FILE_HDR_SIZE;
    } else {
        if (!wal_scan_fd(fd, NULL, NULL, &rec)) {
            close(fd);
            return NULL;
        }
        if (rec.valid_end_offset == 0) {
            if (ftruncate(fd, 0) != 0 || !write_file_header(fd)) {
                close(fd);
                return NULL;
            }
            write_off = VX_WAL_FILE_HDR_SIZE;
        } else {
            if (rec.had_corrupt_tail) {
                if (ftruncate(fd, (off_t)rec.valid_end_offset) != 0 ||
                    fdatasync(fd) != 0) {
                    close(fd);
                    return NULL;
                }
            }
            write_off = rec.valid_end_offset;
        }
    }

    vx_wal_t *wal = (vx_wal_t *)calloc(1, sizeof(vx_wal_t));
    if (!wal) {
        close(fd);
        return NULL;
    }
    wal->fd = fd;
    wal->last_lsn = rec.last_valid_lsn;
    wal->write_offset = write_off;

    if (out_recovery) {
        *out_recovery = rec;
    }
    return wal;
}

void vx_wal_close(vx_wal_t *wal) {
    if (!wal) {
        return;
    }
    if (wal->fd >= 0) {
        close(wal->fd);
    }
    free(wal->scratch);
    free(wal);
}

bool vx_wal_append_batch(vx_wal_t *wal, const vx_wal_rec_t *recs,
                         size_t count, bool fsync_now) {
    if (!wal || !recs || count == 0) {
        return false;
    }

    uint64_t prev_lsn = wal->last_lsn;
    size_t total_bytes = 0;

    for (size_t i = 0; i < count; i++) {
        if (recs[i].lsn <= prev_lsn) {
            return false;
        }
        if (recs[i].payload_len > VX_WAL_MAX_PAYLOAD) {
            return false;
        }
        if (recs[i].payload_len > 0 && !recs[i].payload) {
            return false;
        }
        size_t rec_sz = VX_WAL_REC_HDR_SIZE + (size_t)recs[i].payload_len;
        if (total_bytes > SIZE_MAX - rec_sz) {
            return false;
        }
        total_bytes += rec_sz;
        prev_lsn = recs[i].lsn;
    }

    if (total_bytes > wal->scratch_cap) {
        uint8_t *ns = (uint8_t *)realloc(wal->scratch, total_bytes);
        if (!ns) {
            return false;
        }
        wal->scratch = ns;
        wal->scratch_cap = total_bytes;
    }

    uint8_t *p = wal->scratch;
    for (size_t i = 0; i < count; i++) {
        put_le32(p + 0, VX_WAL_REC_MAGIC);
        put_le64(p + 8, recs[i].lsn);
        put_le32(p + 16, recs[i].table_id);
        put_le16(p + 20, recs[i].op_type);
        put_le16(p + 22, recs[i].flags);
        put_le32(p + 24, recs[i].payload_len);

        if (recs[i].payload_len > 0) {
            memcpy(p + VX_WAL_REC_HDR_SIZE, recs[i].payload, recs[i].payload_len);
        }

        uint32_t crc = crc32_update(0xFFFFFFFFU, p + 8,
                                    (VX_WAL_REC_HDR_SIZE - 8U) + (size_t)recs[i].payload_len);
        put_le32(p + 4, crc ^ 0xFFFFFFFFU);

        p += VX_WAL_REC_HDR_SIZE + (size_t)recs[i].payload_len;
    }

    if (!pwrite_all(wal->fd, wal->scratch, total_bytes, wal->write_offset)) {
        if (ftruncate(wal->fd, (off_t)wal->write_offset) != 0) {
            /* ignore rollback truncate failure */
        }
        return false;
    }
    if (fsync_now && fdatasync(wal->fd) != 0) {
        return false;
    }

    wal->write_offset += total_bytes;
    wal->last_lsn = prev_lsn;
    return true;
}

uint64_t vx_wal_last_lsn(const vx_wal_t *wal) {
    return wal ? wal->last_lsn : 0;
}

bool vx_wal_reset(vx_wal_t *wal, uint64_t new_last_lsn) {
    if (!wal || wal->fd < 0) {
        return false;
    }
    if (ftruncate(wal->fd, 0) != 0) {
        return false;
    }
    if (!write_file_header(wal->fd)) {
        return false;
    }
    wal->write_offset = VX_WAL_FILE_HDR_SIZE;
    wal->last_lsn = new_last_lsn;
    return true;
}

bool vx_wal_replay(const char *path, vx_wal_replay_fn cb, void *user_ctx,
                   vx_wal_recovery_t *out_info) {
    if (!path) {
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    bool ok = wal_scan_fd(fd, cb, user_ctx, out_info);
    close(fd);
    return ok;
}
