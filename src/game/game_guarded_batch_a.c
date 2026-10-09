/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 batch A: hand C for guarded-jump roots (jump table or tail jmp in the closure), proved
 * with the opt-in guarded-jump-schema3 contract (`tools.replace prove --guarded-jump-contract
 * guarded-jump-schema3`). Every root keeps its real callees live through game_guest_call, so the
 * callee leftovers in EAX/ECX/EDX stay visible and each adapter is register-exact. Records:
 * docs/evidence/t1576/batch-a.md, receipts docs/data/t1576-batch-a/receipts/. */
#include "game_replace.h"

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, uint32_t this_pointer, const uint32_t *arguments,
                       unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, this_pointer, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 000379C0: cdecl(kind, flag). With flag == 0 a kind 0..9 picks (jump table at 37AA4, identity
 * map) one text id 0x1C25 0x1C26 0x1C28 0x1C27 0x1C29 0x1C2A 0x1C2B 0x1C2C 0x1C2D 0x1C2E and the
 * result is 3D7B0(id). flag == 1 with kind 10 gives 3D7B0(0x1C2F), flag == 2 with kind 11 gives
 * 3D7B0(0x1C8B). Every other request returns 0 through the single `xor eax, eax` at 37AA0, which
 * is also the JA default of the table, so all of them run through `default:` here. */
GAME_REPLACE_EXACT(000379C0, cdecl, 2, u32, game_ui_add_feedback_reason_text_items_bad_language_cheating)
{
    uint32_t slot = 0xFFFFFFFFu;
    uint32_t text = 0u;
    uint32_t return_pc = 0u;
    int direct = 0;
    g_eax = game_stack_arg(1u);
    if (g_eax == 0u) {
        g_eax = game_stack_arg(0u);
        if (g_eax <= 9u) {
            slot = g_eax;
        }
    } else if (g_eax == 1u && game_stack_arg(0u) == 0xAu) {
        text = 0x1C2Fu;
        return_pc = 0x37A82u;
        direct = 1;
    } else if (g_eax == 2u && game_stack_arg(0u) == 0xBu) {
        text = 0x1C8Bu;
        return_pc = 0x37A9Cu;
        direct = 1;
    }
    if (!direct) {
        switch (slot) {
        case 0:
            text = 0x1C25u;
            return_pc = 0x379EAu;
            break;
        case 1:
            text = 0x1C26u;
            return_pc = 0x379F8u;
            break;
        case 2:
            text = 0x1C28u;
            return_pc = 0x37A06u;
            break;
        case 3:
            text = 0x1C27u;
            return_pc = 0x37A14u;
            break;
        case 4:
            text = 0x1C29u;
            return_pc = 0x37A22u;
            break;
        case 5:
            text = 0x1C2Au;
            return_pc = 0x37A30u;
            break;
        case 6:
            text = 0x1C2Bu;
            return_pc = 0x37A3Eu;
            break;
        case 7:
            text = 0x1C2Cu;
            return_pc = 0x37A4Cu;
            break;
        case 8:
            text = 0x1C2Du;
            return_pc = 0x37A5Au;
            break;
        case 9:
            text = 0x1C2Eu;
            return_pc = 0x37A68u;
            break;
        default:
            g_eax = 0u;
            return;
        }
    }
    const uint32_t arguments[1] = {text};
    call_guest(0x0003D7B0u, GAME_CC_cdecl, return_pc, 0u, 0u, arguments, 1u);
}

/* 0003DFA0: cdecl(size, kind) arena allocator (docs/t1364-arena-allocator-naming.md). The size is
 * rounded to S = (size + 15) & ~15 (32-bit wrapping). `cmp kind, 4 / ja / jmp [kind*4 + 3E094]`:
 *   kind 0  descending shared arena: top [6B8388], remaining [6B8384]. Signed S + 16 > remaining
 *           calls the hook 389B0 twice (-1 then 1, it is a bare RET), then lowers the top,
 *           writes the "LOCALMEM" header (+0 tag, +4 tag, +8 S, +C header address) and returns
 *           header + 16 with remaining -= S + 16.
 *   kind 1  ascending shared cursor [6B837C] (signed S > remaining calls the hook twice).
 *   kind 2  ascending separate cursor [6B8370] against budget [6B8378], no capacity check.
 *   kind 4  ascending cursor [6B8390], no budget.
 *   kind 3 and every unsigned kind above 4 share the arm at 3E08D and return 0.
 * ESI is saved and restored by the original (push esi / pop esi), ECX and EDX leave as the
 * arm's last values. */
GAME_REPLACE_EXACT(0003DFA0, cdecl, 2, u32, game_arena_alloc_round16_by_kind)
{
    const uint32_t kind = game_stack_arg(1u);
    const uint32_t rounded = (game_stack_arg(0u) + 15u) & 0xFFFFFFF0u;
    const uint32_t hook_minus_one[1] = {0xFFFFFFFFu};
    const uint32_t hook_one[1] = {1u};
    switch (kind) {
    case 0:
        g_ecx = guest_read32(0x006B8384u);
        g_eax = rounded + 0x10u;
        if ((int32_t)g_eax > (int32_t)g_ecx) {
            call_guest(0x000389B0u, GAME_CC_cdecl, 0x0003DFD3u, 4u, 0u, hook_minus_one, 1u);
            call_guest(0x000389B0u, GAME_CC_cdecl, 0x0003DFDAu, 8u, 0u, hook_one, 1u);
        }
        g_eax = guest_read32(0x006B8388u);
        g_ecx = 0xFFFFFFF0u - rounded;
        g_eax += g_ecx;
        guest_write32(0x006B8388u, g_eax);
        guest_write32(g_eax, 0x41434F4Cu);
        g_edx = guest_read32(0x006B8388u);
        guest_write32(g_edx + 4u, 0x4D454D4Cu);
        g_eax = guest_read32(0x006B8388u);
        guest_write32(g_eax + 8u, rounded);
        g_eax = guest_read32(0x006B8388u);
        guest_write32(g_eax + 0xCu, g_eax);
        g_eax = guest_read32(0x006B8388u);
        g_edx = guest_read32(0x006B8384u);
        g_ecx = 0xFFFFFFF0u - rounded;
        g_eax += 0x10u;
        g_edx += g_ecx;
        guest_write32(0x006B8384u, g_edx);
        break;
    case 1:
        if ((int32_t)rounded > (int32_t)guest_read32(0x006B8384u)) {
            call_guest(0x000389B0u, GAME_CC_cdecl, 0x0003E041u, 4u, 0u, hook_minus_one, 1u);
            call_guest(0x000389B0u, GAME_CC_cdecl, 0x0003E048u, 8u, 0u, hook_one, 1u);
        }
        g_ecx = guest_read32(0x006B837Cu);
        g_eax = g_ecx;
        g_ecx += rounded;
        guest_write32(0x006B837Cu, g_ecx);
        guest_write32(0x006B8384u, guest_read32(0x006B8384u) - rounded);
        break;
    case 2:
        g_ecx = guest_read32(0x006B8370u);
        g_eax = g_ecx;
        g_ecx += rounded;
        guest_write32(0x006B8370u, g_ecx);
        guest_write32(0x006B8378u, guest_read32(0x006B8378u) - rounded);
        break;
    case 4:
        g_ecx = guest_read32(0x006B8390u);
        g_eax = g_ecx;
        g_ecx += rounded;
        guest_write32(0x006B8390u, g_ecx);
        break;
    case 3:
    default:
        g_eax = 0u;
        break;
    }
}

/* 0003E0B0: cdecl(alignment, size, kind) alignment wrapper around 3DFA0 (docs/t1364-arena-
 * allocator-naming.md). `cmp kind, 4 / ja / jmp [kind*4 + 3E1C4]`, mask = ~(alignment - 1) and
 * up(x) = (x + alignment - 1) & mask (32-bit wrapping, no power-of-two check):
 *   kind 1  pad the cursor [6B837C] to up(cursor), debit [6B8384] by the pad, then 3DFA0(size, 1).
 *   kind 2  the same with cursor [6B8370] and budget [6B8378].
 *   kind 0  top [6B8388] and remaining [6B8384] move by D = (up(top) - top) - alignment, then
 *           3DFA0(up(size), 0), then the header size [[6B8388] + 8] grows by alignment - pad.
 *   kind 4  cursor [6B8390] = up(cursor), then falls through to the delegation below.
 *   kind 3 and every unsigned kind above 4 delegate 3DFA0(size, kind) unchanged.
 * ESI and EDI are saved and restored by the original, EAX is the callee's pointer. */
GAME_REPLACE_EXACT(0003E0B0, cdecl, 3, u32, game_arena_alloc_with_alignment_by_kind)
{
    const uint32_t kind = game_stack_arg(2u);
    uint32_t arguments[2];
    switch (kind) {
    case 0: {
        const uint32_t alignment = game_stack_arg(0u);
        g_ecx = guest_read32(0x006B8388u);
        uint32_t pad = g_ecx + alignment - 1u;
        g_eax = ~(alignment - 1u);
        pad &= g_eax;
        pad -= g_ecx;
        g_edx = pad - alignment;
        g_ecx += g_edx;
        g_edx = guest_read32(0x006B8384u);
        guest_write32(0x006B8388u, g_ecx);
        g_ecx = pad - alignment;
        g_edx += g_ecx;
        guest_write32(0x006B8384u, g_edx);
        g_edx = game_stack_arg(1u);
        g_ecx = alignment + g_edx - 1u;
        g_ecx &= g_eax;
        arguments[0] = g_ecx;
        arguments[1] = 0u;
        call_guest(0x0003DFA0u, GAME_CC_cdecl, 0x0003E183u, 8u, 0u, arguments, 2u);
        g_ecx = guest_read32(0x006B8388u);
        g_edx = guest_read32(g_ecx + 8u);
        g_edx += alignment - pad;
        guest_write32(g_ecx + 8u, g_edx);
        break;
    }
    case 1:
        g_ecx = guest_read32(0x006B837Cu);
        g_edx = game_stack_arg(0u);
        g_eax = g_ecx + g_edx - 1u;
        g_edx = ~(g_edx - 1u);
        g_eax &= g_edx;
        g_edx = game_stack_arg(1u);
        g_eax -= g_ecx;
        g_ecx += g_eax;
        guest_write32(0x006B837Cu, g_ecx);
        g_ecx = guest_read32(0x006B8384u);
        g_ecx -= g_eax;
        guest_write32(0x006B8384u, g_ecx);
        arguments[0] = g_edx;
        arguments[1] = kind;
        call_guest(0x0003DFA0u, GAME_CC_cdecl, 0x0003E0FBu, 4u, 0u, arguments, 2u);
        break;
    case 2:
        g_ecx = guest_read32(0x006B8370u);
        g_edx = game_stack_arg(0u);
        g_eax = g_ecx + g_edx - 1u;
        g_edx = ~(g_edx - 1u);
        g_eax &= g_edx;
        g_edx = game_stack_arg(1u);
        g_eax -= g_ecx;
        g_ecx += g_eax;
        guest_write32(0x006B8370u, g_ecx);
        g_ecx = guest_read32(0x006B8378u);
        g_ecx -= g_eax;
        guest_write32(0x006B8378u, g_ecx);
        arguments[0] = g_edx;
        arguments[1] = kind;
        call_guest(0x0003DFA0u, GAME_CC_cdecl, 0x0003E136u, 4u, 0u, arguments, 2u);
        break;
    case 4:
        g_eax = game_stack_arg(0u);
        g_edx = guest_read32(0x006B8390u);
        g_ecx = g_edx + g_eax - 1u;
        g_eax = ~(g_eax - 1u);
        g_ecx &= g_eax;
        guest_write32(0x006B8390u, g_ecx);
        __attribute__((fallthrough));
    case 3:
    default:
        g_edx = game_stack_arg(1u);
        arguments[0] = g_edx;
        arguments[1] = kind;
        call_guest(0x0003DFA0u, GAME_CC_cdecl, 0x0003E1BDu, 4u, 0u, arguments, 2u);
        break;
    }
}

/* 00045A70: cdecl(). 1CEB00(mode [79094C]) refreshes the cached mode flag [75AF10] through its
 * byte map and two-slot jump table, then the result is (flag == 0) where the flag word is
 * [5236B8] when 1CEB00 returned non-zero and [5236B4] otherwise. */
GAME_REPLACE_EXACT(00045A70, cdecl, 0, u32, game_music_volume_setting_is_zero_by_mode_cache_flag)
{
    g_eax = guest_read32(0x0079094Cu);
    const uint32_t arguments[1] = {g_eax};
    call_guest(0x001CEB00u, GAME_CC_cdecl, 0x00045A7Bu, 0u, 0u, arguments, 1u);
    g_ecx = guest_read32(0x005236B8u);
    if (g_eax == 0u) {
        g_ecx = guest_read32(0x005236B4u);
    }
    g_eax = g_ecx == 0u ? 1u : 0u;
}

#include "game_guest_tail.h"

/* 0008E380: cdecl(blob). With blob and [blob+0x10] non-null the array at [blob+0x10] (16-byte
 * records, key in the first dword, terminated by key -1) is walked: each key goes to 18CE0
 * (which marks a record flag and may call 18300), then the record key is overwritten with
 * 0x40000000. The function ends in a TAIL jmp to 389B0 (a bare RET) once the array is done
 * (also when the first key is already -1). A null blob or null array returns directly with EAX
 * as it entered. EBX, ESI and EDI are saved and restored by the original. */
GAME_REPLACE_EXACT(0008E380, cdecl, 1, u32, game_model_blob_release_texture_entries_and_mark_keys_unloaded)
{
    const uint32_t blob = game_stack_arg(0u);
    if (blob == 0u) {
        return;
    }
    uint32_t record = guest_read32(blob + 0x10u);
    if (record == 0u) {
        return;
    }
    if (guest_read32(record) != 0xFFFFFFFFu) {
        uint32_t offset = 0u;
        do {
            uint32_t arguments[1];
            g_eax = guest_read32(record);
            arguments[0] = g_eax;
            call_guest(0x00018CE0u, GAME_CC_cdecl, 0x0008E3A8u, 12u, 0u, arguments, 1u);
            guest_write32(record, 0x40000000u);
            g_ecx = guest_read32(blob + 0x10u);
            offset += 0x10u;
            g_eax = guest_read32(g_ecx + offset);
            record = g_ecx + offset;
        } while (g_eax != 0xFFFFFFFFu);
    }
    if (game_guest_tail(0x000389B0u, GAME_CC_cdecl, 0u) != GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

/* 000BE860: cdecl(type_mask, index). Masks 1..0x40 go through
 * `dec mask / cmp 0x3F / ja / movzx ecx, byte [mask - 1 + BE950] / jmp [ecx*4 + BE934]`
 * (seven slots, the byte map sends the unused masks to slot 6): slots 0..4 return
 * table + index * 0x54, slot 5 (mask 0x40) asks 1B99B0(index) for a weapon slot and returns
 * 4EA484 + slot * 0xE0 when it is in 1..0x2B. Every exit that returns 0 in the original shares
 * the one instruction `xor eax, eax` at BE931 (negative index, unknown masks, the JA default,
 * slot 6, a failed weapon lookup), so each of them runs through `case 6: default:` here.
 * Masks above 0x80 are plain compares (0x100 and 0x1000 share a table, 0x200, 0x400 and 0x800
 * have their own, 0x800 returns the table address itself). ECX leaves as the byte (the slot)
 * on the table path and as mask - 1 on the JA path. */
GAME_REPLACE_EXACT(000BE860, cdecl, 2, u32, game_object_type_entry_address_by_type_mask_and_index)
{
    uint32_t slot = 0xFFFFFFFFu;
    g_eax = game_stack_arg(1u);
    if ((int32_t)g_eax >= 0) {
        g_ecx = game_stack_arg(0u);
        if ((int32_t)g_ecx > 0x80) {
            if ((int32_t)g_ecx > 0x400) {
                if (g_ecx == 0x800u) {
                    g_eax = 0x004DC568u;
                    return;
                }
                if (g_ecx == 0x1000u) {
                    g_eax = g_eax * 0x54u + 0x004DC768u;
                    return;
                }
            } else if (g_ecx == 0x400u) {
                g_eax = g_eax * 0x54u + 0x004DC510u;
                return;
            } else if (g_ecx == 0x100u) {
                g_eax = g_eax * 0x54u + 0x004DC768u;
                return;
            } else if (g_ecx == 0x200u) {
                g_eax = g_eax * 0x54u + 0x004DDAE8u;
                return;
            }
        } else if (g_ecx == 0x80u) {
            g_eax = g_eax * 0x54u + 0x004DC3C0u;
            return;
        } else {
            g_ecx -= 1u;
            if (g_ecx <= 0x3Fu) {
                g_ecx = guest_read8(0x000BE950u + g_ecx);
                slot = g_ecx;
            }
        }
    }
    switch (slot) {
    case 0:
        g_eax = g_eax * 0x54u + 0x004DC218u;
        break;
    case 1:
        g_eax = g_eax * 0x54u + 0x004FB190u;
        break;
    case 2:
        g_eax = g_eax * 0x54u + 0x004DBA38u;
        break;
    case 3:
        g_eax = g_eax * 0x54u + 0x004DBE28u;
        break;
    case 4:
        g_eax = g_eax * 0x54u + 0x004DC668u;
        break;
    case 5: {
        const uint32_t arguments[1] = {g_eax};
        call_guest(0x001B99B0u, GAME_CC_cdecl, 0x000BE8B3u, 0u, 0u, arguments, 1u);
        if ((int32_t)g_eax > 0 && (int32_t)g_eax < 0x2C) {
            g_eax = g_eax * 0xE0u + 0x004EA484u;
            break;
        }
        __attribute__((fallthrough));
    }
    case 6:
    default:
        g_eax = 0u;
        break;
    }
}

/* 000C0E60: cdecl(type, index, _, hint, weapon). A type 0x40 request with 0 <= index < 0x46, a
 * zero fourth argument and a fifth argument in 1..0x2B returns 4EA484 + weapon * 0xE0 directly;
 * every other request is forwarded to BE860(type, index) (ECX = index, EDX = type, EAX = result). */
GAME_REPLACE_EXACT(000C0E60, cdecl, 5, u32, game_object_type_entry_address_with_weapon_table_4ea484_for_type_0x40)
{
    g_ecx = game_stack_arg(1u);
    g_edx = game_stack_arg(0u);
    if ((int32_t)g_ecx >= 0 && g_edx == 0x40u && (int32_t)g_ecx < 0x46) {
        g_eax = game_stack_arg(3u);
        if (g_eax == 0u) {
            g_eax = game_stack_arg(4u);
            if ((int32_t)g_eax > 0 && (int32_t)g_eax < 0x2C) {
                g_eax = g_eax * 0xE0u + 0x004EA484u;
                return;
            }
        }
    }
    const uint32_t arguments[2] = {g_edx, g_ecx};
    call_guest(0x000BE860u, GAME_CC_cdecl, 0x000C0E9Eu, 0u, 0u, arguments, 2u);
}

/* 000D6ED0: cdecl(id). D6DD0(id) (byte map over id - 0x12, bound 0x98, two-slot jump table) is
 * non-zero for the ids that map to slot 0; the result is 1 only when it returned 0 and the id is
 * none of 0x4D, 0x4F, 0x64. ECX holds the id across the call, EDX and the rest are untouched. */
GAME_REPLACE_EXACT(000D6ED0, cdecl, 1, u32, game_char_type_id_not_in_set_d6dd0_and_not_0x4d_0x4f_0x64)
{
    g_ecx = game_stack_arg(0u);
    const uint32_t arguments[1] = {g_ecx};
    call_guest(0x000D6DD0u, GAME_CC_cdecl, 0x000D6EDAu, 0u, 0u, arguments, 1u);
    if (g_eax != 0u || g_ecx == 0x4Du || g_ecx == 0x4Fu || g_ecx == 0x64u) {
        g_eax = 0u;
        return;
    }
    g_eax = 1u;
}

/* 000E05B0: cdecl(owner, result_block). Zeroes the seven dwords of the block, then walks the
 * entries first..last (the globals [736680] and [737E44]) of the 0x22C-byte table at [737BD4]:
 * every live entry (dword 0 is not -1) whose +0x158 equals `owner` is passed to E0540 with
 * ECX = the entry type and EAX = the block (E0540 is a byte map plus eight-slot jump table
 * that increments one dword of the block). ESI and EDI are saved and restored. */
GAME_REPLACE_EXACT(000E05B0, cdecl, 2, u32, game_table_737bd4_for_entries_with_field_0x158_equal_arg_call_e0540)
{
    g_ecx = guest_read32(0x00737E44u);
    g_eax = game_stack_arg(1u);
    g_edx = 0u;
    const uint32_t first = guest_read32(0x00736680u);
    for (uint32_t offset = 0u; offset <= 0x18u; offset += 4u) {
        guest_write32(g_eax + offset, g_edx);
    }
    g_ecx += 1u;
    if ((int32_t)first >= (int32_t)g_ecx) {
        return;
    }
    const uint32_t table = guest_read32(0x00737BD4u);
    g_edx = first * 0x22Cu + table;
    const uint32_t owner = game_stack_arg(0u);
    g_ecx -= first;
    uint32_t remaining = g_ecx;
    do {
        g_ecx = guest_read32(g_edx);
        if (g_ecx != 0xFFFFFFFFu && guest_read32(g_edx + 0x158u) == owner) {
            call_guest(0x000E0540u, GAME_CC_cdecl, 0x000E060Bu, 8u, 0u, NULL, 0u);
        }
        g_edx += 0x22Cu;
        remaining -= 1u;
    } while (remaining != 0u);
}
