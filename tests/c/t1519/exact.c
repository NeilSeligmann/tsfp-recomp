/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_ebx;
#ifndef MUTANT
#define MUTANT 0
#endif
static void body(unsigned inputs)
{
    uint32_t ecx = g_ecx, esi = g_esi;
    if (MUTANT == 1) { uint32_t swap = ecx; ecx = esi; esi = swap; }
    if (MUTANT == 2) esi = g_edx;
    if (MUTANT == 3) esi = 0;
    uint32_t result = (inputs & 1 ? 3u * ecx : 0u) +
                      (inputs & 2 ? 5u * esi : 0u) +
                      7u * game_stack_arg(0) + 11u * game_stack_arg(1);
    g_eax = result;
    guest_write32(g_ebx, result);
    g_ecx ^= 0x13579BDFu;
    g_esi += 0x2468ACE0u;
    if (MUTANT == 4) g_esp += 4;
}
GAME_REPLACE_EXACT_INPUTS(00010000, cdecl, 2, u32, ecx, ecx_cdecl) { body(1); }
GAME_REPLACE_EXACT_INPUTS(00010010, stdcall, 2, u32, ecx, ecx_stdcall) { body(1); }
GAME_REPLACE_EXACT_INPUTS(00010020, cdecl, 2, u32, esi, esi_cdecl) { body(2); }
GAME_REPLACE_EXACT_INPUTS(00010030, stdcall, 2, u32, esi, esi_stdcall) { body(2); }
GAME_REPLACE_EXACT_INPUTS(00010040, cdecl, 2, u32, ecx_esi, both_cdecl) { body(3); }
GAME_REPLACE_EXACT_INPUTS(00010050, stdcall, 2, u32, ecx_esi, both_stdcall) { body(3); }
GAME_REPLACE_EXACT_INPUTS(00010060, cdecl, 2, void, esi, esi_void) { body(2); }
