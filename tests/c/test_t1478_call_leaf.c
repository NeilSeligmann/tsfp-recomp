/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent frame controls. Mocks check adapter ABI; live proofs use actual callees. */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
static uint32_t expected_argument, expected_pc, expected_eax, callee_result;
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

static void checked_callee(void)
{
    CHECK(g_esp == 0xFFF8u);
    CHECK(guest_read32(g_esp) == expected_pc);
    CHECK(guest_read32(g_esp + 4u) == expected_argument);
    CHECK(g_eax == expected_eax);
    CHECK(g_ecx == 0x12345678u && g_edx == 0x87654321u);
    g_eax = callee_result;
    g_ecx = 0xABCDEF01u;
    g_edx = 0x10203040u;
    g_esp += 4u;
    calls++;
}

game_guest_function recomp_lookup(uint32_t va)
{
    return va == 0x827B0u || va == 0x24E90u || va == 0x23FF20u ? checked_callee : NULL;
}
extern void sub_00190680(void), sub_00190AC0(void), sub_0018AC60(void);

static void prepare(uint32_t argument, uint32_t pc, uint32_t incoming_eax, uint32_t result)
{
    g_esp = 0x10000u;
    g_eax = incoming_eax;
    g_ecx = 0x12345678u;
    g_edx = 0x87654321u;
    expected_argument = argument;
    expected_pc = pc;
    expected_eax = incoming_eax;
    callee_result = result;
    guest_write32(g_esp + 4u, argument);
}
static void preserved_call_effects(void)
{
    CHECK(g_esp == 0x10004u);
    CHECK(g_ecx == 0xABCDEF01u && g_edx == 0x10203040u);
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    prepare(4u, 0x190687u, 0x76543210u, 4u);
    sub_00190680();
    CHECK(g_eax == 3u);
    preserved_call_effects();
    for (uint32_t value = 0u; value < 256u; value++) {
        prepare(value, 0x190ACAu, value, 0xAABBCC00u | value);
        sub_00190AC0();
        CHECK(g_eax == value);
        preserved_call_effects();
    }
    prepare(7u, 0x18AC6Au, 7u, 0u);
    sub_0018AC60();
    CHECK(g_eax == 0u);
    preserved_call_effects();
    prepare(7u, 0x18AC6Au, 7u, 0x20000u);
    guest_write32(0x2007Cu, 0x30000u);
    guest_write32(0x30114u, 0xFFFFFFFFu);
    sub_0018AC60();
    CHECK(g_eax == 0x20000u && g_ecx == 0x30000u);
    CHECK(g_edx == 0x10203040u && g_esp == 0x10004u);
    CHECK(guest_read32(0x30114u) == 1u);
    CHECK(calls == 259u);
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
