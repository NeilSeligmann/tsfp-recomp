/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "recomp_abi.h"
#include "xdk_thunk.h"
#include "xgrph_hle.h"
#include "xgrph_object_lifetime.h"

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;

#define ENTRY 0x003EF236u
#define OBJECT 0x00D10020u
#define VTABLE 0x004A1BD0u
#define SECONDARY_ENTRY 0x003EF3B7u
#define SECONDARY_VTABLE 0x004A1BD4u
#define THIRD_ENTRY 0x004029BDu
#define THIRD_VTABLE 0x004A1C70u
#define BUFFER_POINTER_ENTRY 0x003E6714u
#define STACK 0x00D10100u

static void setup(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00D10000u, 0x1000u);
    xgrph_hle_set_fatal(catching_fatal);
    const xgrph_surface_entry rows[] = {
        {ENTRY, "CompMask vtable reset (inferred)", 32u},
        {SECONDARY_ENTRY, "XGRPH object vtable reset", 12u},
        {THIRD_ENTRY, "XGRPH object vtable reset", 3u},
        {BUFFER_POINTER_ENTRY, "XGBuffer_GetBufferPointer", 6u},
    };
    CHECK(xgrph_hle_init(rows, 4u));
    CHECK_EQ_U32(xgrph_object_lifetime_register(), 4u);
}

static void test_valid_thiscall_write(void)
{
    setup();
    store(OBJECT - 4u, 0x11223344u);
    store(OBJECT, 0x55667788u);
    store(OBJECT + 4u, 0x99AABBCCu);
    kernel_call_frame frame = {0};
    kernel_frame_set_registers(&frame, OBJECT, 0xD00DFEEDu);
    CHECK_EQ_U32(xgrph_hle_call(ENTRY, &frame), 0u);
    CHECK_EQ_U32(load(OBJECT - 4u), 0x11223344u);
    CHECK_EQ_U32(load(OBJECT), VTABLE);
    CHECK_EQ_U32(load(OBJECT + 4u), 0x99AABBCCu);
    const xgrph_entry *entry = xgrph_hle_entry(ENTRY);
    CHECK(entry != NULL && entry->state == XGRPH_ENTRY_IMPLEMENTED);
    CHECK(entry != NULL && entry->call_count == 1u);
    xgrph_hle_shutdown();
    environment_end();
}

static void test_missing_register_and_write_refusal(void)
{
    setup();
    store(OBJECT, 0x12345678u);
    kernel_call_frame frame = {0};
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(ENTRY, &frame));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, ENTRY);
    CHECK_EQ_U32(load(OBJECT), 0x12345678u);

    fatal_seen = false;
    kernel_frame_set_registers(&frame, 0xFFFFFFFCu, 0u);
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(ENTRY, &frame));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, ENTRY);
    CHECK_EQ_U32(load(OBJECT), 0x12345678u);
    xgrph_hle_shutdown();
    environment_end();
}

static void test_secondary_object_reset(void)
{
    setup();
    store(OBJECT, 0x12345678u);
    kernel_call_frame frame = {0};
    kernel_frame_set_registers(&frame, OBJECT, 0xD00DFEEDu);
    CHECK_EQ_U32(xgrph_hle_call(SECONDARY_ENTRY, &frame), 0u);
    CHECK_EQ_U32(load(OBJECT), SECONDARY_VTABLE);
    const xgrph_entry *entry = xgrph_hle_entry(SECONDARY_ENTRY);
    CHECK(entry != NULL && entry->state == XGRPH_ENTRY_IMPLEMENTED);
    CHECK(entry != NULL && entry->call_count == 1u);
    xgrph_hle_shutdown();
    environment_end();

    setup();
    store(OBJECT, 0x12345678u);
    kernel_call_frame missing = {0};
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(SECONDARY_ENTRY, &missing));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, SECONDARY_ENTRY);
    CHECK_EQ_U32(load(OBJECT), 0x12345678u);
    xgrph_hle_shutdown();
    environment_end();

    setup();
    store(OBJECT, 0x12345678u);
    CHECK(guest_region_set_protect(0x00D10000u, 0x1000u, PAGE_READONLY));
    kernel_frame_set_registers(&frame, OBJECT, 0u);
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(SECONDARY_ENTRY, &frame));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, SECONDARY_ENTRY);
    CHECK_EQ_U32(load(OBJECT), 0x12345678u);
    xgrph_hle_shutdown();
    environment_end();
}

static void test_third_object_reset(void)
{
    setup();
    store(OBJECT, 0x12345678u);
    kernel_call_frame frame = {0};
    kernel_frame_set_registers(&frame, OBJECT, 0xD00DFEEDu);
    CHECK_EQ_U32(xgrph_hle_call(THIRD_ENTRY, &frame), 0u);
    CHECK_EQ_U32(load(OBJECT), THIRD_VTABLE);
    const xgrph_entry *entry = xgrph_hle_entry(THIRD_ENTRY);
    CHECK(entry != NULL && entry->state == XGRPH_ENTRY_IMPLEMENTED);
    CHECK(entry != NULL && entry->call_count == 1u);
    xgrph_hle_shutdown();
    environment_end();

    setup();
    store(OBJECT, 0x12345678u);
    kernel_call_frame missing = {0};
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(THIRD_ENTRY, &missing));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, THIRD_ENTRY);
    CHECK_EQ_U32(load(OBJECT), 0x12345678u);
    xgrph_hle_shutdown();
    environment_end();
}


static void test_buffer_pointer_getter(void)
{
    setup();
    const uint32_t args[] = {OBJECT};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, STACK, 0x100u, args, 1u));
    store(OBJECT, 0x11223344u);
    store(OBJECT + 4u, 0xDEADBEEFu);
    store(OBJECT + 8u, 0x55667788u);
    CHECK_EQ_U32(xgrph_hle_call(BUFFER_POINTER_ENTRY, &frame), 0xDEADBEEFu);
    store(OBJECT + 4u, 0u);
    CHECK_EQ_U32(xgrph_hle_call(BUFFER_POINTER_ENTRY, &frame), 0u);
    CHECK_EQ_U32(load(OBJECT), 0x11223344u);
    CHECK_EQ_U32(load(OBJECT + 4u), 0u);
    CHECK_EQ_U32(load(OBJECT + 8u), 0x55667788u);
    const xgrph_entry *entry = xgrph_hle_entry(BUFFER_POINTER_ENTRY);
    CHECK(entry != NULL && entry->state == XGRPH_ENTRY_IMPLEMENTED);
    CHECK(entry != NULL && entry->call_count == 2u);
    xgrph_hle_shutdown();
    environment_end();

    setup();
    kernel_call_frame missing = {0};
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(BUFFER_POINTER_ENTRY, &missing));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, BUFFER_POINTER_ENTRY);
    xgrph_hle_shutdown();
    environment_end();

    setup();
    const uint32_t invalid_args[] = {0x00D20000u};
    kernel_call_frame invalid = {0};
    CHECK(kernel_frame_build(&invalid, STACK, 0x100u, invalid_args, 1u));
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(BUFFER_POINTER_ENTRY, &invalid));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, BUFFER_POINTER_ENTRY);
    xgrph_hle_shutdown();
    environment_end();
}


static void test_buffer_pointer_dispatch(void)
{
    setup();
    const xdk_dispatch_entry row = {
        BUFFER_POINTER_ENTRY, "XGBuffer_GetBufferPointer", XDK_MODULE_XGRPH,
    };
    CHECK(xdk_thunk_init(&row, 1u));
    CHECK(xdk_thunk_declare_abi(BUFFER_POINTER_ENTRY, XDK_CC_STDCALL, 1u, 0u));
    const uint32_t expected = 0xCAFEBABEu;
    store(OBJECT + 4u, expected);
    g_eax = 0x87654321u;
    g_ecx = 0x13572468u;
    g_edx = 0x24681357u;
    g_esp = STACK;
    store(STACK, 0x003EBFF8u);
    store(STACK + 4u, OBJECT);
    xdk_thunk_dispatch_at(BUFFER_POINTER_ENTRY);
    CHECK_EQ_U32(g_eax, expected);
    CHECK_EQ_U32(g_ecx, 0x13572468u);
    CHECK_EQ_U32(g_edx, 0x24681357u);
    CHECK_EQ_U32(g_esp, STACK + 8u);
    CHECK_EQ_U32(load(OBJECT + 4u), expected);
    xdk_thunk_shutdown();
    xgrph_hle_shutdown();
    environment_end();
}

static void test_reset_dispatch_preserves_eax(void)
{
    const uint32_t addresses[] = {ENTRY, SECONDARY_ENTRY, THIRD_ENTRY};
    const uint32_t vtables[] = {VTABLE, SECONDARY_VTABLE, THIRD_VTABLE};
    for (unsigned i = 0u; i < 3u; i++) {
        setup();
        const xdk_dispatch_entry row = {addresses[i], "XGRPH reset", XDK_MODULE_XGRPH};
        CHECK(xdk_thunk_init(&row, 1u));
        CHECK(xdk_thunk_declare_abi(addresses[i], XDK_CC_THISCALL, 0u, 1u));
        g_eax = 0x87654321u;
        g_ecx = OBJECT;
        g_edx = 0x24681357u;
        g_esp = STACK;
        store(STACK, 0x003EBFF8u);
        xdk_thunk_dispatch_at(addresses[i]);
        CHECK_EQ_U32(g_eax, 0x87654321u);
        CHECK_EQ_U32(g_ecx, OBJECT);
        CHECK_EQ_U32(g_edx, 0x24681357u);
        CHECK_EQ_U32(g_esp, STACK + 4u);
        CHECK_EQ_U32(load(OBJECT), vtables[i]);
        xdk_thunk_shutdown();
        xgrph_hle_shutdown();
        environment_end();
    }
}

static void test_read_only_object_refuses_before_write(void)
{
    setup();
    store(OBJECT, 0x12345678u);
    CHECK(guest_region_set_protect(0x00D10000u, 0x1000u, PAGE_READONLY));
    kernel_call_frame frame = {0};
    kernel_frame_set_registers(&frame, OBJECT, 0u);
    RUN_EXPECTING_FATAL((void)xgrph_hle_call(ENTRY, &frame));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, ENTRY);
    CHECK(guest_region_set_protect(0x00D10000u, 0x1000u, PAGE_READWRITE));
    CHECK_EQ_U32(load(OBJECT), 0x12345678u);
    xgrph_hle_shutdown();
    environment_end();
}

int main(void)
{
    test_valid_thiscall_write();
    test_missing_register_and_write_refusal();
    test_secondary_object_reset();
    test_third_object_reset();
    test_buffer_pointer_getter();
    test_buffer_pointer_dispatch();
    test_reset_dispatch_preserves_eax();
    test_read_only_object_refuses_before_write();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
