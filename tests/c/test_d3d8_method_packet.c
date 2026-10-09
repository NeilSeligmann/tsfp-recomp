/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1066: bounded original-backed port of 0x003D52F0 and its two Story callers.
 */
#include "test_d3d8_support.h"

#include "d3d8_method_packet.h"

static uint32_t make_ring(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const d3d8_surface_entry row = {0x003D52F0u, NULL, 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    CHECK_EQ_U32(d3d8_device_register(), 1u);
    CHECK(d3d8_hle_entry(0x003D52F0u)->state == D3D8_ENTRY_IMPLEMENTED);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    return load(0x003E3F60u + 0x24u);
}

static uint32_t byte_transform(uint32_t input)
{
    return (input & 0xFF00FF00u) | ((input & 0xFFu) << 16) | ((input >> 16) & 0xFFu);
}

static void test_story_packet_and_scalar_byte_order(void)
{
    const uint32_t base = make_ring();
    /* Both measured Story callers pass state 2 and the value read from 0x00665448. */
    const uint32_t input = 0x12345678u;
    const uint32_t args[2] = {2u, input};
    const uint32_t result = call_stdcall(0x003D52F0u, args, 2u);

    CHECK_EQ_U32(load(base), 0x00041948u);
    CHECK_EQ_U32(load(base + 4u), 0x12785634u);
    CHECK_EQ_U32(load(0x003E3F60u), base + 8u);
    CHECK_EQ_U32(result, base + 8u);
    /* The byte permutation keeps bytes 1 and 3, and swaps bytes 0 and 2. */
    CHECK_EQ_U32(byte_transform(0xA1B2C3D4u), 0xA1D4C3B2u);
    environment_end();
}

static void test_refills_only_when_put_reaches_limit(void)
{
    (void)make_ring();
    const uint32_t limit = load(0x003E3F60u + 4u);
    store(0x003E3F60u, limit - 4u);
    const uint32_t args[2] = {2u, 0xFFEEDDCCu};
    (void)call_stdcall(0x003D52F0u, args, 2u);
    CHECK_EQ_U32(load(0x003E3F60u), limit + 4u);
    CHECK_EQ_U32(load(limit - 4u), 0x00041948u);
    CHECK_EQ_U32(load(limit), byte_transform(args[1]));
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(), 0u);
    environment_end();

    const uint32_t wrapped_base = make_ring();
    const uint32_t old_limit = load(0x003E3F60u + 4u);
    store(0x003E3F60u, old_limit);
    const uint32_t exact_limit_args[2] = {2u, 0x01020304u};
    const uint32_t result = call_stdcall(0x003D52F0u, exact_limit_args, 2u);
    CHECK_EQ_U32(load(old_limit), 0x00041948u);
    CHECK_EQ_U32(load(old_limit + 4u), byte_transform(exact_limit_args[1]));
    CHECK_EQ_U32(load(0x003E3F60u), old_limit + 8u);
    CHECK_EQ_U32(load(0x003E3F60u + 4u), old_limit + 0x10000u - 0x204u);
    CHECK_EQ_U32(result, old_limit + 8u);
    CHECK(d3d8_pushbuffer_rollovers() == 1u);
    CHECK(wrapped_base != 0u);
    environment_end();
}

static void test_uninitialized_pushbuffer_refuses_before_packet_write(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const d3d8_surface_entry row = {0x003D52F0u, NULL, 2u};
    CHECK(d3d8_hle_init(&row, 1u));
    CHECK_EQ_U32(d3d8_device_register(), 1u);
    const uint32_t args[2] = {2u, 0x12345678u};
    RUN_EXPECTING_FATAL((void)call_stdcall(0x003D52F0u, args, 2u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D6B20u);
    CHECK_EQ_U32(load(0x003E3F60u), 0u);
    environment_end();
}

int main(void)
{
    test_story_packet_and_scalar_byte_order();
    test_refills_only_when_put_reaches_limit();
    test_uninitialized_pushbuffer_refuses_before_packet_write();
    printf("d3d8_method_packet: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
