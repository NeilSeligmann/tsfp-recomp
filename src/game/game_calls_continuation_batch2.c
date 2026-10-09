/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1475/T1477 continuation, batch 2: call-bearing roots with list, table and chain walks. Each
 * keeps the original nested-call frames (saved-register words below the entry ESP, argument
 * slots, return PCs) and leaves the callees' own EAX/ECX/EDX behind, so every adapter is
 * register-exact. In-flight ESI/EDI/EBX values are placed in the guest registers before a call
 * (the callees save them in their frames) and restored from the saved stack words after. */
#include "game_replace.h"

extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

static void call_guest(uint32_t target, uint32_t return_pc, uint32_t caller_bytes,
                       const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, caller_bytes, 0u, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 000606E0: cdecl(index). Clamps the index to [0, [0x7BB260] - 1]. With [0x7BB288] set the
 * answer is 00060410(index, 0), otherwise the record pointer inside the table at [0x6D780C]. */
GAME_REPLACE_EXACT(000606E0, cdecl, 1, u32, game_clamped_index_to_record_pointer)
{
    const uint32_t entry = g_esp;
    guest_write32(entry - 4u, g_esi);
    const uint32_t saved_esi = g_esi;
    uint32_t index = game_stack_arg(0u);
    if ((int32_t)index < 0)
        index = 0u;
    g_eax = guest_read32(0x007BB260u);
    if ((int32_t)index >= (int32_t)g_eax)
        index = g_eax - 1u;
    g_eax = guest_read32(0x007BB288u);
    if (g_eax != 0u) {
        const uint32_t arguments[2] = {index, 0u};
        g_esi = index;
        call_guest(0x00060410u, 0x00060708u, 4u, arguments, 2u);
        g_esi = saved_esi;
        return;
    }
    g_ecx = guest_read32(0x006D780Cu);
    g_eax = guest_read32(g_ecx + 8u);
    g_edx = ((int32_t)g_eax < 0 ? 0xFFFFFFFFu : 0u) & 3u;
    g_eax += g_edx;
    g_eax = (uint32_t)((int32_t)g_eax >> 2);
    g_eax = g_ecx + g_eax * 4u;
    g_ecx = guest_read32(g_eax);
    g_ecx <<= 4;
    g_ecx += index;
    g_ecx = guest_read32(g_eax + g_ecx * 4u + 4u);
    g_eax += 4u;
    g_ecx <<= 6;
    g_eax += g_ecx;
}

/* 00060740: cdecl(index, dst16, dst12, dst4). Copies the record found by 000606E0 into up to
 * three optional destinations (4 dwords, 3 dwords, 1 dword), each only when non-null. */
GAME_REPLACE_EXACT(00060740, cdecl, 4, u32, game_record_by_index_copy_to_optional_outputs)
{
    const uint32_t arguments[1] = {game_stack_arg(0u)};
    call_guest(0x000606E0u, 0x0006074Au, 0u, arguments, 1u);
    if (g_eax == 0u)
        return;
    g_ecx = game_stack_arg(1u);
    if (g_ecx != 0u) {
        for (uint32_t word = 0u; word < 4u; word++) {
            g_edx = guest_read32(g_eax + 4u * word);
            guest_write32(g_ecx + 4u * word, g_edx);
        }
    }
    g_ecx = game_stack_arg(2u);
    if (g_ecx != 0u) {
        for (uint32_t word = 0u; word < 3u; word++) {
            g_edx = guest_read32(g_eax + 0x10u + 4u * word);
            guest_write32(g_ecx + 4u * word, g_edx);
        }
    }
    g_ecx = game_stack_arg(3u);
    if (g_ecx != 0u) {
        g_eax = guest_read32(g_eax + 0x1Cu);
        guest_write32(g_ecx, g_eax);
    }
}

/* 00045E60: cdecl(). Looks up [0x784014] with 0032A5B0(…, 0xC77668); the sign-extended word at
 * +0xE of that record unless [0x761404] is set, the mode byte [0x7DE455] is not 10 and the
 * current record's entry in the table at 0x537210 is valid, then that record index. */
GAME_REPLACE_EXACT(00045E60, cdecl, 0, u32, game_lookup_record_word_0xe_or_current_index)
{
    const uint32_t entry = g_esp;
    guest_write32(entry - 4u, g_esi);
    const uint32_t saved_esi = g_esi;
    g_eax = guest_read32(0x00784014u);
    const uint32_t arguments[2] = {g_eax, 0x00C77668u};
    call_guest(0x0032A5B0u, 0x00045E71u, 4u, arguments, 2u);
    const uint32_t record = g_eax;
    g_esi = record;
    g_eax = guest_read32(0x00761404u);
    int use_word = g_eax == 0u || guest_read8(0x007DE455u) == 0x0Au;
    if (!use_word) {
        call_guest(0x00069640u, 0x00045E8Du, 4u, NULL, 0u);
        g_eax = guest_read32(g_eax + 0x114u);
        use_word = guest_read32(g_eax * 8u + 0x00537210u) == 0xFFFFFFFFu;
    }
    if (use_word) {
        const uint32_t word = guest_read8(record + 0xEu) | ((uint32_t)guest_read8(record + 0xFu) << 8);
        g_eax = (uint32_t)(int32_t)(int16_t)word;
    }
    g_esi = saved_esi;
}

/* 0006BB90: cdecl(pair, limit). Resolves the record set of 0006A850([0x79094C]); unless the mode
 * is 100 it scans the 0x20-byte rows for mask 0x1800000. The row where the match count reaches
 * 2*pair is returned (the following row when limit > 0), 0 when none. */
GAME_REPLACE_EXACT(0006BB90, cdecl, 2, u32, game_record_row_pair_scan_for_mask_0x1800000)
{
    const uint32_t entry = g_esp;
    guest_write32(entry - 4u, g_ebx);
    guest_write32(entry - 8u, g_ebp);
    guest_write32(entry - 12u, g_esi);
    guest_write32(entry - 16u, g_edi);
    g_eax = guest_read32(0x0079094Cu);
    g_esi = 0u;
    const uint32_t arguments[1] = {g_eax};
    call_guest(0x0006A850u, 0x0006BBA1u, 16u, arguments, 1u);
    if (g_eax != 0u && guest_read32(0x0079094Cu) != 0x64u) {
        g_edx = guest_read32(g_eax + 0x28u);
        g_ecx = 0u;
        if ((int32_t)g_edx > 0) {
            g_edi = guest_read32(g_eax + 0x24u);
            g_ebx = game_stack_arg(0u);
            g_eax = g_edi;
            for (;;) {
                if ((guest_read32(g_eax) & 0x01800000u) != 0u) {
                    g_ebp = g_ebx + g_ebx;
                    if (g_esi == g_ebp) {
                        g_edx = game_stack_arg(1u);
                        if ((int32_t)g_edx > 0)
                            g_eax = ((g_ecx + 1u) << 5) + g_edi;
                        goto restore;
                    }
                    g_esi++;
                }
                g_ecx++;
                g_eax += 0x20u;
                if ((int32_t)g_ecx >= (int32_t)g_edx)
                    break;
            }
        }
    }
    g_eax = 0u;
restore:
    g_edi = guest_read32(entry - 16u);
    g_esi = guest_read32(entry - 12u);
    g_ebp = guest_read32(entry - 8u);
    g_ebx = guest_read32(entry - 4u);
}

/* 0006B4E0: cdecl(mask). Refreshes the 12-byte pending records 0x6F1E70..0x6F2ED8: stale handles
 * (top 10 bits index the 0x2E8-byte owner table at [0x72F5E8], tag at +0x2C) are cleared, an
 * unowned flagged record is reset, and the owner flag word at +0x28 gets bit 0 set or cleared
 * according to the 1656D0 predicate, the mask word and the record flags. */
GAME_REPLACE_EXACT(0006B4E0, cdecl, 1, u32, game_pending_records_refresh_owner_flag_bit_0)
{
    const uint32_t entry = g_esp;
    guest_write32(entry - 4u, g_ebx);
    guest_write32(entry - 8u, g_esi);
    guest_write32(entry - 12u, g_edi);
    g_edi = 0x006F1E70u;
    g_ebx = 1u;
    do {
        g_eax = guest_read32(g_edi);
        if (g_eax == 0u) {
            g_esi = 0u;
        } else {
            g_edx = guest_read32(0x0072F5E8u);
            g_esi = (g_eax >> 22) * 0x2E8u;
            g_ecx = guest_read32(g_esi + g_edx + 0x2Cu);
            g_esi += g_edx;
            if (g_ecx != g_eax) {
                guest_write32(g_edi, 0u);
                g_esi = 0u;
            }
        }
        if ((guest_read8(g_edi + 8u) & 4u) != 0u) {
            if (g_esi == 0u) {
                guest_write32(g_edi, 0u);
                guest_write32(g_edi + 4u, 0xFFFFFFFFu);
                guest_write32(g_edi + 8u, 0u);
            } else {
                int predicate_held = 0;
                g_eax = guest_read32(g_esi + 0x80u);
                if (g_eax != 0u) {
                    const uint32_t arguments[1] = {g_eax};
                    call_guest(0x001656D0u, 0x0006B533u, 12u, arguments, 1u);
                    predicate_held = g_eax != 0u;
                }
                if (predicate_held) {
                    g_eax = guest_read32(g_esi + 0x28u) | g_ebx;
                    guest_write32(g_esi + 0x28u, g_eax);
                } else {
                    g_eax = game_stack_arg(0u);
                    if ((guest_read32(g_edi + 4u) & g_eax) != 0u) {
                        if (guest_read32(g_esi + 0x20u) != 0x40u ||
                            (guest_read8(g_esi + 0x28u) & 2u) != 0u) {
                            g_eax = guest_read32(g_esi + 0x28u) | g_ebx;
                            guest_write32(g_esi + 0x28u, g_eax);
                        }
                    } else if ((guest_read8(g_edi + 8u) & (g_ebx & 0xFFu)) != 0u) {
                        g_eax = guest_read32(g_esi + 0x28u) & 0xFFFFFFFEu;
                        guest_write32(g_esi + 0x28u, g_eax);
                    }
                }
            }
        }
        g_edi += 0xCu;
    } while ((int32_t)g_edi < 0x006F2ED8);
    g_edi = guest_read32(entry - 12u);
    g_esi = guest_read32(entry - 8u);
    g_ebx = guest_read32(entry - 4u);
}

/* 0015C390: cdecl(object). owner = [object+0x7C]. When bit 4 of [owner+0x2C] is clear: set it,
 * clear mask 0xFFFFFF9E in [[owner]+0x28] and, if the signed [owner+8] is not below [0x790950],
 * call 0015BCC0([owner]) and return 1. Every other path returns 0. */
GAME_REPLACE_EXACT(0015C390, cdecl, 1, u32, game_owner_mark_pending_and_remove_from_active_list)
{
    const uint32_t object = game_stack_arg(0u);
    g_eax = guest_read32(object + 0x7Cu);
    g_ecx = guest_read32(g_eax + 0x2Cu);
    if ((g_ecx & 4u) != 0u) {
        g_eax = 0u;
        return;
    }
    g_ecx |= 4u;
    guest_write32(g_eax + 0x2Cu, g_ecx);
    g_ecx = guest_read32(g_eax);
    guest_write32(g_ecx + 0x28u, guest_read32(g_ecx + 0x28u) & 0xFFFFFF9Eu);
    g_ecx = guest_read32(g_eax + 8u);
    if ((int32_t)g_ecx < (int32_t)guest_read32(0x00790950u)) {
        g_eax = 0u;
        return;
    }
    g_edx = guest_read32(g_eax);
    const uint32_t arguments[1] = {g_edx};
    call_guest(0x0015BCC0u, 0x0015C3BEu, 0u, arguments, 1u);
    g_eax = 1u;
}

/* 0017A190: cdecl(object). Validates the handle at [object+0xC08] against the 0x2E8-byte owner
 * table at [0x72F5E8] (a stale handle is cleared), resolves the owner's record through
 * 0017A120 and returns the 16-byte row at record+0x144 for its signed index, 0 when absent. */
GAME_REPLACE_EXACT(0017A190, cdecl, 1, u32, game_object_handle_to_owner_row_pointer)
{
    const uint32_t entry = g_esp;
    g_edx = game_stack_arg(0u);
    g_ecx = guest_read32(g_edx + 0xC08u);
    guest_write32(entry - 4u, g_esi);
    g_eax = 0u;
    if (g_ecx != 0u) {
        g_esi = guest_read32(0x0072F5E8u);
        g_eax = (g_ecx >> 22) * 0x2E8u + g_esi;
        if (guest_read32(g_eax + 0x2Cu) != g_ecx) {
            guest_write32(g_edx + 0xC08u, 0u);
            g_eax = 0u;
        } else {
            g_esi = guest_read32(g_eax + 0x7Cu);
            const uint32_t arguments[1] = {g_edx};
            call_guest(0x0017A120u, 0x0017A1CEu, 4u, arguments, 1u);
            if (g_eax != 0u && g_esi != 0u) {
                g_eax = guest_read32(g_eax + 0x2Cu);
                if ((int32_t)g_eax >= 0 && (int32_t)g_eax < (int32_t)guest_read32(g_esi + 0x164u))
                    g_eax = (g_eax << 4) + g_esi + 0x144u;
                else
                    g_eax = 0u;
            } else {
                g_eax = 0u;
            }
        }
    }
    g_esi = guest_read32(entry - 4u);
}

/* 0011D4D0: cdecl(object, mode, value). 0011D400 (ECX = object, one stack argument mode) gathers
 * the pending list at 0x73ECCC and returns its length; each entry's +4 pointer then gets
 * 000942B0(pointer, value) (marks it dirty). Returns the last callee's EAX (the count when
 * the list is empty). */
GAME_REPLACE_EXACT(0011D4D0, cdecl, 3, u32, game_object_gather_list_and_mark_each_entry)
{
    const uint32_t entry = g_esp;
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    guest_write32(entry - 4u, g_esi);
    guest_write32(entry - 8u, g_edi);
    const uint32_t gather[1] = {g_eax};
    call_guest(0x0011D400u, 0x0011D4E0u, 8u, gather, 1u);
    g_edi = g_eax;
    g_esi = 0u;
    if ((int32_t)g_edi > 0) {
        guest_write32(entry - 12u, g_ebx);
        g_ebx = game_stack_arg(2u);
        do {
            g_ecx = guest_read32(g_esi * 4u + 0x0073ECCCu);
            g_edx = guest_read32(g_ecx + 4u);
            const uint32_t mark[2] = {g_edx, g_ebx};
            call_guest(0x000942B0u, 0x0011D501u, 12u, mark, 2u);
            g_esi++;
        } while ((int32_t)g_esi < (int32_t)g_edi);
        g_ebx = guest_read32(entry - 12u);
    }
    g_edi = guest_read32(entry - 8u);
    g_esi = guest_read32(entry - 4u);
}
