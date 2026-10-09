/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1542: hand C replacements for the admitted roots of T1534 list-003 (call-bearing and
 * register-input roots). Drafted by Claude Sonnet 5.5 from the retail disassembly, proven one
 * root at a time by tools.replace prove (O0/O3, seeds 20261001/20261006, 600 random cases, live
 * call closure for the roots that call). Record: docs/t-call-draft-list003.md. Each body keeps
 * the original register hand-off, so callees run live through game_guest_call. */
#include "game_replace.h"

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, 0u, 0u, arguments, count) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
}

static uint32_t read_word_signed(uint32_t address)
{
    const uint32_t low = guest_read8(address);
    const uint32_t high = guest_read8(address + 1u);
    return (uint32_t)(int32_t)(int16_t)(uint16_t)(low | (high << 8));
}

/* 000C7360: cdecl() with ECX = object. Returns 1 when the pair of records at [obj+0x270] and
 * [obj+0x26C] is missing/of type 0x13, or (when [obj+0x2A8] is set) the first has type 0,
 * or either has flag 0x400 in its dword +4, or both have flag bit 0 set. Else 0. */
GAME_REPLACE_EXACT_INPUTS(000C7360, cdecl, 0, u32, ecx, game_object_pair_fields_0x26c_0x270_type_not_0x13_and_flag_bits_conflict)
{
    const uint32_t object = g_ecx;
    g_eax = guest_read32(object + 0x270u);
    const uint32_t second = guest_read32(object + 0x26Cu);
    if (g_eax != 0u) {
        g_edx = guest_read32(g_eax);
        if (g_edx != 0x13u && guest_read32(second) != 0x13u) {
            const uint32_t strict = guest_read32(object + 0x2A8u);
            if (strict == 0u || g_edx != 0u) {
                g_eax = guest_read32(g_eax + 4u);
                if ((g_eax & 0x400u) == 0u) {
                    g_ecx = guest_read32(second + 4u);
                    if ((g_ecx & 0x400u) == 0u && ((g_eax & 1u) == 0u || (g_ecx & 1u) == 0u)) {
                        g_eax = 0u;
                        return;
                    }
                }
            }
        }
    }
    g_eax = 1u;
}

/* 001B6DB0: cdecl(owner, state, mode). state+0x30 selects a 0x29C-stride record at [0x4F9BAC]
 * (when nonnegative). Returns 1 when owner's word [+0x61E + 2*k] plus state's word [+0x38 + 2*k]
 * reaches the record limit ([+0x8C], or [+0x90] when mode is nonzero), with k = [record+0x14].
 * Otherwise, or when the selector is negative, returns 1 when the original 001B6C90(owner,
 * state, 1, mode) is nonzero, else 0. */
GAME_REPLACE_EXACT(001B6DB0, cdecl, 3, u32, game_ammo_total_at_cap_0x8c_0x90_or_covers_fire_cost)
{
    const uint32_t owner = game_stack_arg(0u);
    const uint32_t state = game_stack_arg(1u);
    const uint32_t mode = game_stack_arg(2u);
    g_edx = state;
    g_eax = guest_read32(g_edx + 0x30u);
    if ((int32_t)g_eax >= 0) {
        g_ecx = guest_read32(0x004F9BACu);
        g_eax = g_eax * 0x29Cu + g_ecx;
        g_ecx = guest_read32(g_eax + 0x14u);
        if (mode != 0u)
            g_eax = guest_read32(g_eax + 0x90u);
        else
            g_eax = guest_read32(g_eax + 0x8Cu);
        const uint32_t owner_word = read_word_signed(owner + g_ecx * 2u + 0x61Eu);
        g_ecx = read_word_signed(g_edx + g_ecx * 2u + 0x38u);
        if ((int32_t)(owner_word + g_ecx) >= (int32_t)g_eax) {
            g_eax = 1u;
            return;
        }
    }
    const uint32_t forward[4] = {owner, state, 1u, mode};
    call_guest(0x001B6C90u, GAME_CC_cdecl, 0x001B6E07u, 8u, forward, 4u);
    g_eax = g_eax != 0u ? 1u : 0u;
}

/* 00065340: cdecl(player, bit, count). For 0 <= bit < 128 and count >= 1, sets bit (bit & 31) of
 * the dword at record+0xC0C+4*(bit>>5)+0x10*k for k = 0..count-1, where record is the pointer
 * returned by 00069900(player) on each iteration. EAX is the arithmetic bit>>5 on the early-out
 * paths and the last stored pointer otherwise. */
GAME_REPLACE_EXACT(00065340, cdecl, 3, u32, game_player_record_set_bitmask_range_0xc0c_by_bit_index_and_count)
{
    g_edx = game_stack_arg(1u);
    g_eax = (uint32_t)((int32_t)g_edx >> 5);
    g_ecx = g_edx & 0x8000001Fu;
    if ((int32_t)g_ecx < 0)
        g_ecx = ((g_ecx - 1u) | 0xFFFFFFE0u) + 1u;
    if ((int32_t)g_edx < 0 || (int32_t)g_edx >= 0x80)
        return;
    g_edx = game_stack_arg(2u);
    g_edx--;
    if ((int32_t)g_edx < 0)
        return;
    const uint32_t player = game_stack_arg(0u);
    const uint32_t mask = 1u << (g_ecx & 0x1Fu);
    uint32_t offset = g_eax * 4u + 0xC0Cu;
    uint32_t remaining = g_edx + 1u;
    do {
        const uint32_t record_args[1] = {player};
        call_guest(0x00069900u, GAME_CC_cdecl, 0x00065385u, 16u, record_args, 1u);
        g_edx = guest_read32(g_eax + offset);
        g_eax += offset;
        g_edx |= mask;
        offset += 0x10u;
        remaining--;
        guest_write32(g_eax, g_edx);
    } while (remaining != 0u);
}

/* 002CB360: cdecl(start). For index 1..9 whose word [0x75FC50 + 4*(index-1)] differs from
 * 001CD2A0(index), appends word [0x53EBCC + 2*(index-1)] (sign-extended) to the table 0x78AE80
 * and the constant 0xE92 to 0x78AEC0 at byte offset 4*start (grown by 4 per entry), while that
 * offset stays below 0x28. Returns the number of appended entries. */
GAME_REPLACE_EXACT(002CB360, cdecl, 1, u32, game_append_changed_entries_to_list_78aec0_text_0xe92_from_word_table)
{
    uint32_t offset = game_stack_arg(0u) << 2;
    uint32_t appended = 0u;
    uint32_t index = 0u;
    uint32_t next;
    do {
        next = index + 1u;
        const uint32_t current_args[1] = {next};
        call_guest(0x001CD2A0u, GAME_CC_cdecl, 0x002CB379u, 16u, current_args, 1u);
        g_ecx = guest_read32(index * 4u + 0x0075FC50u);
        if (g_ecx != g_eax && (int32_t)offset < 0x28) {
            const uint32_t low = guest_read8(index * 2u + 0x0053EBCCu);
            const uint32_t high = guest_read8(index * 2u + 0x0053EBCDu);
            g_eax = (uint32_t)(int32_t)(int16_t)(uint16_t)(low | (high << 8));
            guest_write32(offset + 0x0078AEC0u, 0xE92u);
            guest_write32(offset + 0x0078AE80u, g_eax);
            offset += 4u;
            appended++;
        }
        index = next;
    } while ((int32_t)index < 0xA);
    g_eax = appended;
}
