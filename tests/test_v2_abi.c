#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vx_types.h"
#include "vx_owner.h"
#include "vx_arena.h"
#include "vx_str.h"
#include "vx_chunk.h"
#include "vx_batch.h"
#include "vx_op.h"
#include "vx_simd.h"

static bool g_destroyed = false;

static void test_destroy(vx_owner_t *self) {
    (void)self;
    g_destroyed = true;
}

int main(void) {
    printf("[test_v2_abi] Starting V2 ABI verification...\n");

    /* 1. Size & Alignment assertions */
    assert(sizeof(vx_slab_t) == VX_ALIGN);
    assert(sizeof(vx_strview_t) == 16);
    assert(sizeof(vx_type_t) == 1);
    assert(sizeof(vx_encoding_t) == 1);
    assert(sizeof(vx_status_t) == 4);

    /* 2. Test vx_owner lifecycle */
    vx_owner_t owner;
    vx_owner_init(&owner, VX_OWN_CHUNK, test_destroy);
    assert(atomic_load(&owner.refcnt) == 1);

    vx_owner_retain(&owner);
    assert(atomic_load(&owner.refcnt) == 2);

    vx_owner_release(&owner);
    assert(atomic_load(&owner.refcnt) == 1);
    assert(!g_destroyed);

    vx_owner_release(&owner);
    assert(g_destroyed);

    /* 3. Test vx_strview inline */
    vx_strview_t sv;
    memset(&sv, 0, sizeof(sv));
    sv.inl.len = 5;
    memcpy(sv.inl.data, "hello", 5);

    assert(vx_strview_is_inline(sv));
    const uint8_t *ptr = vx_strview_ptr(&sv, NULL);
    assert(memcmp(ptr, "hello", 5) == 0);

    /* 4. Test vx_sel capacity */
    vx_sel_t sel;
    sel.count = VX_BATCH_CAP;
    sel.is_identity = true;
    assert(sel.count == 4096);

    printf("[test_v2_abi] ALL CHECKS PASSED!\n");
    return 0;
}

