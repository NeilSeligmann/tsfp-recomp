/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_state_98_emits_multisample_and_fixed_viewport_tail(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D8250u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    store(D3D8_DEVICE_BASE + 0x794u, 0x003D2000u);
    store(0x003D2004u, 0u); /* fixed-function declaration flags */
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t value = 1u;
    const uint32_t cursor = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    const uint32_t result = call_stdcall(0x003D8250u, &value, 1u);
    CHECK_EQ_U32(load(cursor), 0x00041D7Cu);
    CHECK_EQ_U32(load(cursor + 4u), 0u);
    CHECK_EQ_U32(load(cursor + 8u), 0x00100A20u);
    CHECK_EQ_U32(load(cursor + 12u), 0u);
    CHECK_EQ_U32(load(cursor + 16u), 0u);
    CHECK_EQ_U32(load(cursor + 20u), 0u);
    CHECK_EQ_U32(load(cursor + 24u), 0u);
    CHECK_EQ_U32(load(cursor + 28u), 0x00080394u);
    CHECK_EQ_U32(load(cursor + 32u), 0u);
    CHECK_EQ_U32(load(cursor + 36u), 0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), cursor + 40u);
    CHECK_EQ_U32(result, cursor + 40u);
    CHECK_EQ_U32(load(D3D8_STATE_98), value);

    environment_end();
}

int main(void)
{
    test_registered_state_98_emits_multisample_and_fixed_viewport_tail();
    printf("test_d3d8_state_98_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
