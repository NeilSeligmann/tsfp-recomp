/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 authentic scalar guest callees. Evidence and scope:
 * docs/t77-band-280000-380000-calls.md, docs/t1479-scalar-proof-plan.md.
 */
#include "game_replace.h"

static void call_scalar_leaf(uint32_t target, uint32_t return_pc)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, 0u,
                        0u, 0u, NULL, 0u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

GAME_REPLACE_EXACT(0035E630, cdecl, 0, u32,
                   game_net_total_slot_count_of_76bbbc_76bbc4_76bbc0_or_16_when_not_live)
{
    call_scalar_leaf(0x0035E960u, 0x0035E635u);
    if (g_eax != 0u) {
        g_ecx = guest_read32(0x0076BBC0u);
        g_eax = guest_read32(0x0076BBC4u);
        g_eax += g_ecx;
        g_eax += guest_read32(0x0076BBBCu);
    } else {
        g_eax = 16u;
    }
}

GAME_REPLACE_EXACT(00356320, cdecl, 0, u32,
                   game_net_count_free_player_slots_30c_of_16)
{
    call_scalar_leaf(0x003562E0u, 0x00356325u);
    g_ecx = 16u - g_eax;
    if (g_ecx != 0u && (g_ecx & 0x80000000u) == 0u) {
        call_scalar_leaf(0x003562E0u, 0x00356335u);
        g_ecx = 16u - g_eax;
        g_eax = g_ecx;
    } else {
        g_eax = 0u;
    }
}

/* Second T1479 batch (docs/evidence/t1479/second-batch-predeclaration.md). All callees are
 * original unregistered lifted leaves; the guest frame words they write are observable. */

GAME_REPLACE_EXACT(003568C0, cdecl, 1, u32, game_net_is_player_slot_connected)
{
    call_scalar_leaf(0x003533F0u, 0x003568C5u);
    if (g_eax == 0u) {
        g_eax = 1u;
        return;
    }
    g_eax = game_stack_arg(0u);
    if ((int32_t)g_eax >= 16) {
        g_eax = 0u;
        return;
    }
    g_ecx = g_eax * 0x124u;
    if ((guest_read8(g_ecx + 0x0077728Cu) & 1u) == 0u) {
        g_eax = 0u;
        return;
    }
    if (guest_read32(0x0079094Cu) == 0x65u) {
        g_eax = 1u;
        return;
    }
    g_ecx = guest_read32(0x007356D8u);
    if (g_ecx == 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = g_eax * 0xEA0u + g_ecx;
    if (g_eax == 0u || guest_read32(g_eax) == 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = guest_read32(0x007DE30Cu);
    g_eax = 1u;
}

GAME_REPLACE_EXACT(0035E6A0, cdecl, 3, u32,
                   game_net_set_slot_counts_76bbbc_76bbc4_and_pending_76bbc0_when_role_2)
{
    g_eax = game_stack_arg(0u);
    g_ecx = game_stack_arg(1u);
    guest_write32(0x0076BBBCu, g_eax);
    guest_write32(0x0076BBC4u, g_ecx);
    call_scalar_leaf(0x00355F10u, 0x0035E6B8u);
    if (g_eax == 0u) {
        return;
    }
    g_eax = game_stack_arg(2u);
    if ((int32_t)g_eax > 0) {
        guest_write32(0x0076BBC0u, g_eax);
    }
}

GAME_REPLACE_EXACT(002BFEC0, cdecl, 1, u32,
                   game_setup_78adae_is_0x13_and_arg_below_9_minus_field_c_of_context_69640)
{
    if (guest_read8(0x0078ADAEu) != 0x13u) {
        g_eax = 0u;
        return;
    }
    call_scalar_leaf(0x00069640u, 0x002BFECEu);
    g_edx = guest_read32(g_eax + 0x0Cu);
    g_eax = game_stack_arg(0u);
    g_ecx = 9u - g_edx;
    g_eax = (int32_t)g_eax < (int32_t)g_ecx ? 0u : 1u;
}

GAME_REPLACE_EXACT(002EF160, cdecl, 0, u32, game_current_mode_profile_unlock_flag_is_set)
{
    call_scalar_leaf(0x00069640u, 0x002EF165u);
    g_ecx = guest_read8(0x007DE455u);
    g_edx = guest_read8(0x007DE456u);
    g_edx <<= 5;
    g_ecx = g_eax + g_ecx * 5u * 4u + 0x24u;
    g_eax = guest_read32(g_edx + 0x004D1F84u);
    if (g_eax == 1u || (g_eax == 2u && guest_read32(g_ecx) != 0u)) {
        g_eax = 1u;
        return;
    }
    g_eax = 0u;
}

GAME_REPLACE_EXACT(0035EAF0, cdecl, 0, u32,
                   game_shared_online_advance_session_key_counter_once_when_live)
{
    call_scalar_leaf(0x0035E960u, 0x0035EAF5u);
    if (g_eax == 0u) {
        return;
    }
    g_eax = guest_read32(0x0076BD6Cu);
    if (g_eax != 0u) {
        return;
    }
    g_eax = guest_read8(0x0076BD68u);
    g_ecx = 0xFEu;
    g_edx = g_eax % 0xFEu;
    g_eax = g_eax / 0xFEu;
    guest_write32(0x0076BD6Cu, 1u);
    g_edx = (g_edx & 0xFFFFFF00u) | ((g_edx + 1u) & 0xFFu);
    guest_write8(0x0076BD68u, (uint8_t)g_edx);
}

GAME_REPLACE_EXACT(00356990, cdecl, 0, u32, game_net_is_role_2_in_state_0xa)
{
    if (guest_read32(0x0079094Cu) == 0x65u) {
        g_ecx = guest_read32(0x0076B140u);
        if (g_ecx != 2u) {
            g_eax = 0u;
            return;
        }
    } else {
        call_scalar_leaf(0x003533F0u, 0x003569BFu);
        if (g_eax == 0u || guest_read32(0x0076B140u) != 2u) {
            g_eax = 0u;
            return;
        }
    }
    g_eax = guest_read32(0x0076B1A4u) == 0x0Au ? 1u : 0u;
}

/* Third T1479 batch (docs/evidence/t1479/third-batch-predeclaration.md): net/session roots.
 * Callees are the original unregistered lifted leaves (3533F0, 35E970, 355EC0, 69640,
 * 356800, 35AF40, 358560-through-35AF40, 389B0) or the registered native 3568C0. The pushes
 * of saved registers are mirrored as guest stack writes below the entry ESP. */

extern __thread uint32_t g_esi, g_edi, g_ebx, g_ebp;

static void call_leaf_below(uint32_t target, uint32_t return_pc, uint32_t caller_bytes)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, caller_bytes,
                        0u, 0u, NULL, 0u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

static void call_one_argument(uint32_t target, uint32_t return_pc, uint32_t caller_bytes,
                              uint32_t argument)
{
    const uint32_t arguments[1] = {argument};
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, caller_bytes,
                        0u, 0u, arguments, 1u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

static uint32_t guest_read16_sign_extended(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return (uint32_t)(int32_t)(int16_t)value;
}

/* The NEG / SBB / NEG idiom: 1 when the value is non-zero, else 0. */
static uint32_t boolean_of(uint32_t value)
{
    return value != 0u ? 1u : 0u;
}

GAME_REPLACE_EXACT(002BE9B0, cdecl, 0, u32,
                   game_setup_entry_available_for_selected_mode_and_net_session)
{
    call_scalar_leaf(0x00069640u, 0x002BE9B5u);
    g_ecx = guest_read8(0x0078ADADu);
    g_edx = guest_read8(0x0078ADAEu);
    g_ecx = g_ecx * 5u;
    g_edx <<= 5;
    g_ecx = g_eax + g_ecx * 4u + 0x24u;
    g_eax = guest_read32(g_edx + 0x004D1F84u);
    if (g_eax != 1u && !(g_eax == 2u && guest_read32(g_ecx) != 0u)) {
        g_eax = 0u;
        return;
    }
    if (guest_read32(0x0079094Cu) == 0x65u) {
        g_eax = 1u;
        return;
    }
    call_scalar_leaf(0x003533F0u, 0x002BE9F0u);
    g_eax = g_eax != 0u ? 1u : 0u;
}

GAME_REPLACE_EXACT(002D28F0, cdecl, 0, u32,
                   game_descriptor_4e9a14_or_4e9a24_by_global_76bbc8_value_1_2_3)
{
    guest_write32(g_esp - 4u, g_esi);
    call_leaf_below(0x0035E970u, 0x002D28F8u, 4u);
    if (g_eax == 1u) {
        g_eax = 0x004E9A14u;
        g_esi = guest_read32(g_esp - 4u);
        return;
    }
    call_leaf_below(0x0035E970u, 0x002D2909u, 4u);
    if (g_eax == 2u) {
        g_eax = 0x004E9A24u;
        g_esi = guest_read32(g_esp - 4u);
        return;
    }
    call_leaf_below(0x0035E970u, 0x002D291Au, 4u);
    g_eax = g_eax == 3u ? 0x004E9A24u : 0u;
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(00358130, cdecl, 0, u32,
                   game_net_take_pickup_flag_when_session_client)
{
    call_scalar_leaf(0x003533F0u, 0x00358135u);
    if (g_eax == 0u) {
        return;
    }
    call_scalar_leaf(0x00355EC0u, 0x0035813Eu);
    if (g_eax != 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = guest_read32(0x00541E9Cu);
    guest_write32(0x00541E9Cu, 1u);
}

GAME_REPLACE_EXACT(00358160, cdecl, 0, u32,
                   game_net_next_pickup_sequence_id_by_mode_or_minus1)
{
    call_scalar_leaf(0x003533F0u, 0x00358165u);
    if (g_eax == 0u) {
        return;
    }
    call_scalar_leaf(0x00355EC0u, 0x0035816Eu);
    if (g_eax == 0u) {
        g_eax = guest_read32(0x007DE30Cu);
        if (g_eax == 7u || g_eax == 6u) {
            g_eax = guest_read32(0x00541EA4u);
            guest_write32(0x00541EA4u, 0xFFFFFFFFu);
            return;
        }
    }
    g_ecx = guest_read32(0x00519750u);
    g_eax = g_ecx;
    g_ecx += 1u;
    guest_write32(0x00519750u, g_ecx);
}

GAME_REPLACE_EXACT(00356080, cdecl, 1, u32,
                   game_net_player_index_in_session_by_field_0x3c)
{
    call_scalar_leaf(0x003533F0u, 0x00356085u);
    if (g_eax == 0u) {
        g_eax = game_stack_arg(0u);
        return;
    }
    g_ecx = guest_read32(0x00774028u);
    g_edx = guest_read32(g_ecx + 0x20u);
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    g_eax = 0u;
    if ((int32_t)g_edx > 0) {
        const uint32_t key = game_stack_arg(0u);
        g_ecx += 0x0Cu;
        for (;;) {
            const uint32_t record = guest_read32(g_ecx);
            if (guest_read32(record + 0x3Cu) == key) {
                return;
            }
            g_eax += 1u;
            g_ecx += 4u;
            if ((int32_t)g_eax >= (int32_t)g_edx) {
                break;
            }
        }
    }
    g_eax = 0xFFFFFFFFu;
}

GAME_REPLACE_EXACT(00356870, cdecl, 1, u32,
                   game_net_get_player_slot_field_0x3c_by_slot_or_arg_when_offline)
{
    call_scalar_leaf(0x003533F0u, 0x00356875u);
    if (g_eax != 0u) {
        g_eax = guest_read32(0x00774028u);
        if (g_eax == 0u) {
            g_eax = game_stack_arg(0u);
            return;
        }
    }
    guest_write32(g_esp - 4u, g_esi);
    call_leaf_below(0x003533F0u, 0x0035688Du, 4u);
    const uint32_t slot = game_stack_arg(0u);
    if (g_eax != 0u) {
        g_eax = guest_read32(0x00774028u);
        g_ecx = guest_read32(g_eax + slot * 4u + 0x0Cu);
        if (g_ecx == 0u) {
            g_eax = slot;
            return;
        }
    }
    call_leaf_below(0x003533F0u, 0x003568A7u, 4u);
    if (g_eax == 0u) {
        g_eax = slot;
        return;
    }
    g_ecx = guest_read32(0x00774028u);
    g_edx = guest_read32(g_ecx + slot * 4u + 0x0Cu);
    g_eax = guest_read32(g_edx + 0x3Cu);
}

GAME_REPLACE_EXACT(00358560, cdecl, 1, u32,
                   game_net_object_replicates_locally_by_player_slot_index)
{
    call_scalar_leaf(0x003533F0u, 0x00358565u);
    if (g_eax == 0u) {
        g_eax = 1u;
        return;
    }
    g_eax = game_stack_arg(0u);
    g_ecx = guest_read32(g_eax + 0x7Cu);
    g_edx = guest_read32(g_ecx + 8u);
    g_edx -= guest_read32(0x00790950u);
    if ((g_edx & 0x80000000u) != 0u) {
        g_eax = 0u;
        return;
    }
    call_scalar_leaf(0x00355EC0u, 0x00358583u);
    g_eax = boolean_of(g_eax);
}

GAME_REPLACE_EXACT(0035AFB0, cdecl, 1, u32,
                   game_net_player_respawn_replication_guard_when_not_in_local_respawn)
{
    call_scalar_leaf(0x003533F0u, 0x0035AFB5u);
    if (g_eax == 0u) {
        g_eax = 1u;
        return;
    }
    g_eax = guest_read32(0x0076B180u);
    if (g_eax != 0u) {
        g_eax = 1u;
        return;
    }
    g_eax = game_stack_arg(0u);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = guest_read32(g_eax + 0x14u);
    call_leaf_below(0x0035AF40u, 0x0035AFCFu, 4u);
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(0035B140, cdecl, 2, u32,
                   game_net_pickup_touch_replication_guard_by_pickup_kind)
{
    call_scalar_leaf(0x003533F0u, 0x0035B145u);
    if (g_eax == 0u) {
        g_eax = 1u;
        return;
    }
    g_ecx = guest_read32(0x0076B16Cu);
    g_eax = game_stack_arg(0u);
    g_eax = guest_read32(g_eax + 0x7Cu);
    if (g_ecx != 0u) {
        g_eax = 1u;
        return;
    }
    g_eax = guest_read32(g_eax + 0x0Cu);
    if (g_eax == 0x20u || g_eax == 1u) {
        call_scalar_leaf(0x00355EC0u, 0x0035B178u);
        g_eax = boolean_of(g_eax);
        return;
    }
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(1u);
    call_leaf_below(0x0035AF40u, 0x0035B171u, 4u);
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(003560E0, cdecl, 2, u32, game_net_record_124_init_at_index)
{
    call_one_argument(0x000389B0u, 0x003560E7u, 0u, 0xFFFFFFFFu);
    g_ecx = game_stack_arg(0u);
    g_edx = game_stack_arg(1u);
    g_eax = g_ecx * 0x124u;
    guest_write32(g_eax + 0x0077727Cu, g_ecx);
    guest_write32(g_eax + 0x00777284u, g_ecx);
    g_ecx = guest_read32(g_eax + 0x0077728Cu);
    g_ecx |= 1u;
    guest_write32(g_eax + 0x00777244u, g_edx);
    guest_write32(g_eax + 0x0077728Cu, g_ecx);
    guest_write32(g_eax + 0x00777360u, 0u);
    guest_write32(g_eax + 0x00777348u, 0xFFFFFFFFu);
}

/* 00359910: cdecl(pointer). Smallest signed word at record + 0xA60 over the connected slots
 * whose entry key differs from *pointer (0x7FFFFFFF when none). Saved EBX/EBP/ESI/EDI. */
GAME_REPLACE_EXACT(00359910, cdecl, 1, u32,
                   game_net_min_word_0xa60_over_connected_slots_except_arg_owner)
{
    const uint32_t entry_esp = g_esp;
    uint32_t slot = 0u;
    uint32_t minimum = 0x7FFFFFFFu;
    uint32_t record_offset = 0u;
    uint32_t entry = 0x00777244u;
    guest_write32(entry_esp - 4u, g_ebx);
    guest_write32(entry_esp - 8u, g_ebp);
    guest_write32(entry_esp - 12u, g_esi);
    guest_write32(entry_esp - 16u, g_edi);
    do {
        call_one_argument(0x003568C0u, 0x00359928u, 16u, slot);
        if (g_eax != 0u) {
            g_eax = guest_read32(entry);
            g_ecx = guest_read32(g_eax);
            g_edx = game_stack_arg(0u);
            if (g_ecx != guest_read32(g_edx)) {
                g_eax = guest_read32(0x007356D8u);
                g_eax = guest_read16_sign_extended(record_offset + g_eax + 0xA60u);
                if ((int32_t)g_eax < (int32_t)minimum) {
                    minimum = g_eax;
                }
            }
        }
        entry += 0x124u;
        slot += 1u;
        record_offset += 0xEA0u;
    } while ((int32_t)entry < (int32_t)0x00778484u);
    g_eax = minimum;
}

/* 002E7270: cdecl(value). Counts roster entries (id array 7DE4C0, [7A2958] + [790950]
 * entries, the bound re-read every iteration) whose record at [7356D8] + id * 0xEA0 is
 * non-null, whose slot ([record + 8]) 3568C0 reports connected and whose [record + 0x84]
 * equals value. Saved EBX/EBP/EDI/ESI. */
GAME_REPLACE_EXACT(002E7270, cdecl, 1, u32,
                   game_count_roster_7de4c0_entries_with_connected_net_slot)
{
    const uint32_t entry_esp = g_esp;
    uint32_t count = 0u;
    uint32_t index = 0u;
    const uint32_t wanted = game_stack_arg(0u);
    g_eax = guest_read32(0x007A2958u);
    g_ecx = guest_read32(0x00790950u);
    guest_write32(entry_esp - 4u, g_ebx);
    guest_write32(entry_esp - 8u, g_ebp);
    guest_write32(entry_esp - 12u, g_edi);
    g_eax += g_ecx;
    if ((int32_t)g_eax > 0) {
        guest_write32(entry_esp - 16u, g_esi);
        do {
            g_eax = guest_read32(0x007DE4C0u + index * 4u);
            if ((int32_t)g_eax >= 0) {
                g_ecx = guest_read32(0x007356D8u);
                g_eax = g_eax * 0xEA0u;
                g_eax += g_ecx;
                const uint32_t record = g_eax;
                if (record != 0u) {
                    g_edx = guest_read32(record + 8u);
                    call_one_argument(0x003568C0u, 0x002E72B1u, 16u, g_edx);
                    if (g_eax != 0u && guest_read32(record + 0x84u) == wanted) {
                        count += 1u;
                    }
                }
            }
            g_eax = guest_read32(0x007A2958u);
            g_ecx = guest_read32(0x00790950u);
            index += 1u;
            g_eax += g_ecx;
        } while ((int32_t)index < (int32_t)g_eax);
    }
    g_eax = count;
}

/* Fourth T1479 batch (docs/evidence/t1479/fourth-batch-predeclaration.md): session/table
 * roots. Callees are original unregistered lifted leaves except 3C8960 (registered native
 * bounded copy). Saved register pushes are mirrored as guest stack writes below the entry ESP,
 * and the call arguments and return PCs follow the original pushes. */

static void pop_saved_esi(void)
{
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(002E4C10, cdecl, 1, u32,
                   game_local_slot_flag_bit0_clear_check_by_index_with_mode_gates)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    if ((int32_t)g_esi < 0 || (int32_t)g_esi >= 4) {
        g_eax = 0u;
        pop_saved_esi();
        return;
    }
    g_eax = guest_read32(g_esi * 4u + 0x00761C48u);
    g_ecx = guest_read32(g_eax);
    if ((g_ecx & 0x200u) != 0u) {
        g_eax = 0u;
        pop_saved_esi();
        return;
    }
    if ((int32_t)guest_read32(0x0079094Cu) > 0x64 ||
        (guest_read32(0x007DE458u) & 0x20000000u) != 0u) {
        g_eax = 1u;
        pop_saved_esi();
        return;
    }
    call_leaf_below(0x0035E960u, 0x002E4C46u, 4u);
    if (g_eax != 0u && (guest_read8(0x007DE458u) & 0x40u) != 0u) {
        call_leaf_below(0x00042620u, 0x002E4C58u, 4u);
        if (g_eax == 0u) {
            pop_saved_esi();
            return;
        }
    }
    g_ecx = guest_read32(g_esi * 4u + 0x00761C48u);
    g_eax = ~guest_read32(g_ecx) & 1u;
    pop_saved_esi();
}

GAME_REPLACE_EXACT(002E4470, cdecl, 0, u32,
                   game_mode_0xa_multiplayer_pair_state_or_flag_bit9_active)
{
    if (guest_read8(0x007DE455u) != 0x0Au || (int32_t)guest_read32(0x00790950u) <= 1) {
        g_eax = 0u;
        return;
    }
    call_scalar_leaf(0x000CB0F0u, 0x002E4487u);
    if (g_eax != 0u) {
        g_eax = 1u;
        return;
    }
    g_eax = guest_read32(0x007DE458u);
    g_eax = (g_eax & 0x200u) != 0u ? 1u : 0u;
}

GAME_REPLACE_EXACT(002CB1E0, cdecl, 0, u32,
                   game_get_global_761260_after_69930_with_selected_index_78ad40)
{
    g_eax = guest_read32(0x0078AD40u);
    const uint32_t arguments[2] = {0u, g_eax};
    if (game_guest_call(0x00069930u, GAME_CC_cdecl, 0x002CB1EDu, 0u, 0u, 0u,
                        arguments, 2u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
    g_edx = guest_read32(g_eax + 0x0Cu);
    g_eax = guest_read32(0x00761260u);
    g_ecx = (int32_t)g_edx > (int32_t)g_eax ? 1u : 0u;
    g_eax = g_ecx;
}

GAME_REPLACE_EXACT(002D74B0, cdecl, 1, u32, game_any_of_4_input_pads_has_button_mask_arg)
{
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    const uint32_t mask = game_stack_arg(0u);
    for (uint32_t pad = 0u; pad < 4u; pad++) {
        call_one_argument(0x00042710u, 0x002D74BEu, 8u, pad);
        if (g_eax == 0u) {
            continue;
        }
        call_one_argument(0x00042C30u, 0x002D74CBu, 8u, pad);
        if ((mask & g_eax) != 0u) {
            g_eax = 1u;
            return;
        }
    }
    g_eax = 0u;
}

GAME_REPLACE_EXACT(00361950, cdecl, 3, u32,
                   game_online_store_target_player_name_address_record_and_kind)
{
    g_eax = game_stack_arg(0u);
    const uint32_t copy[3] = {0x00773F00u, g_eax, 0x28u};
    if (game_guest_call(0x003C8960u, GAME_CC_cdecl, 0x00361961u, 0u, 0u, 0u,
                        copy, 3u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
    g_ecx = game_stack_arg(1u);
    g_edx = guest_read32(g_ecx);
    guest_write32(0x00773EE8u, g_edx);
    g_eax = guest_read32(g_ecx + 4u);
    guest_write32(0x00773EECu, g_eax);
    g_edx = guest_read32(g_ecx + 8u);
    guest_write32(0x00773EF0u, g_edx);
    g_eax = guest_read32(g_ecx + 0x0Cu);
    guest_write32(0x00773EF4u, g_eax);
    g_edx = guest_read32(g_ecx + 0x10u);
    guest_write32(0x00773EF8u, g_edx);
    g_eax = guest_read32(g_ecx + 0x14u);
    g_ecx = game_stack_arg(2u);
    guest_write32(0x00773EFCu, g_eax);
    guest_write32(0x00773F28u, g_ecx);
}

GAME_REPLACE_EXACT(0036EBE0, cdecl, 5, u32,
                   game_terminal_icon_widget_init_from_rect_and_table_entry)
{
    g_eax = game_stack_arg(1u);
    g_ecx = guest_read32(g_eax);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    guest_write32(g_esi, g_ecx);
    g_edx = guest_read32(g_eax + 4u);
    guest_write32(g_esi + 4u, g_edx);
    g_ecx = guest_read32(g_eax + 8u);
    guest_write32(g_esi + 8u, g_ecx);
    g_edx = guest_read32(g_eax + 0x0Cu);
    g_eax = game_stack_arg(4u);
    guest_write32(g_esi + 0x0Cu, g_edx);
    call_one_argument(0x00080F20u, 0x0036EC09u, 4u, g_eax);
    guest_write32(g_esi + 0x24u, g_eax);
    g_ecx = guest_read32(g_esi);
    g_edx = guest_read32(g_esi + 4u);
    g_eax = guest_read32(g_esi + 8u);
    guest_write32(g_esi + 0x10u, g_ecx);
    g_ecx = guest_read32(g_esi + 0x0Cu);
    guest_write32(g_esi + 0x14u, g_edx);
    g_edx = game_stack_arg(2u);
    guest_write32(g_esi + 0x18u, g_eax);
    g_eax = game_stack_arg(3u);
    guest_write32(g_esi + 0x1Cu, g_ecx);
    guest_write32(g_esi + 0x20u, g_edx);
    guest_write32(g_esi + 0x28u, g_eax);
    guest_write32(g_esi + 0x2Cu, 1u);
    pop_saved_esi();
}

GAME_REPLACE_EXACT(002BE5E0, cdecl, 2, u32, game_find_record_30_payload_by_key_pair)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    const uint32_t arguments[2] = {g_ecx, g_eax};
    if (game_guest_call(0x002BE590u, GAME_CC_cdecl, 0x002BE5EFu, 0u, 0u, 0u,
                        arguments, 2u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
    g_eax = g_eax != 0u ? g_eax + 0x14u : 0u;
}

GAME_REPLACE_EXACT(002875F0, cdecl, 1, u32, game_scalar_object_lookup_by_game_mode_1_or_0x11)
{
    g_eax = guest_read32(0x0079094Cu);
    g_eax -= 1u;
    if (g_eax == 0u) {
        g_ecx = game_stack_arg(0u);
        const uint32_t arguments[2] = {g_ecx, 0x19Cu};
        if (game_guest_call(0x00059DB0u, GAME_CC_cdecl, 0x00287622u, 0u, 0u, 0u,
                            arguments, 2u) != GAME_GUEST_CALL_OK) {
            __builtin_trap();
        }
        return;
    }
    g_eax -= 0x10u;
    if (g_eax != 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = game_stack_arg(0u);
    const uint32_t arguments[2] = {g_eax, 0x23Bu};
    if (game_guest_call(0x00059DB0u, GAME_CC_cdecl, 0x0028760Fu, 0u, 0u, 0u,
                        arguments, 2u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

GAME_REPLACE_EXACT(002E4CB0, cdecl, 0, u32,
                   game_set_current_player_7ba9e0_from_7b0c7c_field_4_and_reset_pad_when_slot_flag_bit0)
{
    g_ecx = guest_read32(0x007B0C7Cu);
    g_eax = guest_read32(g_ecx + 4u);
    guest_write32(0x007BA9E0u, g_eax);
    g_eax = guest_read32(g_eax * 4u + 0x00761C48u);
    if ((guest_read8(g_eax) & 1u) != 0u) {
        g_ecx = guest_read32(g_ecx);
        call_one_argument(0x00042870u, 0x002E4CD2u, 0u, g_ecx);
    }
    g_edx = guest_read32(0x007B0CC0u);
    g_edx += 0x224u;
    call_one_argument(0x00055D80u, 0x002E4CE7u, 0u, g_edx);
    g_ecx = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(003381D0, cdecl, 1, u32,
                   game_player_current_effect_entry_has_flag_bit_25_clear)
{
    g_eax = game_stack_arg(0u);
    g_eax = guest_read32(g_eax + 0xF54u);
    g_eax -= 1u;
    call_one_argument(0x00083970u, 0x003381E1u, 0u, g_eax);
    g_eax = (g_eax >> 25) & 1u;
    g_ecx = g_eax == 0u ? 1u : 0u;
    g_eax = g_ecx;
}

/* Fifth T1479 batch (docs/evidence/t1479/fifth-batch-predeclaration.md): table and record
 * roots. Every callee is an original unregistered lifted leaf. Saved register pushes are
 * mirrored as guest stack writes below the entry ESP and the call frames keep the original
 * caller byte counts and return PCs. Partial register writes (MOV DL / CL / AL) keep the
 * upper bytes. */

static uint32_t signed_byte(uint32_t address)
{
    return (uint32_t)(int32_t)(int8_t)guest_read8(address);
}

GAME_REPLACE_EXACT(002C3390, cdecl, 0, u32,
                   game_audio_settings_menu_globals_match_profile_values)
{
    call_scalar_leaf(0x00069640u, 0x002C3395u);
    g_eax = guest_read32(g_eax + 0x120u);
    if (g_eax != guest_read32(0x005236BCu)) {
        g_eax = 1u;
        return;
    }
    call_scalar_leaf(0x00069640u, 0x002C33A8u);
    g_ecx = guest_read32(g_eax + 0x118u);
    if (g_ecx != guest_read32(0x005236B4u)) {
        g_eax = 1u;
        return;
    }
    call_scalar_leaf(0x00069640u, 0x002C33BBu);
    g_edx = guest_read32(g_eax + 0x11Cu);
    if (g_edx != guest_read32(0x005236B8u)) {
        g_eax = 1u;
        return;
    }
    call_scalar_leaf(0x00069640u, 0x002C33CEu);
    g_ecx = guest_read32(0x007613D8u);
    if (guest_read32(g_eax + 0x12Cu) != g_ecx) {
        g_eax = 1u;
        return;
    }
    call_scalar_leaf(0x00069640u, 0x002C33E1u);
    g_edx = guest_read32(g_eax + 0x124u);
    if (g_edx != guest_read32(0x005236C0u)) {
        g_eax = 1u;
        return;
    }
    call_scalar_leaf(0x00069640u, 0x002C33F4u);
    g_eax = guest_read32(g_eax + 0x130u);
    g_eax = g_eax != guest_read32(0x007613DCu) ? 1u : 0u;
}

GAME_REPLACE_EXACT(002E6F80, cdecl, 1, u32,
                   game_count_roster_7de4c0_entries_via_table_6f0b08_get_by_arg)
{
    const uint32_t entry_esp = g_esp;
    const uint32_t record_argument = game_stack_arg(0u);
    uint32_t sum = 0u;
    uint32_t index = 0u;
    g_eax = guest_read32(0x007A2958u);
    g_ecx = guest_read32(0x00790950u);
    guest_write32(entry_esp - 4u, g_ebx);
    guest_write32(entry_esp - 8u, g_esi);
    guest_write32(entry_esp - 12u, g_edi);
    g_eax += g_ecx;
    if ((int32_t)g_eax > 0) {
        do {
            g_eax = guest_read32(0x007DE4C0u + index * 4u);
            if ((int32_t)g_eax >= 0) {
                call_one_argument(0x00063EA0u, 0x002E6FB1u, 12u, g_eax);
                if (g_eax != 0u) {
                    g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(g_eax + 1u);
                    if ((g_edx & 0xFFu) != guest_read8(record_argument + 1u)) {
                        g_eax = signed_byte(g_eax);
                        uint16_t word;
                        memcpy(&word, game_host_ptr(record_argument + g_eax * 2u + 2u), sizeof word);
                        g_ecx = (uint32_t)(int32_t)(int16_t)word;
                        sum += g_ecx;
                    }
                }
            }
            g_edx = guest_read32(0x007A2958u);
            g_eax = guest_read32(0x00790950u);
            index += 1u;
            g_edx += g_eax;
        } while ((int32_t)index < (int32_t)g_edx);
    }
    g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(0x007DE458u);
    if ((g_eax & 0x20u) != 0u) {
        uint16_t word;
        g_ecx = signed_byte(record_argument);
        memcpy(&word, game_host_ptr(record_argument + g_ecx * 2u + 2u), sizeof word);
        g_edx = (uint32_t)(int32_t)(int16_t)word;
        sum -= g_edx;
        g_eax = sum;
        return;
    }
    if ((g_eax & 0x10u) != 0u) {
        uint16_t word;
        memcpy(&word, game_host_ptr(record_argument + 0x4Eu), sizeof word);
        g_eax = (uint32_t)(int32_t)(int16_t)word;
        sum -= g_eax;
    }
    g_eax = sum;
}

GAME_REPLACE_EXACT(002B2390, cdecl, 0, u32,
                   game_table_7a2960_no_entry_has_flag_bit0_in_field_28)
{
    uint32_t index = 0u;
    guest_write32(g_esp - 4u, g_esi);
    call_leaf_below(0x0015BE50u, 0x002B2398u, 4u);
    if ((int32_t)g_eax > 0) {
        do {
            call_one_argument(0x0015BE60u, 0x002B23A6u, 4u, index);
            g_eax = guest_read32(g_eax + 4u);
            g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_eax + 0x28u);
            if ((g_ecx & 1u) != 0u) {
                g_eax = 0u;
                return;
            }
            index += 1u;
            call_leaf_below(0x0015BE50u, 0x002B23BAu, 4u);
        } while ((int32_t)index < (int32_t)g_eax);
    }
    g_eax = 1u;
}

GAME_REPLACE_EXACT(002FE0F0, cdecl, 3, u32,
                   game_copy_profile_stats_entry_or_widen_byte_struct_to_dwords)
{
    g_eax = game_stack_arg(2u);
    if (g_eax != 0u) {
        g_eax = guest_read32(0x007DE470u);
        const uint32_t arguments[2] = {0u, g_eax};
        if (game_guest_call(0x00069930u, GAME_CC_cdecl, 0x002FE105u, 0u, 0u, 0u,
                            arguments, 2u) != GAME_GUEST_CALL_OK) {
            __builtin_trap();
        }
        g_edx = guest_read32(g_eax);
        g_ecx = game_stack_arg(1u);
        guest_write32(g_ecx, g_edx);
        g_edx = guest_read32(g_eax + 4u);
        guest_write32(g_ecx + 4u, g_edx);
        g_edx = guest_read32(g_eax + 8u);
        guest_write32(g_ecx + 8u, g_edx);
        g_edx = guest_read32(g_eax + 0x0Cu);
        guest_write32(g_ecx + 0x0Cu, g_edx);
        g_edx = guest_read32(g_eax + 0x10u);
        guest_write32(g_ecx + 0x10u, g_edx);
        g_eax = guest_read32(g_eax + 0x14u);
        guest_write32(g_ecx + 0x14u, g_eax);
        return;
    }
    g_eax = game_stack_arg(0u);
    g_edx = guest_read8(g_eax);
    g_ecx = game_stack_arg(1u);
    guest_write32(g_ecx, g_edx);
    g_edx = guest_read32(g_eax + 4u);
    guest_write32(g_ecx + 4u, g_edx);
    g_edx = guest_read32(g_eax + 8u);
    guest_write32(g_ecx + 8u, g_edx);
    g_edx = guest_read8(g_eax + 0x0Cu);
    guest_write32(g_ecx + 0x0Cu, g_edx);
    g_eax = guest_read8(g_eax + 0x0Du);
    guest_write32(g_ecx + 0x10u, g_eax);
}

GAME_REPLACE_EXACT(0033ACB0, cdecl, 0, u32,
                   game_players_apply_class_entry_from_class_array_and_sync_slot_byte)
{
    const uint32_t entry_esp = g_esp;
    uint32_t player = 0u;
    uint32_t table_offset = 0u;
    guest_write32(entry_esp - 4u, g_edi);
    g_eax = guest_read32(0x00790950u);
    if ((int32_t)g_eax > 0) {
        guest_write32(entry_esp - 8u, g_ebx);
        guest_write32(entry_esp - 12u, g_esi);
        do {
            g_eax = guest_read32(0x007B0C48u);
            g_edx = guest_read32(0x00784034u + player * 4u);
            g_ecx = guest_read32(table_offset + g_eax + 0x14u);
            const uint32_t object = guest_read32(g_ecx + 0x7Cu);
            g_edx += 1u;
            const uint32_t arguments[2] = {object, g_edx};
            if (game_guest_call(0x000D7550u, GAME_CC_cdecl, 0x0033ACDBu, 12u, 0u, 0u,
                                arguments, 2u) != GAME_GUEST_CALL_OK) {
                __builtin_trap();
            }
            g_eax = guest_read32(object + 8u);
            g_ecx = guest_read8(g_eax + 0x007DE424u);
            g_edx = guest_read32(object + 0x84u);
            if (g_ecx != g_edx) {
                guest_write8(g_eax + 0x007DE424u, (uint8_t)g_edx);
            }
            g_eax = guest_read32(0x00790950u);
            player += 1u;
            table_offset += 0x1584u;
        } while ((int32_t)player < (int32_t)g_eax);
    }
}

GAME_REPLACE_EXACT(00321560, cdecl, 1, u32,
                   game_count_records_8c_with_dword_98_equal_arg_and_no_record_b4_match_plus_one)
{
    const uint32_t entry_esp = g_esp;
    uint32_t count = 0u;
    uint32_t record = 0u;
    g_eax = guest_read32(0x007844A8u);
    guest_write32(entry_esp - 4u, g_ebp);
    guest_write32(entry_esp - 8u, g_esi);
    guest_write32(entry_esp - 12u, g_edi);
    const uint32_t total = guest_read32(g_eax + 0xDD10u);
    if ((int32_t)total > 0) {
        guest_write32(entry_esp - 16u, g_ebx);
        uint32_t cursor = g_eax + 0xDD98u;
        do {
            g_eax = game_stack_arg(0u);
            if (guest_read32(cursor) == g_eax) {
                call_one_argument(0x0031DBC0u, 0x0032158Eu, 16u, record);
                if (g_eax == 0u) {
                    count += 1u;
                }
            }
            record += 1u;
            cursor += 0x8Cu;
        } while ((int32_t)record < (int32_t)total);
    }
    g_eax = count + 1u;
}
