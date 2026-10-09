/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_state_9d_emits_truncated_scaled_word(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D71B0u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    store(D3D8_DEVICE_BASE + 0x964u, 0x3F800000u);
    store(0x0047D1E4u, 0x41000000u);
    store(0x00475CD4u, 0x3F000000u);
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t value = 0x3FC00000u; /* 1.5f: 1.5 * 1.0 * 8.0 + 0.5 -> cvtt 12 */
    const uint32_t cursor = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    const uint32_t result = call_stdcall(0x003D71B0u, &value, 1u);
    CHECK_EQ_U32(load(cursor), 0x00040380u);
    CHECK_EQ_U32(load(cursor + 4u), 12u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), cursor + 8u);
    CHECK_EQ_U32(result, cursor + 8u);
    CHECK_EQ_U32(load(0x003E3F34u), value);

    environment_end();
}

int main(void)
{
    test_registered_state_9d_emits_truncated_scaled_word();
    printf("test_d3d8_state_9d_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
