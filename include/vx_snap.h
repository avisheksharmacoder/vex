#ifndef VX_SNAP_H
#define VX_SNAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t version;
    uint64_t checkpoint_lsn;
    uint32_t ntables;
    uint64_t total_rows;
    uint64_t file_bytes;
    bool     valid_crc;
} vx_snap_info_t;

/* Inspects and verifies CRC32 of a binary .snap checkpoint file without loading it */
bool vx_snap_inspect(const char *snap_path, vx_snap_info_t *out);

#endif /* VX_SNAP_H */
