/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_state_9c_emits_packet_and_returns_cursor(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D6FD0u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t value = 0xA1B2C3D4u;
    const uint32_t cursor = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    const uint32_t result = call_stdcall(0x003D6FD0u, &value, 1u);
    CHECK_EQ_U32(load(cursor), 0x00041E6Cu);
    CHECK_EQ_U32(load(cursor + 4u), value - 0x200u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), cursor + 8u);
    CHECK_EQ_U32(result, cursor + 8u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_RS_SHADOW + 0x9Cu * 4u), value);

    /* The original saves ESI before writing; a packet at slot-4 overwrites the slot word. */
    const uint32_t alias_cursor = D3D8_DEVICE_POINTER_SLOT - 4u;
    const uint32_t alias_value = 0x12345678u;
    store(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR, alias_cursor);
    const uint32_t alias_result = call_stdcall(0x003D6FD0u, &alias_value, 1u);
    CHECK_EQ_U32(load(alias_cursor), 0x00041E6Cu);
    CHECK_EQ_U32(load(D3D8_DEVICE_POINTER_SLOT), alias_value - 0x200u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), alias_cursor + 8u);
    CHECK_EQ_U32(alias_result, alias_cursor + 8u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_RS_SHADOW + 0x9Cu * 4u), alias_value);
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);

    environment_end();
}

int main(void)
{
    test_registered_state_9c_emits_packet_and_returns_cursor();
    printf("test_d3d8_state_9c_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
