/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 validated signed-table bodies. Original contract: docs/evidence/t1479/native-signed-table-predecl.md.
 */
#include "game_replace.h"

extern __thread uint32_t g_esi;

static void call_one(uint32_t target, uint32_t return_pc,
                     uint32_t local_bytes, uint32_t argument)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, local_bytes,
                        0u, 0u, &argument, 1u) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

void t1479_candidate_338150(void)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    call_one(0x45590u, 0x33815Bu, 4u, g_esi);
    guest_write32(0x765E4Cu, g_esi);
    g_esi = guest_read32(g_esp - 4u);
}

void t1479_candidate_2d2c10(void)
{
    g_eax = guest_read32(0x78AD2Cu);
    if (g_eax != 6u && g_eax != 7u) {
        g_eax = guest_read8(0x78ADAEu);
        return;
    }
    g_eax = guest_read32(0x78AD40u);
    call_one(0x22E050u, 0x2D2C32u, 0u, g_eax);
    g_eax = guest_read32(g_eax + 4u);
}

void t1479_candidate_2d2c40(void)
{
    g_eax = guest_read32(0x78AD2Cu);
    if (g_eax != 6u && g_eax != 7u) {
        const uint32_t byte = guest_read8(0x78ADACu);
        g_eax = byte < 128u ? byte : byte | 0xFFFFFF00u;
        return;
    }
    g_eax = guest_read32(0x78AD40u);
    call_one(0x22E050u, 0x2D2C62u, 0u, g_eax);
    g_eax = guest_read32(g_eax + 8u);
}
