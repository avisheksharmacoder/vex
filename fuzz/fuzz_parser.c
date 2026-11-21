#include "vx_arena.h"
#include "vx_parser.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096U) {
        return 0;
    }
    vx_arena_t arena;
    if (!vx_arena_init(&arena, 64 * 1024)) {
        return 0;
    }
    vx_parse_result_t res;
    (void)vx_parse((const char *)data, size, &arena, &res);
    vx_arena_destroy(&arena);
    return 0;
}
