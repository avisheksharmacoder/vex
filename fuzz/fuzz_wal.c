#include "vx_wal.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static bool sink_cb(const vx_wal_rec_t *rec, void *ctx) {
    (void)rec; (void)ctx;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536U) {
        return 0;
    }
    char path[128];
    snprintf(path, sizeof(path), "/tmp/vx_libfuzz_%ld.wal", (long)getpid());
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return 0;
    }
    if (size > 0) {
        ssize_t unused = write(fd, data, size);
        (void)unused;
    }
    close(fd);

    vx_wal_recovery_t info = {0};
    (void)vx_wal_replay(path, sink_cb, NULL, &info);

    vx_wal_t *w = vx_wal_open(path, &info);
    if (w) {
        vx_wal_close(w);
    }
    unlink(path);
    return 0;
}
