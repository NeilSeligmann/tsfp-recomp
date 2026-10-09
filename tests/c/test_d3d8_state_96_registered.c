/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_state_96_matches_zero_and_nonzero_packets(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D7320u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t zero = 0u;
    const uint32_t first = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    CHECK_EQ_U32(call_stdcall(0x003D7320u, &zero, 1u), first + 8u);
    CHECK_EQ_U32(load(first), 0x000417BCu);
    CHECK_EQ_U32(load(first + 4u), 0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), first + 8u);
    CHECK_EQ_U32(load(D3D8_STATE_96), 0u);

    const uint32_t enabled = 0x13579BDFu;
    const uint32_t second = first + 8u;
    CHECK_EQ_U32(call_stdcall(0x003D7320u, &enabled, 1u), second + 12u);
    CHECK_EQ_U32(load(second), 0x000817BCu);
    CHECK_EQ_U32(load(second + 4u), 1u);
    CHECK_EQ_U32(load(second + 8u), enabled);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), second + 12u);
    CHECK_EQ_U32(load(D3D8_STATE_96), enabled);

    environment_end();
}

int main(void)
{
    test_registered_state_96_matches_zero_and_nonzero_packets();
    printf("test_d3d8_state_96_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
