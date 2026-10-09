/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1475 live-call wrappers. Callees execute their original guest bodies through
 * dispatch; frame layout and return PCs match retail, including saved locals.
 * See docs/t77-band-0-80000.md for proof scope and dispositions. */
#include "game_replace.h"

static void call_guest(uint32_t va, uint32_t pc, uint32_t locals,
                       const uint32_t *args, unsigned count)
{
    if (game_guest_call(va, GAME_CC_cdecl, pc, locals, 0u, 0u, args, count) !=
        GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

GAME_REPLACE_EXACT(00012FE0, cdecl, 2, u32, game_pair_table_74c368_entry_addr_wrapper_c)
{
    const uint32_t args[2] = {game_stack_arg(0u), game_stack_arg(1u)};
    g_eax = args[1];
    g_ecx = args[0];
    call_guest(0x0015BD00u, 0x00012FEFu, 0u, args, 2u);
    g_eax = guest_read32(g_eax + 8u);
}


GAME_REPLACE_EXACT(000662D0, cdecl, 2, u32, game_wrapper_66280_arg_0)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    const uint32_t args[3] = {g_ecx, 0u, g_eax};
    call_guest(0x00066280u, 0x000662E1u, 0u, args, 3u);
}


GAME_REPLACE_EXACT(000662F0, cdecl, 2, u32, game_wrapper_66280_arg_1)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    const uint32_t args[3] = {g_ecx, 1u, g_eax};
    call_guest(0x00066280u, 0x00066301u, 0u, args, 3u);
}


GAME_REPLACE_EXACT(000425A0, cdecl, 0, u32, game_state_6b98a4_enter_4_from_3_or_5_store_elapsed_in_6f1e10_field_0xc)
{
    if ((guest_read32(0x007DE458u) & 0x00800000u) != 0u) {
        return;
    }
    g_eax = guest_read32(0x006B98A4u);
    if (g_eax != 3u && g_eax != 5u) {
        return;
    }
    g_eax = guest_read32(0x006B94C0u) - guest_read32(0x006B989Cu);
    const uint32_t args[1] = {g_eax};
    call_guest(0x00069EA0u, 0x000425CCu, 0u, args, 1u);
    guest_write32(0x006B98A4u, 4u);
}


extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

GAME_REPLACE_EXACT(00066280, cdecl, 3, u32, game_player_record_count_bits_set_in_13_bit_mask_by_group_and_sub_index)
{
    const uint32_t saved_ebx = g_ebx, saved_ebp = g_ebp;
    const uint32_t saved_esi = g_esi, saved_edi = g_edi;
    guest_write32(g_esp - 4u, saved_ebx);
    guest_write32(g_esp - 8u, saved_ebp);
    guest_write32(g_esp - 12u, saved_esi);
    guest_write32(g_esp - 16u, saved_edi);
    g_eax = game_stack_arg(1u) + 0xFFu;
    g_ecx = game_stack_arg(2u);
    g_ebx = game_stack_arg(0u);
    g_esi = 4u * (g_ecx + g_eax * 3u);
    g_ebp = 0u;
    for (g_edi = 0u; g_edi < 13u; g_edi++) {
        const uint32_t args[1] = {g_ebx};
        call_guest(0x00069900u, 0x000662A7u, 16u, args, 1u);
        g_ecx = guest_read32(g_eax + g_esi);
        g_edx = 1u << g_edi;
        if ((g_edx & g_ecx) != 0u) {
            g_ebp++;
        }
    }
    g_eax = g_ebp;
    g_ebx = guest_read32(g_esp - 4u);
    g_ebp = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 12u);
    g_edi = guest_read32(g_esp - 16u);
}

/* Preserve the global read before the ESI spill: the global can alias that slot. */
GAME_REPLACE_EXACT(00071A60, cdecl, 1, u32, game_selected_object_chain_or_field198)
{
    g_eax = guest_read32(0x007BB1E0u);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    if (g_eax != 0u) {
        g_ecx = guest_read32(g_eax + 4u);
        if (g_ecx == 0u && guest_read32(0x007BB1E8u) == g_esi) {
            g_ecx = guest_read32(g_eax + 0x60u);
            g_edx = guest_read32(g_eax + 0x5Cu);
            const uint32_t args[3] = {g_esi, g_edx, g_ecx};
            call_guest(0x00071970u, 0x00071A8Bu, 4u, args, 3u);
            if (g_eax != 0u) {
                g_esi = guest_read32(g_esp - 4u);
                return;
            }
        }
    }
    g_eax = guest_read32(g_esi + 0x198u);
    g_esi = guest_read32(g_esp - 4u);
}

/* Resolve each matching index through the real leaf, preserving live counts. */
GAME_REPLACE_EXACT(000657A0, cdecl, 3, u32, game_record_other_key_with_different_tag)
{
    g_eax = guest_read32(0x007A2958u);
    g_ecx = guest_read32(0x00790950u);
    guest_write32(g_esp - 4u, g_ebx);
    guest_write32(g_esp - 8u, g_ebp);
    guest_write32(g_esp - 12u, g_esi);
    g_eax += g_ecx;
    g_esi = 0u;
    guest_write32(g_esp - 16u, g_edi);
    if ((int32_t)g_eax > 0) {
        g_edi = game_stack_arg(2u);
        g_ebx = game_stack_arg(1u);
        g_ebp = game_stack_arg(0u);
        do {
            if (g_ebp != g_esi) {
                const uint32_t args[1] = {g_esi};
                call_guest(0x00063EA0u, 0x000657CDu, 16u, args, 1u);
                g_ecx = guest_read32(g_eax + 0x58u);
                if (g_ecx == g_ebx) {
                    call_guest(0x00063EA0u, 0x000657DDu, 16u, args, 1u);
                    g_edx = (uint32_t)(int32_t)(int8_t)guest_read8(g_eax + 1u);
                    if (g_edx != g_edi) {
                        g_edi = guest_read32(g_esp - 16u);
                        g_esi = guest_read32(g_esp - 12u);
                        g_ebp = guest_read32(g_esp - 8u);
                        g_eax = 1u;
                        g_ebx = guest_read32(g_esp - 4u);
                        return;
                    }
                }
            }
            g_eax = guest_read32(0x007A2958u);
            g_ecx = guest_read32(0x00790950u);
            g_esi++;
            g_eax += g_ecx;
        } while ((int32_t)g_esi < (int32_t)g_eax);
    }
    g_edi = guest_read32(g_esp - 16u);
    g_esi = guest_read32(g_esp - 12u);
    g_ebp = guest_read32(g_esp - 8u);
    g_eax = 0u;
    g_ebx = guest_read32(g_esp - 4u);
}
