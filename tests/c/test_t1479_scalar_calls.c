/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original scalar caller controls. Callees are explicitly instruction-
 * transcribed native controls; proof must use authentic lifted callees.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define ROOT_STACK 0x00010000u

static void live_flag_leaf_control(void)
{
    CHECK(g_esp == ROOT_STACK - 4u && guest_read32(g_esp) == 0x0035E635u);
    CHECK(g_eax == 0x11223344u && g_ecx == 0x22334455u && g_edx == 0x33445566u);
    g_ecx = guest_read32(0x00774024u);
    g_eax = g_ecx == 1u ? 1u : 0u;
    g_esp += 4u;
    calls++;
}
static void occupied_slot_leaf_control(void)
{
    CHECK(g_esp == ROOT_STACK - 4u);
    CHECK(guest_read32(g_esp) == (calls == 0u ? 0x00356325u : 0x00356335u));
    if (calls == 0u) {
        CHECK(g_eax == 0x11223344u && g_ecx == 0x22334455u && g_edx == 0x33445566u);
    } else {
        CHECK(g_ecx == 16u - g_eax && g_edx == 0x33445501u);
    }
    g_eax = 0u;
    g_ecx = 0x007744C0u;
    g_edx = (g_edx & 0xFFFFFF00u) | 1u;
    do {
        if ((guest_read8(g_ecx - 0x30Cu) & (uint8_t)g_edx) != 0u) g_eax++;
        if ((guest_read8(g_ecx) & (uint8_t)g_edx) != 0u) g_eax++;
        if ((guest_read8(g_ecx + 0x30Cu) & (uint8_t)g_edx) != 0u) g_eax++;
        if ((guest_read8(g_ecx + 0x618u) & (uint8_t)g_edx) != 0u) g_eax++;
        g_ecx += 0xC30u;
    } while (g_ecx < 0x00777580u);
    g_esp += 4u;
    calls++;
}
game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x0035E960u: return live_flag_leaf_control;
    case 0x003562E0u: return occupied_slot_leaf_control;
    default: return NULL;
    }
}
extern void sub_0035E630(void), sub_00356320(void);
static void prepare(uint32_t root)
{
    g_esp = ROOT_STACK;
    g_eax = 0x11223344u; g_ecx = 0x22334455u; g_edx = 0x33445566u;
    g_ebx = 0x44556677u; g_ebp = 0x55667788u;
    g_esi = 0x66778899u; g_edi = 0x778899AAu;
    guest_write32(ROOT_STACK, 0x12345678u);
    guest_write32(ROOT_STACK - 4u, 0xC0DE0001u ^ root);
    calls = 0u;
}
static void check_preserved_state(void)
{
    CHECK(g_esp == ROOT_STACK + 4u && guest_read32(ROOT_STACK) == 0x12345678u);
    CHECK(g_ebx == 0x44556677u && g_ebp == 0x55667788u);
    CHECK(g_esi == 0x66778899u && g_edi == 0x778899AAu);
}
static const uint32_t sum_rows[6][4] = {
    {0u, 0x11223344u, 0x55667788u, 0x99AABBCCu},
    {1u, 0u, 0u, 0u}, {1u, 0xFFFFFFFFu, 1u, 1u},
    {1u, 0x80000000u, 0x7FFFFFFFu, 2u},
    {2u, 0x13579BDFu, 0x2468ACE0u, 0xFFFFFFFFu}, {0xFFFFFFFFu, 7u, 11u, 13u},
};
static void test_sum(unsigned ordinal)
{
    prepare(0x0035E630u);
    static const uint32_t fields[4] = {0x00774024u, 0x0076BBBCu, 0x0076BBC0u, 0x0076BBC4u};
    for (unsigned j = 0u; j < 4u; j++) guest_write32(fields[j], sum_rows[ordinal][j]);
    const uint32_t guards[4] = {0x00774020u, 0x00774028u, 0x0076BBB8u, 0x0076BBC8u};
    for (unsigned j = 0u; j < 4u; j++) guest_write32(guards[j], 0x13579BDFu ^ guards[j]);
    sub_0035E630();
    const int live = ordinal == 1u || ordinal == 2u || ordinal == 3u;
    const uint32_t expected[6] = {16u, 0u, 1u, 1u, 16u, 16u};
    CHECK(g_eax == expected[ordinal]);
    CHECK(g_ecx == (live ? sum_rows[ordinal][2] : sum_rows[ordinal][0]));
    CHECK(g_edx == 0x33445566u && calls == 1u);
    CHECK(guest_read32(ROOT_STACK - 4u) == 0x0035E635u);
    for (unsigned j = 0u; j < 4u; j++) CHECK(guest_read32(fields[j]) == sum_rows[ordinal][j]);
    for (unsigned j = 0u; j < 4u; j++) CHECK(guest_read32(guards[j]) == (0x13579BDFu ^ guards[j]));
    check_preserved_state();
}
static uint8_t slot_input(unsigned ordinal, unsigned index)
{
    const int active = ordinal == 1u || (ordinal == 2u && index % 2u == 0u) ||
                       (ordinal == 3u && index == 15u);
    return (uint8_t)(0xA4u | (unsigned)active);
}
static void test_free_slots(unsigned ordinal)
{
    prepare(0x00356320u);
    for (unsigned index = 0u; index < 16u; index++) {
        const uint32_t address = 0x007741B4u + 0x30Cu * index;
        guest_write8(address, slot_input(ordinal, index));
        const uint32_t neighbors[4] = {address - 1u, address + 1u, address + 2u, address + 3u};
        for (unsigned j = 0u; j < 4u; j++) {
            guest_write8(neighbors[j], (uint8_t)(0x5Au ^ neighbors[j]));
        }
    }
    sub_00356320();
    const uint32_t expected[4] = {16u, 0u, 8u, 15u};
    CHECK(g_eax == expected[ordinal] && g_ecx == expected[ordinal]);
    CHECK(g_edx == 0x33445501u && calls == (ordinal == 1u ? 1u : 2u));
    CHECK(guest_read32(ROOT_STACK - 4u) == (ordinal == 1u ? 0x00356325u : 0x00356335u));
    for (unsigned index = 0u; index < 16u; index++) {
        const uint32_t address = 0x007741B4u + 0x30Cu * index;
        CHECK(guest_read8(address) == slot_input(ordinal, index));
        const uint32_t neighbors[4] = {address - 1u, address + 1u, address + 2u, address + 3u};
        for (unsigned j = 0u; j < 4u; j++) {
            CHECK(guest_read8(neighbors[j]) == (uint8_t)(0x5Au ^ neighbors[j]));
        }
    }
    check_preserved_state();
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned ordinal = 0u; ordinal < 6u; ordinal++) test_sum(ordinal);
    for (unsigned ordinal = 0u; ordinal < 4u; ordinal++) test_free_slots(ordinal);
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
