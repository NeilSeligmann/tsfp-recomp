/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1476 table lookup: publish a holder's value into the object reached through the
 * 0x54-byte table entry that matches the holder's key. */
#include "game_replace.h"

extern __thread uint32_t g_esi;

/* 000AF330: cdecl(holder). Lookup 000B7600 runs as the original lifted leaf. */
GAME_REPLACE_EXACT(000AF330, cdecl, 1, u32, game_object_set_linked_7b2780_object_flags_from_ext_0x54)
{
    const uint32_t holder = game_stack_arg(0u);
    g_eax = holder;
    if (holder == 0u)
        return;
    guest_write32(g_esp - 4u, g_esi);
    g_esi = guest_read32(holder + 0x7Cu);
    g_eax = guest_read32(g_esi + 0x60u);
    const uint32_t key[1] = {g_eax};
    if (game_guest_call(0x000B7600u, GAME_CC_cdecl, 0x000AF345u, 4u, 0u, 0u, key, 1u) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
    if (g_eax != 0u) {
        g_ecx = guest_read32(g_eax + 8u);
        g_edx = guest_read32(g_ecx + 4u);
        g_eax = guest_read32(g_esi + 0x54u);
        guest_write32(g_edx + 0x30u, g_eax);
    }
    g_esi = guest_read32(g_esp - 4u);
}
