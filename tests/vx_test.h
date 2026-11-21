#ifndef VX_TEST_H
#define VX_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int vx_tests_run = 0;
static int vx_tests_failed = 0;

#define VX_ASSERT(cond) do {                                                \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        vx_tests_failed++;                                                  \
        return;                                                             \
    }                                                                       \
} while (0)

#define VX_RUN(fn) do {                                                     \
    int before = vx_tests_failed;                                           \
    vx_tests_run++;                                                         \
    fn();                                                                   \
    if (vx_tests_failed == before) {                                        \
        printf("  [PASS] %s\n", #fn);                                       \
    }                                                                       \
} while (0)

static inline double vx_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline int vx_test_summary(const char *suite) {
    printf("\n[%s] %d run, %d failed\n", suite, vx_tests_run, vx_tests_failed);
    return vx_tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* VX_TEST_H */

