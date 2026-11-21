#ifndef VX_PARSER_H
#define VX_PARSER_H

#include "vx_arena.h"
#include "vx_ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool         ok;
    uint32_t     err_line;
    uint32_t     err_col;
    char         err_msg[128];
    vx_program_t program;
} vx_parse_result_t;

/* Parses 1..N statements into arena memory with zero heap allocations. */
bool vx_parse(const char *src, size_t len, vx_arena_t *arena, vx_parse_result_t *out);

#endif /* VX_PARSER_H */

