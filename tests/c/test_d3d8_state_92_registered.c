/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_state_92_emits_both_packets_and_returns_tail_cursor(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D70D0u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t current_93 = 3u;
    const uint32_t value = 2u;
    store(D3D8_STATE_92, 0u);
    store(D3D8_STATE_93, current_93);
    const uint32_t cursor = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    const uint32_t result = call_stdcall(0x003D70D0u, &value, 1u);
    CHECK_EQ_U32(load(cursor), 0x000403A0u);
    CHECK_EQ_U32(load(cursor + 4u), value);
    CHECK_EQ_U32(load(cursor + 8u), 0x00040308u);
    CHECK_EQ_U32(load(cursor + 12u), 1u);
    CHECK_EQ_U32(load(cursor + 16u), 0x0004039Cu);
    CHECK_EQ_U32(load(cursor + 20u), 0x405u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), cursor + 24u);
    CHECK_EQ_U32(result, cursor + 24u);
    CHECK_EQ_U32(load(D3D8_STATE_92), value);
    CHECK_EQ_U32(load(D3D8_STATE_93), current_93);

    environment_end();
}

int main(void)
{
    test_registered_state_92_emits_both_packets_and_returns_tail_cursor();
    printf("test_d3d8_state_92_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
