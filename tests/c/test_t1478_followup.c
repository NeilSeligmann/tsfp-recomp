/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent frame, forward-copy overlap and ESI-spill controls. */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls;
static unsigned clear_fallback;
static uint32_t key, pc, lookup_result, out_after_call, leaf_result, expected_stack;
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

static void lookup_control(void)
{
    CHECK(guest_read32(g_esp) == pc && guest_read32(g_esp + 4u) == key);
    CHECK(g_eax == 0x11111111u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    if (pc == 0x0024D26Au) {
        /* Original caller reads this argument only AFTER the guest call. */
        guest_write32(g_esp + 12u, out_after_call);
    }
    if (clear_fallback != 0u) guest_write32(0x0075BF0Cu, 0u);
    g_eax = lookup_result;
    g_ecx = 0xAAu;
    g_edx = 0xBBu;
    g_esp += 4u;
    calls++;
}
static void leaf_control(void)
{
    CHECK(guest_read32(g_esp) == 0x00259814u);
    CHECK(g_esp == expected_stack - 8u);
    CHECK(g_esi == 0x20000u && g_eax == 0x30000u && g_ecx == 1u);
    CHECK(g_edx == 0x33333333u);
    g_eax = leaf_result;
    g_esp += 4u;
    calls++;
}
game_guest_function recomp_lookup(uint32_t va)
{
    if (va == 0x0023FF20u) return lookup_control;
    if (va == 0x00064D10u) return leaf_control;
    return NULL;
}
extern void sub_001EC8E0(void), sub_0024D260(void), sub_00259800(void);
static void prepare(void)
{
    g_eax = 0x11111111u;
    g_ecx = 0x22222222u;
    g_edx = 0x33333333u;
    g_esi = 0xEEEEEEEEu;
    g_esp = expected_stack = 0x10000u;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t modes[4] = {0u, 1u, 2u, 0xFFFFFFFFu};
    key = 0x47415330u; pc = 0x001EC8EAu;
    for (unsigned found = 0u; found < 2u; found++) {
        for (unsigned mode = 0u; mode < 4u; mode++) {
            prepare(); lookup_result = found ? 0x20000u : 0u;
            guest_write32(0x0075BF0Cu, modes[mode]);
            sub_001EC8E0();
            CHECK(g_eax == (found == 0u && mode == 1u ? 1u : 0u));
            CHECK(g_ecx == 0xAAu && g_edx == 0xBBu && g_esp == 0x10004u);
        }
    }
    prepare(); lookup_result = 0u; clear_fallback = 1u;
    guest_write32(0x0075BF0Cu, 1u);
    sub_001EC8E0();
    CHECK(g_eax == 0u && guest_read32(0x0075BF0Cu) == 0u);
    clear_fallback = 0u;
    const uint32_t patterns[4][3] = {
        {0u, 0u, 0u}, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
        {0x80000000u, 0x7FFFFFFFu, 1u}, {0x11223344u, 0x55667788u, 0x99AABBCCu}
    };
    key = 0x54524350u; pc = 0x0024D26Au;
    for (unsigned pattern = 0u; pattern < 4u; pattern++) {
        for (int offset = -4; offset <= 12; offset += 4) {
            prepare(); lookup_result = 0x20000u;
            out_after_call = 0x20064u + (uint32_t)offset;
            guest_write32(g_esp + 4u, 0x90000000u);
            for (unsigned slot = 0u; slot < 3u; slot++)
                guest_write32(0x20064u + slot * 4u, patterns[pattern][slot]);
            sub_0024D260();
            CHECK(g_eax == 1u && g_ecx == out_after_call && g_esp == 0x10004u);
            CHECK(guest_read32(out_after_call) == patterns[pattern][0]);
            CHECK(guest_read32(out_after_call + 4u) == patterns[pattern][offset == 4 ? 0 : 1]);
            CHECK(guest_read32(out_after_call + 8u) == patterns[pattern][offset == 4 || offset == 8 ? 0 : 2]);
        }
    }
    prepare(); lookup_result = 0u; out_after_call = 0x90000000u;
    guest_write32(g_esp + 4u, out_after_call);
    sub_0024D260();
    CHECK(g_eax == 0u && g_ecx == 0xAAu && g_edx == 0xBBu);
    for (unsigned nested = 0u; nested < 2u; nested++) {
        for (leaf_result = 0u; leaf_result < 2u; leaf_result++) {
            prepare(); guest_write32(g_esp + 4u, 0x20000u);
            guest_write32(0x20014u, 0x30000u);
            guest_write32(0x3007Cu, nested);
            guest_write32(0x214F8u, 0x11u);
            unsigned before = calls;
            sub_00259800();
            CHECK(g_eax == (nested != 0u && leaf_result == 0u ? 1u : 0u));
            CHECK(calls == before + nested);
            CHECK(g_esi == 0xEEEEEEEEu && g_esp == 0x10004u && g_edx == 0x33333333u);
        }
    }
    leaf_result = 0u;
    for (unsigned alias = 0u; alias < 2u; alias++) {
        prepare(); g_esp = expected_stack = 0x214FCu;
        g_esi = alias == 0u ? 0x11u : 0xFFFFFFFFu;
        guest_write32(g_esp + 4u, 0x20000u);
        guest_write32(0x20014u, 0x30000u); guest_write32(0x3007Cu, 1u);
        guest_write32(0x214F8u, 0u);
        sub_00259800();
        CHECK(g_eax == (alias == 0u ? 1u : 0u));
        CHECK(g_esi == (alias == 0u ? 0x11u : 0xFFFFFFFFu));
        CHECK(guest_read32(0x214F8u) == g_esi);
        CHECK(g_esp == expected_stack + 4u);
    }
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
