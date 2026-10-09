/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "xgrph_hle.h"
#include "xgrph_swizzle.h"
#define SOURCE 0x00D10020u
#define DESTINATION 0x00D18020u
#define RECTANGLE 0x00D20004u

static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00D10000u, 0x5000u);
    map_fixed(0x00D18000u, 0x5000u);
    map_fixed(0x00D20000u, 0x1000u);
    xgrph_hle_set_fatal(catching_fatal);
    memset(kernel_guest_at(SOURCE, 16384u), 0x5Au, 16384u);
    memset(kernel_guest_at(DESTINATION - 16u, 16416u), 0xCDu, 16416u);
    const uint32_t full[4] = {0u, 0u, 64u, 64u};
    memcpy(kernel_guest_at(RECTANGLE, sizeof(full)), full, sizeof(full));
}

static void refuse(uint32_t args[8])
{
    RUN_EXPECTING_FATAL((void)xgrph_swizzle_rect(args[0], args[1], args[2], args[3],
        args[4], args[5], args[6], args[7]));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003EEABEu);
    const uint8_t *destination = kernel_guest_at(DESTINATION - 16u, 16416u);
    for (size_t i = 0u; i < 16416u; i++) CHECK_EQ_U32(destination[i], 0xCDu);
}

static void test_atomic_refusals(void)
{
    initialise();
    const uint32_t valid[8] = {SOURCE,0u,0u,DESTINATION,64u,64u,0u,4u};
    const uint32_t invalid[][2] = {{1u,1u},{4u,32u},{5u,32u},{6u,RECTANGLE},
        {7u,2u},{0u,0u},{0u,0x00D14001u},{3u,0x00D1C001u},
        {0u,DESTINATION},{0u,DESTINATION - 4u},{0u,DESTINATION + 16380u},
        {0u,0xFFFFFF00u},{3u,0xFFFFFF00u},{2u,0x00D20FF8u}};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint32_t args[8];
        memcpy(args, valid, sizeof(args));
        args[invalid[i][0]] = invalid[i][1];
        refuse(args);
    }
    for (unsigned i = 0u; i < 4u; i++) {
        uint32_t args[8];
        memcpy(args, valid, sizeof(args));
        args[2] = RECTANGLE;
        const uint32_t saved = load(RECTANGLE + i * 4u);
        store(RECTANGLE + i * 4u, saved + 1u);
        refuse(args);
        store(RECTANGLE + i * 4u, saved);
    }
    environment_end();
}

static void test_dispatch_and_argument_preflight(void)
{
    initialise();
    const xgrph_surface_entry row = {0x003EEABEu, "swizzle", 1u};
    CHECK(xgrph_hle_init(&row, 1u));
    CHECK_EQ_U32(xgrph_swizzle_register(), 1u);
    const uint32_t args[8] = {SOURCE,0u,0u,DESTINATION,64u,64u,0u,4u};
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    CHECK(kernel_frame_build(&frame, call_scratch, 0x100u, args, 8u));
    frame.stack_limit = frame.stack_ptr + 32u; /* final argument excluded */
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(0x003EEABEu, &frame));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(DESTINATION), 0xCDCDCDCDu);
    frame.stack_limit = frame.stack_ptr + 36u;
    CHECK_EQ_U32(xgrph_hle_call(0x003EEABEu, &frame), 0xFE0u);
    CHECK_EQ_U32(load(DESTINATION), 0x5A5A5A5Au);
    CHECK_EQ_U32(load(DESTINATION + 16380u), 0x5A5A5A5Au);
    CHECK_EQ_U32(load(DESTINATION - 4u), 0xCDCDCDCDu);
    CHECK_EQ_U32(load(DESTINATION + 16384u), 0xCDCDCDCDu);
    xgrph_hle_shutdown();
    environment_end();
}

int main(void)
{
    test_atomic_refusals();
    test_dispatch_and_argument_preflight();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
