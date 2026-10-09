/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1478: two legacy-vector census admissions. Original access and register order,
 * including signed count comparison, aliased input fields and saved ESI stack
 * word, is retained. See docs/t1478-vector-census.md for fixed proofs and limits. */
#include "game_replace.h"
typedef union T1478VectorXmm {
    uint32_t u[4];
    uint64_t q[2];
} T1478VectorXmm;
__thread T1478VectorXmm g_xmm0 __attribute__((weak, aligned(8)));

/* Append input fields to the stride-0x20 record table if its signed count < 18.
 * EAX is the new record address on append; unchanged when already full.
 * ECX ends at zero, EDX is the incremented count, ESI is restored. */
GAME_REPLACE_EXACT(0025F9C0, cdecl, 1, u32,
                  game_table_78b2e0_append_entry_stride_0x20_max_0x12)
{
    g_edx = guest_read32(0x0078B520u);
    if ((int32_t)g_edx >= 18) return;
    g_ecx = guest_read32(g_esp + 4u);
    g_xmm0.q[0] = 0u;
    g_xmm0.q[1] = 0u;
    g_esp -= 4u;
    guest_write32(g_esp, g_esi);
    g_esi = guest_read32(g_ecx);
    g_eax = g_edx;
    g_eax <<= 5;
    g_eax += 0x0078B2E0u;
    guest_write32(g_eax, g_esi);
    g_esi = guest_read32(g_ecx + 0xCu);
    guest_write32(g_eax + 4u, g_esi);
    g_esi = guest_read32(g_ecx + 0x10u);
    guest_write32(g_eax + 8u, g_esi);
    g_esi = guest_read32(g_ecx + 8u);
    guest_write32(g_eax + 0xCu, g_esi);
    g_ecx = guest_read32(g_ecx + 0x14u);
    guest_write32(g_eax + 0x1Cu, g_ecx);
    g_ecx = 0u;
    g_edx += 1u;
    guest_write32(g_eax + 0x10u, g_ecx);
    guest_write32(g_eax + 0x14u, g_ecx);
    guest_write32(g_eax + 0x18u, g_xmm0.u[0]);
    guest_write32(0x0078B520u, g_edx);
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
}

/* Request state 4 through the object extension unless its flag bit is set or
 * its current state is already 4. A memory MOVSS clears XMM0's upper lanes. */
GAME_REPLACE_EXACT(002649D0, cdecl, 1, u32,
                  game_object_ext_request_state_4_unless_flag_0x10000_or_already_state_4)
{
    g_eax = guest_read32(g_esp + 4u);
    g_eax = guest_read32(g_eax + 0x7Cu);
    g_edx = guest_read32(g_eax + 0x14u);
    g_edx &= 0x00010000u;
    g_ecx = 0u;
    g_ecx |= g_edx;
    if (g_ecx != 0u) return;
    g_edx = guest_read32(g_eax + 0x20u);
    g_ecx = 4u;
    if (g_edx == g_ecx) return;
    g_xmm0.u[0] = guest_read32(0x00475C78u);
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_eax + 0x9D0u, g_ecx);
    guest_write32(g_eax + 0x9E0u, g_xmm0.u[0]);
}
