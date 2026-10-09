/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_edge_anti_alias_emits_packet_and_returns_cursor(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D6F90u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t value = 0xA1B2C3D4u;
    const uint32_t cursor = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    const uint32_t result = call_stdcall(0x003D6F90u, &value, 1u);
    CHECK_EQ_U32(load(cursor), 0x00080320u);
    CHECK_EQ_U32(load(cursor + 4u), value);
    CHECK_EQ_U32(load(cursor + 8u), value);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), cursor + 12u);
    CHECK_EQ_U32(result, cursor + 12u);
    CHECK_EQ_U32(load(D3D8_STATE_97), value);

    environment_end();
}

int main(void)
{
    test_registered_edge_anti_alias_emits_packet_and_returns_cursor();
    printf("test_d3d8_state_97_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
