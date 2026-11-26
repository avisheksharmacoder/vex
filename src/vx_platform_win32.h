#pragma once

#if !defined(_WIN32)
/* ============================================================================
 * POSIX Fast Path (Linux / macOS): Zero Overhead Wrappers
 * ============================================================================ */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct stat vx_stat_t;
#define vx_fstat64(fd, sb)        fstat((fd), (sb))
#define vx_lseek64(fd, off, wh)   lseek((fd), (off), (wh))
#define vx_ftruncate64(fd, len)   ftruncate((fd), (len))
#define vx_rename_atomic(src,dst) rename((src), (dst))

static inline int vx_aligned_alloc_posix(void **out, size_t align, size_t sz) {
    return posix_memalign(out, align, sz);
}
static inline void vx_aligned_free(void *ptr) {
    free(ptr);
}

#else
/* ============================================================================
 * Win32 Portability Layer (Windows x64 / ARM64)
 * ============================================================================ */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <malloc.h>
#include <process.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct __stat64 vx_stat_t;
#define vx_fstat64(fd, sb)        _fstat64((fd), (sb))
#define vx_lseek64(fd, off, wh)   _lseeki64((fd), (off), (wh))
#define vx_ftruncate64(fd, len)   _chsize_s((fd), (len))

static inline int vx_aligned_alloc_posix(void **out, size_t align, size_t sz) {
    if (!out) return EINVAL;
    void *p = _aligned_malloc(sz, align);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
static inline void vx_aligned_free(void *ptr) {
    _aligned_free(ptr);
}
#endif

