/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1478 exact guest-call adapters. See the band receipt for linked callee scope. */
#include "game_replace.h"

static void call_cdecl(uint32_t target, uint32_t return_pc, uint32_t argument)
{
    const uint32_t arguments[1] = {argument};
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, 0u, 0u, 0u,
                        arguments, 1u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

static void call_cdecl_no_args(uint32_t target, uint32_t return_pc)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, 0u, 0u, 0u, NULL, 0u) !=
        GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

GAME_REPLACE_EXACT(00190680, cdecl, 0, u32, game_set_globals_7b9798_7b9794_to_4_return_3)
{
    call_cdecl(0x000827B0u, 0x00190687u, 4u);
    g_eax = 3u;
}

GAME_REPLACE_EXACT(00190AC0, cdecl, 1, u32, game_scaled_table_byte_zero_extended)
{
    g_eax = game_stack_arg(0u);
    call_cdecl(0x00024E90u, 0x00190ACAu, g_eax);
    g_eax &= 0xFFu;
}

GAME_REPLACE_EXACT(0018AC60, cdecl, 1, u32, game_object_by_group_key_set_child_0x7c_field_0x114_to_1)
{
    g_eax = game_stack_arg(0u);
    call_cdecl(0x0023FF20u, 0x0018AC6Au, g_eax);
    if (g_eax != 0u) {
        g_ecx = guest_read32(g_eax + 0x7Cu);
        guest_write32(g_ecx + 0x114u, 1u);
    }
}

GAME_REPLACE_EXACT(001EC8E0, cdecl, 0, u32, game_object_key_0x47415330_absent_and_global_75bf0c_is_1)
{
    call_cdecl(0x0023FF20u, 0x001EC8EAu, 0x47415330u);
    if (g_eax == 0u) {
        g_eax = guest_read32(0x0075BF0Cu) == 1u ? 1u : 0u;
    } else {
        g_eax = 0u;
    }
}

GAME_REPLACE_EXACT(0024D260, cdecl, 1, u32, game_get_position_of_tagged_object_pcrt)
{
    call_cdecl(0x0023FF20u, 0x0024D26Au, 0x54524350u);
    if (g_eax == 0u) {
        return;
    }
    g_edx = guest_read32(g_eax + 0x64u);
    g_ecx = game_stack_arg(0u);
    guest_write32(g_ecx, g_edx);
    g_edx = guest_read32(g_eax + 0x68u);
    guest_write32(g_ecx + 4u, g_edx);
    g_eax = guest_read32(g_eax + 0x6Cu);
    guest_write32(g_ecx + 8u, g_eax);
    g_eax = 1u;
}

extern __thread uint32_t g_esi;

GAME_REPLACE_EXACT(00259800, cdecl, 1, u32, game_player_lock_on_allowed_when_action_0x11_and_ext_field_0x7c_set)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    g_eax = guest_read32(g_esi + 0x14u);
    g_ecx = guest_read32(g_eax + 0x7Cu);
    if (g_ecx != 0u) {
        if (game_guest_call(0x00064D10u, GAME_CC_cdecl, 0x00259814u, 4u,
                            0u, 0u, NULL, 0u) != GAME_GUEST_CALL_OK) {
            __builtin_trap();
        }
        if (g_eax == 0u && guest_read32(g_esi + 0x14F8u) == 0x11u) {
            g_eax = 1u;
            g_esi = guest_read32(g_esp - 4u);
            return;
        }
    }
    g_eax = 0u;
    g_esi = guest_read32(g_esp - 4u);
}

/* T1478 third batch: scalar roots whose callees are one registered or original leaf. */

GAME_REPLACE_EXACT(00190690, cdecl, 0, u32, game_memory_unit_current_slot_7b979c_active_returns_1_else_6)
{
    call_cdecl_no_args(0x000827F0u, 0x00190695u);
    call_cdecl(0x00024E60u, 0x0019069Bu, g_eax);
    g_eax = (g_eax & 0xFFu) != 0u ? 1u : 6u;
}

GAME_REPLACE_EXACT(001CD560, cdecl, 1, u32, game_profile_bit_set_in_global_6f1e0c_word_0xbf0_or_override)
{
    g_eax = guest_read32(0x0075AEF4u);
    if (g_eax != 0u) {
        g_eax = 1u;
        return;
    }
    call_cdecl_no_args(0x00069630u, 0x001CD574u);
    g_ecx = game_stack_arg(0u);
    g_eax = guest_read32(g_eax + 0x0BF0u);
    g_edx = 1u << (g_ecx & 31u);
    g_eax = (g_eax & g_edx) != 0u ? 1u : 0u;
}

GAME_REPLACE_EXACT(0022DE90, cdecl, 0, u32, game_is_state_7de470_6_to_8_and_not_flags_bit23_and_bit6_both_set)
{
    call_cdecl_no_args(0x00064D40u, 0x0022DE95u);
    if (g_eax != 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = guest_read32(0x007DE470u);
    g_eax = (int32_t)g_eax >= 6 && (int32_t)g_eax <= 8 ? 1u : 0u;
}

/* T1478 fourth batch: guest-call roots with one linked callee family (docs/evidence/t1478/fourth-domain.md). */

extern __thread uint32_t g_edi;

/* Call a cdecl guest function while `reserved_bytes` of the caller's own pushes are still on the stack. */
static void call_cdecl_reserved(uint32_t target, uint32_t return_pc, uint32_t reserved_bytes,
                                const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, reserved_bytes, 0u, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

/* 0009E2D0 with the literal (mask, slot, callback) triple the registration roots push. The
 * original never pops these pushes between calls, so each call sits 12 bytes deeper. */
static void register_callback(uint32_t return_pc, uint32_t depth, uint32_t mask, uint32_t slot,
                              uint32_t callback)
{
    const uint32_t arguments[3] = {mask, slot, callback};
    call_cdecl_reserved(0x0009E2D0u, return_pc, 12u * depth, arguments, 3u);
}

GAME_REPLACE_EXACT(00190A80, cdecl, 0, u32, game_set_globals_7b9798_7b9794_to_0_and_clear_7b978c)
{
    call_cdecl(0x000827B0u, 0x00190A87u, 0u);
    guest_write32(0x007B978Cu, 0u);
}

GAME_REPLACE_EXACT(00190A70, cdecl, 0, u32, game_memory_unit_current_slot_7b979c_get_field_0x4)
{
    call_cdecl_no_args(0x000827F0u, 0x00190A75u);
    call_cdecl(0x00024E40u, 0x00190A7Bu, g_eax);
}

GAME_REPLACE_EXACT(001829D0, cdecl, 0, u32, game_install_four_indexed_word_handlers_173790_17e680)
{
    register_callback(0x001829E1u, 0u, 0x00400000u, 2u, 0x00173790u);
    register_callback(0x001829F2u, 1u, 0x00400000u, 3u, 0x0017E680u);
    register_callback(0x00182A03u, 2u, 0x00400000u, 4u, 0x0017C2E0u);
    register_callback(0x00182A14u, 3u, 0x00400000u, 1u, 0x00180800u);
}

GAME_REPLACE_EXACT(00249880, cdecl, 0, u32, game_register_object_class_10000000_handlers_248d90)
{
    register_callback(0x00249891u, 0u, 0x10000000u, 3u, 0x00248D90u);
    register_callback(0x002498A2u, 1u, 0x10000000u, 0u, 0x002493E0u);
    register_callback(0x002498B3u, 2u, 0x10000000u, 1u, 0x00249580u);
}

GAME_REPLACE_EXACT(00240560, cdecl, 1, u32, game_get_field_7c_of_object_found_by_23ff20)
{
    g_eax = game_stack_arg(0u);
    call_cdecl(0x0023FF20u, 0x0024056Au, g_eax);
    g_eax = g_eax != 0u ? guest_read32(g_eax + 0x7Cu) : 0u;
}

GAME_REPLACE_EXACT(001DE560, cdecl, 0, u32, game_is_object_key_0x44524157_field7c_ac_zero)
{
    call_cdecl(0x0023FF20u, 0x001DE56Au, 0x44524157u);
    g_eax = guest_read32(g_eax + 0x7Cu);
    g_edx = guest_read32(g_eax + 0xACu);
    g_ecx = g_edx == 0u ? 1u : 0u;
    g_eax = g_ecx;
}

/* 0015BA10 returns the first record whose key matches, or null. These three roots test
 * whether the record behind a fixed key has type 0x4C. ESI (and EDI for ECA00) are
 * callee-saved by the original, so the pushes are real stack writes. */
GAME_REPLACE_EXACT(001EC980, cdecl, 0, u32, game_object_key_0x47415330_type_0x4c_and_global_75bf0c_zero_missing_object_true)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = 1u;
    const uint32_t key = 0x47415330u;
    call_cdecl_reserved(0x0015BA10u, 0x001EC990u, 4u, &key, 1u);
    if (g_eax == 0u) {
        g_eax = g_esi;
    } else {
        g_ecx = guest_read32(g_eax + 0x34u);
        g_eax = 0u;
        if (g_ecx == 0x4Cu) {
            g_ecx = guest_read32(0x0075BF0Cu);
            g_eax = g_ecx == 0u ? 1u : 0u;
        }
    }
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(001EC9C0, cdecl, 0, u32, game_object_key_0x47415331_type_0x4c_and_global_75bf0c_zero_missing_object_true)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = 1u;
    const uint32_t key = 0x47415331u;
    call_cdecl_reserved(0x0015BA10u, 0x001EC9D0u, 4u, &key, 1u);
    if (g_eax == 0u) {
        g_eax = g_esi;
    } else {
        g_ecx = guest_read32(g_eax + 0x34u);
        g_eax = 0u;
        if (g_ecx == 0x4Cu) {
            g_ecx = guest_read32(0x0075BF0Cu);
            g_eax = g_ecx == 0u ? 1u : 0u;
        }
    }
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(001ECA00, cdecl, 0, u32, game_object_key_0x47415330_or_31_type_0x4c_either_of_two_objects)
{
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    g_edi = 0u;
    const uint32_t first_key = 0x47415330u;
    call_cdecl_reserved(0x0015BA10u, 0x001ECA0Eu, 8u, &first_key, 1u);
    g_esi = g_eax;
    const uint32_t second_key = 0x47415331u;
    call_cdecl_reserved(0x0015BA10u, 0x001ECA1Au, 12u, &second_key, 1u);
    const uint32_t second = g_eax;
    g_ecx = 0x4Cu;
    if (g_esi != 0u && guest_read32(g_esi + 0x34u) == g_ecx) {
        g_edi = 1u;
    }
    if (second != 0u && guest_read32(second + 0x34u) == g_ecx) {
        g_eax = 1u;
    } else {
        g_eax = g_edi;
    }
    g_edi = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 4u);
}

/* T1478 fifth batch: profile/record bit tests and table lookups (docs/evidence/t1478/fifth-domain.md). */

extern __thread uint32_t g_ebx, g_ebp;

GAME_REPLACE_EXACT(00235600, cdecl, 1, u32, game_net_slot_object_entry_set_flag_0x20000000_when_field_0x14_masks_0x1201)
{
    g_eax = game_stack_arg(0u);
    call_cdecl(0x0015BA10u, 0x0023560Au, g_eax);
    if (g_eax == 0u) {
        return;
    }
    g_edx = guest_read32(g_eax + 0x7Cu);
    g_ecx = guest_read32(g_edx + 0x14u) & 0x1201u;
    g_eax = g_ecx;
    if (g_eax != 0u) {
        guest_write32(g_edx + 0x28u, guest_read32(g_edx + 0x28u) | 0x20000000u);
    }
}

GAME_REPLACE_EXACT(00208AC0, cdecl, 0, u32, game_is_slot_4c_status_above_1_and_not_5_by_index)
{
    call_cdecl(0x00227400u, 0x00208AC7u, 4u);
    if (g_eax == 0u) {
        g_eax = 0u;
        return;
    }
    call_cdecl(0x002273C0u, 0x00208AD5u, 4u);
    g_eax = g_eax != 0u ? 0u : 1u;
}

GAME_REPLACE_EXACT(0025EF30, cdecl, 1, u32, game_player_7b0c7c_record_238_field4_bit_test_returns_2_else_flag_default)
{
    g_eax = guest_read32(0x007B0C7Cu);
    g_edx = guest_read32(g_eax);
    g_ecx = game_stack_arg(0u);
    g_edx *= 0x238u;
    g_eax = 1u << (g_ecx & 31u);
    g_ecx = guest_read32(0x0075F5D0u);
    if ((guest_read32(g_edx + g_ecx + 4u) & g_eax) != 0u) {
        g_eax = 2u;
        return;
    }
    if ((guest_read32(0x007DE45Cu) & 0x10000u) != 0u) {
        g_eax = 1u;
        return;
    }
    call_cdecl_no_args(0x00035DE60u, 0x0025EF6Bu);
    g_eax = g_eax == 0u ? 1u : 0u;
}

GAME_REPLACE_EXACT(001CD380, cdecl, 4, u32, game_profile_bit_set_in_global_6f1e0c_block_0xc00_or_per_difficulty_block)
{
    g_eax = game_stack_arg(2u);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(1u);
    guest_write32(g_esp - 8u, g_edi);
    g_edi = game_stack_arg(0u);
    if (g_eax == 0u) {
        call_cdecl_reserved(0x00069630u, 0x001CD397u, 8u, NULL, 0u);
        g_ecx = g_edi;
        g_edx = 1u << (g_ecx & 31u);
        if ((guest_read32(g_eax + g_esi * 4u + 0x0C00u) & g_edx) != 0u) {
            g_eax = 1u;
            g_edi = guest_read32(g_esp - 8u);
            g_esi = guest_read32(g_esp - 4u);
            return;
        }
    }
    guest_write32(g_esp - 12u, g_ebx);
    call_cdecl_reserved(0x00069630u, 0x001CD3B7u, 12u, NULL, 0u);
    g_edx = g_eax;
    g_eax = game_stack_arg(3u) + 0xFFu;
    g_esi = g_esi + g_eax * 3u;
    g_eax = guest_read32(g_edx + g_esi * 4u);
    g_ecx = g_edi;
    g_ebx = 1u << (g_ecx & 31u);
    g_eax = (g_eax & g_ebx) != 0u ? 1u : 0u;
    g_ebx = guest_read32(g_esp - 12u);
    g_edi = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(001CD2B0, cdecl, 2, u32, game_profile_bit_set_in_global_6f1e0c_block_0xbfc_for_difficulty_or_any_of_1_to_4)
{
    g_eax = game_stack_arg(0u);
    guest_write32(g_esp - 4u, g_ebx);
    guest_write32(g_esp - 8u, g_esi);
    g_esi = (uint32_t)((int32_t)g_eax >> 5);
    g_eax &= 0x8000001Fu;
    guest_write32(g_esp - 12u, g_edi);
    if ((g_eax & 0x80000000u) != 0u) {
        g_eax = ((g_eax - 1u) | 0xFFFFFFE0u) + 1u;
    }
    g_edi = game_stack_arg(1u);
    g_ebx = g_eax;
    if (g_edi != 0xFFFFFFFFu) {
        call_cdecl_reserved(0x00069630u, 0x001CD30Fu, 12u, NULL, 0u);
        g_edx = g_esi + g_edi * 4u;
        g_eax = guest_read32(g_eax + g_edx * 4u + 0x0BFCu);
        g_ecx = g_ebx;
        g_esi = 1u << (g_ecx & 31u);
        g_eax = (g_eax & g_esi) != 0u ? 1u : 0u;
        g_edi = guest_read32(g_esp - 12u);
        g_esi = guest_read32(g_esp - 8u);
        g_ebx = guest_read32(g_esp - 4u);
        return;
    }
    guest_write32(g_esp - 16u, g_ebp);
    g_edi = 1u;
    g_ebp = g_edi;
    g_ecx = g_ebx;
    g_ebp <<= g_ecx & 31u;
    g_esi = g_esi * 4u + 0x0C0Cu;
    for (;;) {
        call_cdecl_reserved(0x00069630u, 0x001CD2EBu, 16u, NULL, 0u);
        if ((guest_read32(g_eax + g_esi) & g_ebp) != 0u) {
            g_eax = 1u;
            break;
        }
        g_edi += 1u;
        g_esi += 0x10u;
        if ((int32_t)g_edi >= 5) {
            g_eax = 0u;
            break;
        }
    }
    g_ebp = guest_read32(g_esp - 16u);
    g_edi = guest_read32(g_esp - 12u);
    g_esi = guest_read32(g_esp - 8u);
    g_ebx = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(001CD590, cdecl, 1, u32, game_set_profile_bit_in_word_0xbf0_for_all_players_5236c4)
{
    g_ecx = game_stack_arg(0u);
    g_eax = guest_read32(0x005236C4u);
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    g_edi = 1u << (g_ecx & 31u);
    g_esi = 0u;
    while ((int32_t)g_eax > 0) {
        call_cdecl_reserved(0x00069900u, 0x001CD5AEu, 8u, &g_esi, 1u);
        g_edx = guest_read32(g_eax + 0x0BF0u);
        g_eax += 0x0BF0u;
        g_edx |= g_edi;
        guest_write32(g_eax, g_edx);
        g_eax = guest_read32(0x005236C4u);
        g_esi += 1u;
        if ((int32_t)g_esi >= (int32_t)g_eax) {
            break;
        }
    }
    g_edi = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 4u);
}

/* T1478 sixth batch: pool scans, table lookups and side-entry stamps (docs/evidence/t1478/sixth-domain.md). */

GAME_REPLACE_EXACT(00247A10, cdecl, 1, u32, game_pool_78b720_find_entry_object_by_sequence_id_230)
{
    const uint32_t pool = 0x0078B720u;
    int found = 0;
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    call_cdecl_reserved(0x00156E50u, 0x00247A1Cu, 8u, &pool, 1u);
    g_esi = g_eax;
    call_cdecl_reserved(0x00156E80u, 0x00247A28u, 12u, &pool, 1u);
    if (g_eax != 0u) {
        g_edi = game_stack_arg(0u);
        g_ecx = g_eax * 0x5Cu + g_esi + 0xCu;
        for (;;) {
            g_edx = guest_read32(g_ecx - 0x64u);
            g_ecx -= 0x5Cu;
            g_eax -= 1u;
            if (g_edx != 0u) {
                g_edx = guest_read32(g_ecx);
                if (g_edx != 0u && guest_read32(g_edx + 0x230u) == g_edi) {
                    g_eax = guest_read32(g_eax * 0x5Cu + g_esi + 0xCu);
                    found = 1;
                    break;
                }
            }
            if (g_eax == 0u) {
                break;
            }
        }
    }
    if (!found) {
        g_eax = 0u;
    }
    g_edi = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(00248910, cdecl, 1, u32, game_pool_78b720_find_entry_object_by_owner_when_state_0x1000)
{
    guest_write32(g_esp - 4u, g_ebx);
    guest_write32(g_esp - 8u, g_edi);
    g_edi = game_stack_arg(0u);
    g_eax = guest_read32(g_edi + 0x150u);
    g_ecx = guest_read32(g_eax + 0x114u);
    g_ebx = 0u;
    if (g_ecx == 0x1000u) {
        const uint32_t pool = 0x0078B720u;
        guest_write32(g_esp - 12u, g_esi);
        g_esi = 0u;
        do {
            call_cdecl_reserved(0x00156E50u, 0x0024893Eu, 12u, &pool, 1u);
            g_ecx = guest_read32(g_eax + g_esi + 4u);
            g_eax += g_esi;
            if (g_ecx != 0u && guest_read32(g_eax + 0x10u) == g_edi) {
                g_ebx = guest_read32(g_eax + 0xCu);
                break;
            }
            g_esi += 0x5Cu;
        } while ((int32_t)g_esi < 0x170);
        g_esi = guest_read32(g_esp - 12u);
    }
    g_eax = g_ebx;
    g_edi = guest_read32(g_esp - 8u);
    g_ebx = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(002346A0, cdecl, 2, u32, game_object_field_0x678_key_matches_any_of_4_keys_in_arg_at_4_8_c_10)
{
    g_eax = game_stack_arg(1u);
    if (g_eax == 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = guest_read32(g_eax + 0x678u);
    call_cdecl(0x0009A4C0u, 0x002346B4u, g_eax);
    g_edx = game_stack_arg(0u);
    for (uint32_t offset = 4u; offset <= 0xCu; offset += 4u) {
        g_ecx = guest_read32(g_edx + offset);
        if ((int32_t)g_ecx > -1 && g_eax == g_ecx) {
            g_eax = 1u;
            return;
        }
    }
    g_edx = guest_read32(g_edx + 0x10u);
    g_eax = (int32_t)g_edx > -1 && g_eax == g_edx ? 1u : 0u;
}

GAME_REPLACE_EXACT(002291E0, cdecl, 1, u32, game_trigger_record_condition_met_by_timer_or_entry_key)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    if ((guest_read32(g_esi + 0x28u) & 0x40u) != 0u) {
        g_edx = guest_read32(g_esi + 0x50u);
        g_edx *= guest_read32(0x004E8CECu);
        g_eax = guest_read32(0x007B0C7Cu);
        g_ecx = guest_read32(g_eax + 4u);
        g_eax = guest_read32(g_ecx * 4u + 0x0075C368u) > g_edx ? 1u : 0u;
    } else {
        g_eax = guest_read32(0x007356D8u);
        g_ecx = guest_read32(g_eax + 0x678u);
        call_cdecl_reserved(0x0009A4C0u, 0x0022921Eu, 4u, &g_ecx, 1u);
        g_ecx = guest_read32(g_esi + 0x50u);
        if (g_ecx == g_eax) {
            g_eax = 1u;
        } else {
            g_edx = guest_read32(0x007356D8u);
            g_eax = guest_read32(g_edx + 0x678u);
            call_cdecl_reserved(0x0009A4C0u, 0x0022923Au, 4u, &g_eax, 1u);
            g_ecx = guest_read32(g_esi + 0x54u);
            g_eax = g_ecx == g_eax ? 1u : 0u;
        }
    }
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(001B7E80, cdecl, 1, u32, game_record_29c_id_0x1f8_or_default_0x60a_0x61a_by_flag_2_else_random_0x1d2)
{
    g_eax = game_stack_arg(0u);
    if ((int32_t)g_eax < 0) {
        g_eax = 0u;
        return;
    }
    g_ecx = guest_read32(0x004F9BACu);
    g_eax *= 0x29Cu;
    g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(0x007DE455u);
    g_eax += g_ecx;
    g_ecx = guest_read32(g_eax + 0x1F8u);
    if ((g_edx & 0xFFu) != 0x0Au) {
        call_cdecl_no_args(0x00153B40u, 0x001B7EC5u);
        g_eax = 0x1D2u;
        return;
    }
    if ((int32_t)g_ecx > 0) {
        g_eax = g_ecx;
        return;
    }
    g_eax = (guest_read32(g_eax + 0xCu) & 2u) != 0u ? 0x609u : 0x61Au;
}

GAME_REPLACE_EXACT(0025F320, cdecl, 1, u32, game_char_set_ext_field_4_from_table_78b2e0_entry_by_anim_key)
{
    g_eax = game_stack_arg(0u);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = guest_read32(g_eax + 0x7Cu);
    g_eax = guest_read32(g_esi + 0xBF0u);
    g_ecx = guest_read32(g_eax + 0x6Cu);
    if (g_ecx != 0u && g_ecx == 0x00163830u) {
        g_ecx = guest_read32(g_eax + 0x38u);
        call_cdecl_reserved(0x0025FA10u, 0x0025F346u, 4u, &g_ecx, 1u);
        g_eax = (g_eax << 5) + 0x0078B2E0u;
        g_eax = guest_read32(g_eax + 0x10u);
        if (g_eax != 0u && guest_read32(g_eax + 0x20u) == 8u) {
            guest_write32(g_esi + 4u, g_eax);
        } else {
            guest_write32(g_esi + 4u, 0u);
        }
    }
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(00275640, cdecl, 1, u32, game_stamp_side_entry_74c3b8_when_table_7a2be4_flag_changes_in_mode_2)
{
    g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(0x007DE402u);
    if ((g_eax & 0xFFu) == 0u) {
        return;
    }
    g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(0x007DE455u);
    g_eax = (g_edx & 0xFFu) == 2u ? 1u : 0u;
    if (g_eax == 0u) {
        return;
    }
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    call_cdecl_reserved(0x00274380u, 0x00275666u, 4u, &g_esi, 1u);
    g_ecx = guest_read32(0x0074C3B8u);
    g_eax = g_eax != 0u ? 0u : 1u;
    g_esi <<= 4;
    g_edx = guest_read32(g_esi + g_ecx - 8u);
    if (g_eax != g_edx) {
        guest_write32(g_esi + g_ecx - 8u, g_eax);
        g_eax = guest_read32(0x0074C3B8u);
        g_ecx = guest_read32(0x007DE318u);
        guest_write32(g_esi + g_eax - 4u, g_ecx);
    }
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(00275EF0, cdecl, 0, u32, game_stamp_side_entries_74c3b8_for_both_sides)
{
    if (guest_read32(0x007DE30Cu) != 7u) {
        return;
    }
    g_eax = guest_read32(0x004E7944u);
    if ((int32_t)g_eax <= 0) {
        return;
    }
    const uint32_t first_side = 1u;
    call_cdecl_reserved(0x00275640u, 0x00275F09u, 0u, &first_side, 1u);
    const uint32_t second_side = 2u;
    call_cdecl_reserved(0x00275640u, 0x00275F10u, 4u, &second_side, 1u);
    g_eax = guest_read32(0x0075F5E4u);
    g_ecx = guest_read32(0x00790950u);
    g_eax += 1u;
    guest_write32(0x0075F5E4u, g_eax);
    if (g_eax == g_ecx) {
        guest_write32(0x0075F5E4u, 0u);
    }
}

GAME_REPLACE_EXACT(0022DE00, cdecl, 0, u32, game_current_player_selection_key_to_id_by_state_6_to_8)
{
    g_eax = guest_read32(0x007DE470u) - 6u;
    if (g_eax > 2u) {
        g_eax = 0xFFFFFFFFu;
        return;
    }
    /* Selector 6, 7 and 8 use table 50E94C, 50EA0C and 50EA8C. Each path loads the
     * player's row through 7B0C7C and 75CC90 with a different register assignment that the
     * callee (9CC40) can observe, so the assignments are reproduced exactly. */
    uint32_t key;
    uint32_t return_pc;
    if (g_eax == 2u) {
        g_eax = guest_read32(0x007B0C7Cu);
        g_ecx = guest_read32(g_eax);
        g_edx = guest_read32(0x0075CC90u);
        g_ecx *= 0x34u;
        g_eax = guest_read32(g_ecx + g_edx + 4u);
        g_eax <<= 5;
        g_ecx = guest_read32(g_eax + 0x0050EA8Cu);
        key = g_ecx;
        return_pc = 0x0022DE37u;
    } else if (g_eax == 1u) {
        g_edx = guest_read32(0x007B0C7Cu);
        g_eax = guest_read32(g_edx);
        g_ecx = guest_read32(0x0075CC90u);
        g_eax *= 0x34u;
        g_edx = guest_read32(g_eax + g_ecx + 4u);
        g_edx <<= 5;
        g_eax = guest_read32(g_edx + 0x0050EA0Cu);
        key = g_eax;
        return_pc = 0x0022DE5Fu;
    } else {
        g_ecx = guest_read32(0x007B0C7Cu);
        g_edx = guest_read32(g_ecx);
        g_eax = guest_read32(0x0075CC90u);
        g_edx *= 0x34u;
        g_ecx = guest_read32(g_edx + g_eax + 4u);
        g_ecx <<= 5;
        g_edx = guest_read32(g_ecx + 0x0050E94Cu);
        key = g_edx;
        return_pc = 0x0022DE86u;
    }
    call_cdecl(0x0009CC40u, return_pc, key);
}
