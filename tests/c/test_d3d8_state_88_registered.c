/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

static void test_registered_state_88_stores_and_returns_argument(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D6C60u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    CHECK(d3d8_device_register() != 0u);

    const uint32_t initial_dirty = 0x31u;
    const uint32_t value = 0xA1B2C3D4u;
    store(D3D8_GLOBAL_DIRTY_MASK, initial_dirty);
    const uint32_t result = call_stdcall(0x003D6C60u, &value, 1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x790u), value);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), initial_dirty | 0x4000u);
    CHECK_EQ_U32(load(0x003E3EE0u), value);
    CHECK_EQ_U32(result, value);
    store(D3D8_DEVICE_POINTER_SLOT, SCRATCH_DATA);
    store(SCRATCH_DATA + 0x790u, 0x55u);
    const uint32_t redirected = 0x87654321u;
    CHECK_EQ_U32(call_stdcall(0x003D6C60u, &redirected, 1u), redirected);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x790u), redirected);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x790u), value);

    environment_end();
}

int main(void)
{
    test_registered_state_88_stores_and_returns_argument();
    printf("test_d3d8_state_88_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
