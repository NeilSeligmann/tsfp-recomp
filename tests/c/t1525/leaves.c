/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
static void synthetic(uint32_t global_a, uint32_t global_b, uint32_t output)
{
    g_ecx = guest_read32(global_a);
    g_eax = 3u * guest_read32(g_ecx);
    g_edx = guest_read32(g_ecx + 4);
    g_edx = 5u * guest_read32(g_edx);
    g_eax += g_edx;
    g_ecx = guest_read32(global_b);
    g_eax += guest_read32(g_ecx);
    guest_write32(output, g_eax);
}
GAME_REPLACE_EXACT(00010000, cdecl, 0, u32, synthetic_shared)
{ synthetic(0x800000, 0x800004, 0x800010); }
GAME_REPLACE_EXACT(00010040, cdecl, 0, u32, synthetic_distinct)
{ synthetic(0x800008, 0x80000C, 0x800014); }
