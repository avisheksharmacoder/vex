#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/vx_types.h"
#include "src/vx_owner.h"
#include "src/vx_chunk.h"
#include "src/vx_seg_fmt.h"
#include "src/vx_seg.h"
#include "src/vx_manifest_fmt.h"
#include "src/vx_manifest.h"

static void dummy_owner_destroy(vx_owner_t *o) {
    (void)o;
}

/* Helper to clean and re-create a temporary test directory */
static void setup_test_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    assert(system(cmd) == 0);
}

/* 1. Test versioned .del sidecar writer and sparse loader */
static void test_del_write_load_sparse(void) {
    printf("[1/5] Testing .del sidecar writer and sparse chunk loader...\n");

    const char *dir = "/tmp/vex_test_del";
    setup_test_dir(dir);

    const uint32_t table_id = 1;
    const uint64_t seg_id = 42;
    const uint64_t del_gen = 3;
    const uint32_t nrows = 9000; /* 3 chunks: chunk 0 (4096), chunk 1 (4096), chunk 2 (808) */
    const uint32_t nchunks = 3;

    /* Chunk 0: 100% live (no deletes) -> delmap = NULL
     * Chunk 1: 3 rows deleted (rows 0, 100, 200) -> 4093 live
     * Chunk 2: 100% live (no deletes) -> delmap = NULL */
    vx_delmap_t dm1;
    memset(&dm1, 0, sizeof(dm1));
    vx_owner_init(&dm1.hdr, VX_OWN_DELMAP, dummy_owner_destroy);
    dm1.generation = del_gen;
    dm1.nrows = 4096;
    dm1.live_count = 4093;
    dm1.bits[0 >> 6]   |= (1ULL << (0 & 63));
    dm1.bits[100 >> 6] |= (1ULL << (100 & 63));
    dm1.bits[200 >> 6] |= (1ULL << (200 & 63));

    const vx_delmap_t *chunk_delmaps[3] = { NULL, &dm1, NULL };
    uint32_t out_live = 0;

    vx_status_t st = vx_del_write(dir, table_id, seg_id, del_gen, nrows, nchunks,
                                  chunk_delmaps, &out_live);
    assert(st == VX_OK);
    assert(out_live == nrows - 3);

    /* Load sidecar */
    vx_delmap_t **loaded_delmaps = NULL;
    uint32_t loaded_live = 0;
    st = vx_del_load(dir, table_id, seg_id, del_gen, nrows, nchunks,
                     &loaded_delmaps, &loaded_live);
    assert(st == VX_OK);
    assert(loaded_delmaps != NULL);
    assert(loaded_live == nrows - 3);

    /* Sparse Invariant: chunk 0 and chunk 2 MUST be NULL! */
    assert(loaded_delmaps[0] == NULL);
    assert(loaded_delmaps[2] == NULL);

    /* Chunk 1 must be loaded */
    assert(loaded_delmaps[1] != NULL);
    assert(loaded_delmaps[1]->generation == del_gen);
    assert(loaded_delmaps[1]->nrows == 4096);
    assert(loaded_delmaps[1]->live_count == 4093);
    assert(((loaded_delmaps[1]->bits[0 >> 6] >> (0 & 63)) & 1ULL) == 1ULL);
    assert(((loaded_delmaps[1]->bits[100 >> 6] >> (100 & 63)) & 1ULL) == 1ULL);
    assert(((loaded_delmaps[1]->bits[200 >> 6] >> (200 & 63)) & 1ULL) == 1ULL);
    assert(((loaded_delmaps[1]->bits[1 >> 6] >> (1 & 63)) & 1ULL) == 0ULL);

    /* Cleanup loaded delmaps */
    for (uint32_t k = 0; k < nchunks; ++k) {
        if (loaded_delmaps[k]) vx_owner_release(&loaded_delmaps[k]->hdr);
    }
    free(loaded_delmaps);

    printf("      -> .del write and sparse loader PASSED.\n");
}

/* 2. Test .del sidecar corruption and boundary validation */
static void test_del_corruption(void) {
    printf("[2/5] Testing .del corruption rejection and bounds validation...\n");

    const char *dir = "/tmp/vex_test_del_corrupt";
    setup_test_dir(dir);

    const uint32_t table_id = 1, nrows = 100, nchunks = 1;
    const uint64_t seg_id = 10, del_gen = 1;

    vx_delmap_t dm;
    memset(&dm, 0, sizeof(dm));
    vx_owner_init(&dm.hdr, VX_OWN_DELMAP, dummy_owner_destroy);
    dm.nrows = nrows;
    dm.live_count = 99;
    dm.bits[0] = 1ULL; /* delete row 0 */

    const vx_delmap_t *dms[1] = { &dm };
    assert(vx_del_write(dir, table_id, seg_id, del_gen, nrows, nchunks, dms, NULL) == VX_OK);

    char del_file[512];
    assert(vx_del_format_path(dir, seg_id, del_gen, del_file, sizeof(del_file)) == VX_OK);

    /* Corrupt byte in file (byte 56 is file_crc32) */
    int fd = open(del_file, O_RDWR);
    assert(fd >= 0);
    uint8_t bad = 0xFF;
    assert(pwrite(fd, &bad, 1, 56) == 1); /* corrupt CRC */
    close(fd);

    vx_delmap_t **loaded = NULL;
    uint32_t live = 0;
    vx_status_t st = vx_del_load(dir, table_id, seg_id, del_gen, nrows, nchunks, &loaded, &live);
    assert(st == VX_ERR_CORRUPT);

    printf("      -> .del corruption rejection PASSED.\n");
}

/* 3. Test MANIFEST publish, serialization, and reload */
static void test_manifest_publish_and_load(void) {
    printf("[3/5] Testing MANIFEST publish, binary serialization & load...\n");

    const char *dir = "/tmp/vex_test_manifest";
    setup_test_dir(dir);

    vx_manifest_t man;
    memset(&man, 0, sizeof(man));
    man.manifest_gen = 10;
    man.checkpoint_lsn = 9999;
    man.next_seg_id = 5;
    man.flags = 0;

    /* Table 1: users */
    man.ntables = 2;
    man.tables = (vx_man_table_def_t *)calloc(man.ntables, sizeof(vx_man_table_def_t));

    man.tables[0].table_id = 1;
    man.tables[0].name = strdup("users");
    man.tables[0].pk_col_idx = 0;
    man.tables[0].ncols = 3;
    man.tables[0].cols = (vx_man_col_def_t *)calloc(3, sizeof(vx_man_col_def_t));
    man.tables[0].cols[0] = (vx_man_col_def_t){ .col_id = 0, .name = strdup("id"), .type = VX_TYPE_U64, .nullable = 0 };
    man.tables[0].cols[1] = (vx_man_col_def_t){ .col_id = 1, .name = strdup("name"), .type = VX_TYPE_STR, .nullable = 0 };
    man.tables[0].cols[2] = (vx_man_col_def_t){ .col_id = 2, .name = strdup("age"), .type = VX_TYPE_I64, .nullable = 1 };

    man.tables[0].n_indexes = 1;
    man.tables[0].indexes = (vx_man_index_def_t *)calloc(1, sizeof(vx_man_index_def_t));
    man.tables[0].indexes[0].index_id = 101;
    man.tables[0].indexes[0].name = strdup("idx_users_name");
    man.tables[0].indexes[0].ncols = 1;
    man.tables[0].indexes[0].col_ids = (uint32_t *)malloc(sizeof(uint32_t));
    man.tables[0].indexes[0].col_ids[0] = 1;
    man.tables[0].indexes[0].flags = VX_IDX_FLAG_HASH;

    /* Table 2: orders */
    man.tables[1].table_id = 2;
    man.tables[1].name = strdup("orders");
    man.tables[1].pk_col_idx = 0;
    man.tables[1].ncols = 4;
    man.tables[1].cols = (vx_man_col_def_t *)calloc(4, sizeof(vx_man_col_def_t));
    man.tables[1].cols[0] = (vx_man_col_def_t){ .col_id = 0, .name = strdup("id"), .type = VX_TYPE_U64, .nullable = 0 };
    man.tables[1].cols[1] = (vx_man_col_def_t){ .col_id = 1, .name = strdup("user_id"), .type = VX_TYPE_U64, .nullable = 0 };
    man.tables[1].cols[2] = (vx_man_col_def_t){ .col_id = 2, .name = strdup("amount"), .type = VX_TYPE_F64, .nullable = 0 };
    man.tables[1].cols[3] = (vx_man_col_def_t){ .col_id = 3, .name = strdup("paid"), .type = VX_TYPE_BOOL, .nullable = 0 };

    /* Segments */
    man.nsegments = 2;
    man.segments = (vx_man_seg_t *)calloc(2, sizeof(vx_man_seg_t));
    man.segments[0] = (vx_man_seg_t){
        .table_id = 1, .segment_id = 1, .level = 0, .flags = 1,
        .del_gen = 2, .nrows = 4096, .live_rows = 4000,
        .file_bytes = 100000, .min_pk = 1, .max_pk = 4096
    };
    man.segments[1] = (vx_man_seg_t){
        .table_id = 2, .segment_id = 2, .level = 0, .flags = 1,
        .del_gen = 0, .nrows = 1000, .live_rows = 1000,
        .file_bytes = 50000, .min_pk = 10, .max_pk = 1000
    };

    /* Publish */
    vx_status_t st = vx_manifest_publish(dir, &man);
    assert(st == VX_OK);
    assert(man.manifest_gen == 11); /* Incremented generation */

    /* Load from disk */
    vx_manifest_t loaded;
    bool existed = false;
    st = vx_manifest_load(dir, &loaded, &existed);
    assert(st == VX_OK);
    assert(existed == true);
    assert(loaded.manifest_gen == 11);
    assert(loaded.checkpoint_lsn == 9999);
    assert(loaded.next_seg_id == 5);
    assert(loaded.ntables == 2);
    assert(loaded.nsegments == 2);

    /* Verify table 0 (users) */
    assert(strcmp(loaded.tables[0].name, "users") == 0);
    assert(loaded.tables[0].ncols == 3);
    assert(strcmp(loaded.tables[0].cols[1].name, "name") == 0);
    assert(loaded.tables[0].cols[1].type == VX_TYPE_STR);
    assert(loaded.tables[0].n_indexes == 1);
    assert(strcmp(loaded.tables[0].indexes[0].name, "idx_users_name") == 0);
    assert(loaded.tables[0].indexes[0].col_ids[0] == 1);
    assert(loaded.tables[0].indexes[0].flags == VX_IDX_FLAG_HASH);

    /* Verify table 1 (orders) */
    assert(strcmp(loaded.tables[1].name, "orders") == 0);
    assert(loaded.tables[1].ncols == 4);
    assert(loaded.tables[1].cols[2].type == VX_TYPE_F64);

    /* Verify segments */
    assert(loaded.segments[0].segment_id == 1);
    assert(loaded.segments[0].del_gen == 2);
    assert(loaded.segments[0].live_rows == 4000);
    assert(loaded.segments[1].segment_id == 2);
    assert(loaded.segments[1].del_gen == 0);

    /* Test V1 .snap detection when MANIFEST missing */
    char v1_path[512];
    snprintf(v1_path, sizeof(v1_path), "%s/app.wal.snap", dir);
    int sfd = open(v1_path, O_WRONLY | O_CREAT, 0644);
    assert(sfd >= 0);
    close(sfd);

    char man_path[512];
    snprintf(man_path, sizeof(man_path), "%s/MANIFEST", dir);
    (void)unlink(man_path);

    vx_manifest_t v1_man;
    bool v1_existed = false;
    st = vx_manifest_load(dir, &v1_man, &v1_existed);
    assert(st == VX_ERR_SCHEMA); /* Detected V1 snap! */

    vx_manifest_free(&man);
    vx_manifest_free(&loaded);
    printf("      -> MANIFEST publish and load PASSED.\n");
}

/* Helper to touch a file */
static void touch_file(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    assert(fd >= 0);
    close(fd);
}

/* 4. Test orphan sweeping */
static void test_orphan_sweep(void) {
    printf("[4/5] Testing orphan sweeping with strict filename grammar...\n");

    const char *dir = "/tmp/vex_test_sweep";
    setup_test_dir(dir);

    /* Set up MANIFEST referencing seg 1 (del_gen 2) and seg 2 (del_gen 0) */
    vx_manifest_t man;
    memset(&man, 0, sizeof(man));
    man.next_seg_id = 3;
    man.nsegments = 2;
    man.segments = (vx_man_seg_t *)calloc(2, sizeof(vx_man_seg_t));
    man.segments[0].segment_id = 1; man.segments[0].del_gen = 2;
    man.segments[1].segment_id = 2; man.segments[1].del_gen = 0;

    assert(vx_manifest_publish(dir, &man) == VX_OK);

    /* Live files */
    touch_file(dir, "seg_00000001.vx");
    touch_file(dir, "seg_00000001_g0002.del");
    touch_file(dir, "seg_00000002.vx");

    /* Orphan files to sweep */
    touch_file(dir, "MANIFEST.tmp");
    touch_file(dir, "seg_00000001.vx.tmp");
    touch_file(dir, "seg_00000001_g0001.del");      /* superseded generation 1 */
    touch_file(dir, "seg_00000001_g0002.del.tmp");  /* temp file */
    touch_file(dir, "seg_00000003.vx");             /* retired segment */
    touch_file(dir, "seg_00000003_g0001.del");      /* retired del */

    /* Non-Vex file to preserve */
    touch_file(dir, "user_notes.txt");

    uint32_t swept = 0;
    vx_status_t st = vx_manifest_sweep_orphans(dir, &man, &swept);
    assert(st == VX_OK);
    assert(swept == 6); /* 6 orphan files swept */

    /* Verify live and non-Vex files exist */
    char p[512];
    snprintf(p, sizeof(p), "%s/seg_00000001.vx", dir); assert(access(p, F_OK) == 0);
    snprintf(p, sizeof(p), "%s/seg_00000001_g0002.del", dir); assert(access(p, F_OK) == 0);
    snprintf(p, sizeof(p), "%s/seg_00000002.vx", dir); assert(access(p, F_OK) == 0);
    snprintf(p, sizeof(p), "%s/user_notes.txt", dir); assert(access(p, F_OK) == 0);

    /* Verify swept files are gone */
    snprintf(p, sizeof(p), "%s/MANIFEST.tmp", dir); assert(access(p, F_OK) != 0);
    snprintf(p, sizeof(p), "%s/seg_00000001_g0001.del", dir); assert(access(p, F_OK) != 0);
    snprintf(p, sizeof(p), "%s/seg_00000003.vx", dir); assert(access(p, F_OK) != 0);

    vx_manifest_free(&man);
    printf("      -> Strict orphan sweep PASSED.\n");
}

/* 5. Test full crash-recovery entry point (vx_recover_open) */
static uint32_t g_wal_replayed_count = 0;

static vx_status_t mock_wal_apply(void *ctx, const vx_wal_frame_t *frame) {
    (void)ctx;
    (void)frame;
    g_wal_replayed_count++;
    return VX_OK;
}

/* Mock WAL frame serializer */
static void write_mock_wal_frame(int fd, uint64_t lsn, uint32_t op_type, const char *payload) {
    uint32_t plen = (uint32_t)strlen(payload);
    uint32_t total = 8 + 4 + 4 + plen + 4; /* lsn(8) + op(4) + len(4) + payload(plen) + crc(4) */
    uint8_t *buf = (uint8_t *)malloc(total);

    memcpy(buf, &lsn, 8);
    memcpy(buf + 8, &op_type, 4);
    memcpy(buf + 12, &plen, 4);
    memcpy(buf + 16, payload, plen);

    /* CRC over first 16 + plen bytes */
    uint32_t crc = 0;
    for (size_t i = 0; i < 16 + plen; ++i) crc = (crc * 31) ^ buf[i];
    memcpy(buf + 16 + plen, &crc, 4);

    ssize_t nw = write(fd, buf, total);
    assert(nw == (ssize_t)total);
    free(buf);
}

/* Mock WAL frame reader with CRC verification */
static vx_status_t mock_wal_next(void *ctx, int wal_fd, uint64_t *io_off,
                                 vx_wal_frame_t *out_frame, bool *out_eof) {
    (void)ctx;
    off_t fsz = lseek(wal_fd, 0, SEEK_END);
    if ((off_t)*io_off >= fsz) {
        *out_eof = true;
        return VX_OK;
    }

    uint64_t rem = (uint64_t)fsz - *io_off;
    if (rem < 20) { /* Minimum frame size */
        return VX_ERR_CORRUPT; /* Torn write */
    }

    uint8_t hdr[16];
    assert(pread(wal_fd, hdr, 16, (off_t)*io_off) == 16);
    uint64_t lsn; uint32_t op_type, plen;
    memcpy(&lsn, hdr, 8);
    memcpy(&op_type, hdr + 8, 4);
    memcpy(&plen, hdr + 12, 4);

    uint64_t frame_bytes = 16ULL + (uint64_t)plen + 4ULL;
    if (rem < frame_bytes) {
        return VX_ERR_CORRUPT; /* Partial torn write */
    }

    uint8_t *full = (uint8_t *)malloc((size_t)frame_bytes);
    assert(pread(wal_fd, full, (size_t)frame_bytes, (off_t)*io_off) == (ssize_t)frame_bytes);

    uint32_t exp_crc;
    memcpy(&exp_crc, full + 16 + plen, 4);
    uint32_t calc_crc = 0;
    for (size_t i = 0; i < 16 + plen; ++i) calc_crc = (calc_crc * 31) ^ full[i];

    if (calc_crc != exp_crc) {
        free(full);
        return VX_ERR_CORRUPT; /* Corrupted frame */
    }

    out_frame->lsn = lsn;
    out_frame->op_type = op_type;
    out_frame->payload = NULL; /* Not needed for mock */
    out_frame->payload_len = plen;
    *io_off += frame_bytes;
    *out_eof = false;

    free(full);
    return VX_OK;
}

static void test_recover_open_and_wal_heal(void) {
    printf("[5/5] Testing vx_recover_open crash recovery & WAL torn-write auto-healing...\n");

    const char *dir = "/tmp/vex_test_recover";
    setup_test_dir(dir);

    /* 1. Create a valid segment file on disk (seg_00000001.vx) */
    vx_type_t col_types[1] = { VX_TYPE_U64 };
    vx_chunk_t *ch = (vx_chunk_t *)calloc(1, sizeof(*ch));
    vx_owner_init(&ch->hdr, VX_OWN_CHUNK, dummy_owner_destroy);
    ch->nrows = 100; ch->ncols = 1;
    ch->cols = (vx_col_chunk_t *)calloc(1, sizeof(vx_col_chunk_t));
    ch->cols[0].type = VX_TYPE_U64;
    uint64_t *pks = (uint64_t *)malloc(100 * sizeof(uint64_t));
    for (uint32_t i = 0; i < 100; ++i) pks[i] = i + 1;
    ch->cols[0].data = pks;

    const vx_chunk_t *chunks[1] = { ch };
    vx_seg_write_cfg_t scfg = {
        .table_id = 1, .segment_id = 1, .lsn_max = 50,
        .ncols = 1, .pk_col_idx = 0, .col_types = col_types,
        .chunks = chunks, .delmaps = NULL, .nchunks = 1, .spill_dir = "/tmp"
    };
    char seg_file[512];
    assert(vx_seg_format_path(dir, 1, seg_file, sizeof(seg_file)) == VX_OK);
    vx_seg_write_info_t sinfo;
    assert(vx_seg_write(seg_file, &scfg, &sinfo) == VX_OK);

    /* 2. Create MANIFEST referencing seg 1 */
    vx_manifest_t man;
    memset(&man, 0, sizeof(man));
    man.checkpoint_lsn = 50;
    man.next_seg_id = 2;
    man.ntables = 1;
    man.tables = (vx_man_table_def_t *)calloc(1, sizeof(vx_man_table_def_t));
    man.tables[0].table_id = 1;
    man.tables[0].name = strdup("users");
    man.tables[0].pk_col_idx = 0;
    man.tables[0].ncols = 1;
    man.tables[0].cols = (vx_man_col_def_t *)calloc(1, sizeof(vx_man_col_def_t));
    man.tables[0].cols[0] = (vx_man_col_def_t){ .col_id = 0, .name = strdup("id"), .type = VX_TYPE_U64 };

    man.nsegments = 1;
    man.segments = (vx_man_seg_t *)calloc(1, sizeof(vx_man_seg_t));
    man.segments[0] = (vx_man_seg_t){
        .table_id = 1, .segment_id = 1, .level = 0, .flags = sinfo.flags,
        .del_gen = 0, .nrows = 100, .live_rows = 100,
        .file_bytes = sinfo.file_bytes, .min_pk = 1, .max_pk = 100
    };
    assert(vx_manifest_publish(dir, &man) == VX_OK);
    vx_manifest_free(&man);

    /* 3. Create app.wal with:
     *    - frame 1: lsn 40 (<= checkpoint_lsn 50, skipped)
     *    - frame 2: lsn 60 (> checkpoint_lsn, replayed)
     *    - frame 3: lsn 70 (> checkpoint_lsn, replayed)
     *    - torn trailing bytes (crash during frame 4) */
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s/app.wal", dir);
    int wfd = open(wal_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(wfd >= 0);
    write_mock_wal_frame(wfd, 40, 1, "insert user 40");
    write_mock_wal_frame(wfd, 60, 1, "insert user 60");
    write_mock_wal_frame(wfd, 70, 1, "insert user 70");
    off_t valid_end = lseek(wfd, 0, SEEK_CUR);

    /* Append 7 bytes of garbage representing torn write */
    assert(write(wfd, "torn_tail_bytes", 7) == 7);
    close(wfd);

    /* 4. Recover */
    g_wal_replayed_count = 0;
    vx_recovery_state_t rs;
    vx_status_t st = vx_recover_open(dir, mock_wal_next, NULL, mock_wal_apply, NULL, &rs);
    assert(st == VX_OK);
    assert(rs.wal_fd >= 0);
    assert(rs.nmounted == 1);
    assert(rs.mounted[0].seg != NULL);
    assert(rs.mounted[0].seg->file_hdr->segment_id == 1);
    assert(rs.wal_skipped == 1);
    assert(rs.wal_replayed == 2);
    assert(g_wal_replayed_count == 2);
    assert(rs.wal_healed_tail == true);
    assert(rs.max_lsn == 70);
    assert(rs.wal_valid_len == (uint64_t)valid_end);

    /* Verify app.wal was truncated to remove torn write */
    struct stat wal_sb;
    assert(stat(wal_path, &wal_sb) == 0);
    assert(wal_sb.st_size == valid_end);

    /* Verify second open fails due to flock */
    vx_recovery_state_t rs2;
    st = vx_recover_open(dir, mock_wal_next, NULL, mock_wal_apply, NULL, &rs2);
    assert(st == VX_ERR_IO);

    /* Cleanup */
    vx_recovery_state_destroy(&rs);

    /* Free test chunk */
    free(pks);
    free(ch->cols);
    free(ch);

    printf("      -> Crash recovery & WAL auto-healing PASSED.\n");
}

int main(void) {
    printf("=== V2 MANIFEST, .del Sidecars & Recovery Verification ===\n");
    test_del_write_load_sparse();
    test_del_corruption();
    test_manifest_publish_and_load();
    test_orphan_sweep();
    test_recover_open_and_wal_heal();
    printf("All 5 MANIFEST, .del & recovery tests passed successfully!\n");
    return 0;
}
