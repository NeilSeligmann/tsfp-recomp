/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "test_d3d8_support.h"

#include "d3d8_state.h"

static void test_registered_state_a5_recomputes_control_words_and_packet(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_hle_shutdown();
    const d3d8_surface_entry row = {.address = 0x003D81D0u, .name = NULL, .sites = 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    CHECK(d3d8_device_register() != 0u);

    const uint32_t prior_a = 0x87654321u;
    const uint32_t prior_b = 0x12345678u;
    store(D3D8_DEVICE_BASE + 0x2428u, prior_a);
    store(D3D8_DEVICE_BASE + 0x242Cu, prior_b);
    store(D3D8_STATE_A3, 1u);
    store(D3D8_STATE_A4, 1u);
    const uint32_t value = 1u;
    const uint32_t cursor = load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR);
    const uint32_t result = call_stdcall(0x003D81D0u, &value, 1u);
    const uint32_t word_a = (prior_a & 0xFFFFFFF7u) | 0x8u;
    const uint32_t word_b = (prior_b & 0xE7EFFFFFu) | 0x08100000u;
    const uint32_t expected[] = {0x00040110u, 0u,          0x00081D8Cu, 0x00400094u,
                                 word_a,     0x00040100u, 9u,           0x00081D8Cu,
                                 0x00400B80u, word_b,     0x00040100u, 9u};
    for (size_t index = 0u; index < sizeof(expected) / sizeof(expected[0]); index++) {
        CHECK_EQ_U32(load(cursor + (uint32_t)index * 4u), expected[index]);
    }
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x2428u), word_a);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 0x242Cu), word_b);
    CHECK_EQ_U32(load(0x003E3F54u), value);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + D3D8_DEV_CURSOR), cursor + 48u);
    CHECK_EQ_U32(result, cursor + 48u);

    environment_end();
}

int main(void)
{
    test_registered_state_a5_recomputes_control_words_and_packet();
    printf("test_d3d8_state_a5_registered: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
