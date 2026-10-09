/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary bounded predecessor-table/frame controls with an explicit double.
 * No saved-frame aliases, fault probes or original execution.
 */
#include "game_replace.h"
#include <stdio.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls;
static uint32_t entry_stack, object, selected;
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)
extern void t1478_draft_predecessor_select(void);
static void controlled_select(void)
{
    CHECK(g_esp == entry_stack - 20u);
    CHECK(guest_read32(g_esp) == 0x001BA20Du);
    CHECK(guest_read32(g_esp + 4u) == object && guest_read32(g_esp + 8u) == selected);
    CHECK(g_eax == selected && g_edx == 6u && g_esi == object);
    CHECK(g_ecx == 0x004FA128u + selected * 0x3Cu);
    CHECK(g_ebx == 0xBBBBBB80u);
    guest_write32(object + 0x94u, selected);
    g_eax = 0xAAAAAAAAu;
    g_ecx = 0xCCCCCCCCu;
    g_edx = 0xDDDDDDDDu;
    g_esp += 4u;
    calls++;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x001BA130u);
    return va == 0x001BA130u ? controlled_select : NULL;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const int32_t sentinels[] = {5, 2, -1};
    for (unsigned frame = 0u; frame < 2u; frame++) {
        for (unsigned bit = 0u; bit < 2u; bit++) {
            for (unsigned gate = 0u; gate < 2u; gate++) {
                for (unsigned flags = 0u; flags < 2u; flags++) {
                    for (unsigned sentinel = 0u; sentinel < 3u; sentinel++) {
                        entry_stack = 0x10000u + frame * 0x400u;
                        object = 0xD90000u;
                        selected = (uint32_t)sentinels[sentinel];
                        g_esp = entry_stack;
                        g_eax = 0x11111111u;
                        g_ecx = 0x22222222u;
                        g_edx = 0x33333333u;
                        g_ebx = 0xBBBBBBBBu;
                        g_esi = 0xEEEEEEEEu;
                        guest_write32(entry_stack, 0x00ABCDEFu);
                        guest_write32(entry_stack + 4u, object);
                        guest_write32(object + 0x94u, 6u);
                        guest_write32(object + 0x80u, gate);
                        guest_write32(0x007DE458u, flags ? 0x1000u : 0u);
                        for (int32_t row = -2; row <= 8; row++)
                            guest_write8(0x004FA128u + (uint32_t)row * 0x3Cu,
                                         row == sentinels[sentinel] ? 0x80u : 0u);
                        guest_write8(0x004FA128u + 6u * 0x3Cu, bit ? 4u : 0u);
                        unsigned prior_calls = calls;
                        t1478_draft_predecessor_select();
                        unsigned called = bit && gate && !flags;
                        uint32_t expected_ecx = !bit ? 6u * 0x3Cu : !gate ? 0u : 0x1000u;
                        CHECK(calls == prior_calls + called);
                        CHECK(g_eax == (called ? 0xAAAAAAAAu : 6u));
                        CHECK(g_ecx == (called ? 0xCCCCCCCCu : expected_ecx));
                        CHECK(g_edx == (called ? 0xDDDDDDDDu : 6u));
                        CHECK(g_ebx == 0xBBBBBBBBu && g_esi == 0xEEEEEEEEu);
                        CHECK(g_esp == entry_stack + 4u);
                        CHECK(guest_read32(object + 0x94u) == (called ? selected : 6u));
                        CHECK(guest_read32(entry_stack) == 0x00ABCDEFu);
                        CHECK(guest_read32(entry_stack - 4u) == 0xBBBBBBBBu);
                        CHECK(guest_read32(entry_stack - 8u) == 0xEEEEEEEEu);
                        if (called) {
                            CHECK(guest_read32(entry_stack - 12u) == selected);
                            CHECK(guest_read32(entry_stack - 16u) == object);
                            CHECK(guest_read32(entry_stack - 20u) == 0x001BA20Du);
                        }
                    }
                }
            }
        }
    }
    printf("T1478 unregistered predecessor-select draft: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
