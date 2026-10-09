/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1477 exact live-call adapters. See docs/t77-band-100000-180000-calls.md.
 * Guest save/scan/call ordering is observable for aliased string/frame inputs.
 */
#include "game_replace.h"

extern __thread uint32_t g_esi;

GAME_REPLACE_EXACT(00150530, cdecl, 1, u32, game_crc32_cstring)
{
    g_edx = game_stack_arg(0u);
    g_eax = g_edx;
    const uint32_t saved_esi_address = g_esp - 4u;
    guest_write32(saved_esi_address, g_esi);
    g_esi = g_eax + 1u;
    do {
        g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_eax);
        g_eax += 1u;
    } while ((g_ecx & 0xFFu) != 0u);
    g_eax -= g_esi;
    const uint32_t args[2] = {g_edx, g_eax};
    if (game_guest_call(0x001504F0u, GAME_CC_cdecl, 0x00150550u,
                        4u, 0u, 0u, args, 2u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
    g_esi = guest_read32(saved_esi_address);
}

GAME_REPLACE_EXACT(00150560, cdecl, 3, u32, game_crc32_buffer_equals)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    const uint32_t args[2] = {g_ecx, g_eax};
    if (game_guest_call(0x001504F0u, GAME_CC_cdecl, 0x0015056Fu,
                        0u, 0u, 0u, args, 2u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
    /* The original reads the expected CRC after the live buffer helper returns. */
    g_eax = g_eax == game_stack_arg(2u) ? 1u : 0u;
}

GAME_REPLACE_EXACT(00115F10, cdecl, 1, u32, game_player_by_id_0xca_to_0xcd_get_field_0x14_0x34)
{
    g_eax = game_stack_arg(0u);
    if (g_eax - 202u < 4u) {
        g_eax -= 202u;
        const uint32_t args[1] = {g_eax};
        if (game_guest_call(0x00356800u, GAME_CC_cdecl, 0x00115F2Du,
                            0u, 0u, 0u, args, 1u) != GAME_GUEST_CALL_OK) {
            __builtin_trap();
        }
        g_eax *= 0x1584u;
        g_ecx = guest_read32(0x007B0C48u);
        g_eax += g_ecx;
        g_eax = guest_read32(g_eax + 0x14u);
        g_eax = guest_read32(g_eax + 0x34u);
    }
}

GAME_REPLACE_EXACT(00160B80, cdecl, 1, u32, game_object_record_entry_715f2c_by_field_0x678_or_mode_a_override)
{
    g_eax = game_stack_arg(0u);
    g_edx = guest_read32(0x00715F2Cu);
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(0x007DE455u);
    const uint32_t saved_esi_address = g_esp - 4u;
    guest_write32(saved_esi_address, g_esi);
    g_esi = (guest_read32(g_eax + 0x678u) << 5) + g_edx;
    if ((g_ecx & 0xFFu) == 0x0Au) {
        g_eax = guest_read32(g_eax + 0xBF0u);
        if (g_eax != 0u) {
            const uint32_t args[1] = {guest_read32(g_eax + 0x10u)};
            g_eax = args[0];
            if (game_guest_call(0x0009CCA0u, GAME_CC_cdecl, 0x00160BB4u,
                                4u, 0u, 0u, args, 1u) != GAME_GUEST_CALL_OK) {
                __builtin_trap();
            }
            if (g_eax != 0u) {
                g_ecx = guest_read32(g_eax + 4u);
                if ((g_ecx & 0x0800u) != 0u) {
                    int16_t entry_word;
                    memcpy(&entry_word, game_host_ptr(g_eax + 8u), sizeof entry_word);
                    g_ecx = (uint32_t)(int32_t)entry_word;
                    if (guest_read32(g_esi) == g_ecx) {
                        g_esi = guest_read32(saved_esi_address);
                        return;
                    }
                }
            }
        }
    }
    g_eax = g_esi;
    g_esi = guest_read32(saved_esi_address);
}
