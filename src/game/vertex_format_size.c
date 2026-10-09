/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
GAME_REPLACE_EXACT(0001E8D0, cdecl, 1, u32, game_d3d_vertex_format_size_from_flags)
{
    g_ecx = game_stack_arg(0u);
    ORDER_GUEST();
    g_eax = 0u;
    if (g_ecx & 0x10u) g_eax += 4u;
    if (g_ecx & 0x1u) g_eax += 12u;
    if (g_ecx & 0x40u) g_eax += 12u;
    else if (g_ecx & 0x8000u) g_eax += 4u;
    if (g_ecx & 0x8u) g_eax += 4u;
    if (g_ecx & 0x20000u) g_eax += 4u;
    if (g_ecx & 0x4u) g_eax += 8u;
    else if (g_ecx & 0x80u) g_eax += 12u;
    if (g_ecx & 0x200u) g_eax += 8u;
    else if (g_ecx & 0x1000u) g_eax += 12u;
    if (g_ecx & 0x400u) g_eax += 8u;
    else if (g_ecx & 0x2000u) g_eax += 12u;
    if (g_ecx & 0x800u) g_eax += 8u;
    else if (g_ecx & 0x4000u) g_eax += 12u;
    if (g_ecx & 0x20u) g_eax += 8u;
    if (g_ecx & 0x100u) g_eax += 24u;
    if (g_ecx & 0x10000u) g_eax += 8u;
    ORDER_GUEST();
}
#undef ORDER_GUEST
