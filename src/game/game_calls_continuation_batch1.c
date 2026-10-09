/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1475/T1477 continuation, batch 1: call-bearing roots whose nested guest calls keep the
 * original frames (saved-register words and argument slots below the entry ESP) and whose
 * callee EAX/ECX/EDX leftovers stay visible, so each adapter is register-exact. */
#include "game_replace.h"

extern __thread uint32_t g_esi, g_edi;

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, uint32_t this_pointer, const uint32_t *arguments,
                       unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, this_pointer, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

static uint32_t guest_string_length(uint32_t text)
{
    uint32_t cursor = text;
    while (guest_read8(cursor++) != 0u) {
    }
    return cursor - text - 1u;
}

/* 00061EE0: cdecl(slot). Tag at [0x7B0C48] + slot*0x1584 + 0x1568 resolved through the
 * mask table (fastcall 000618C0, ECX=tag, EDX untouched). -1 when the global gate bits
 * 0x20000200 are set, the slot is unused ([0x7356D8] + slot*0xEA0 + 0x678 == -1) or unmatched. */
GAME_REPLACE_EXACT(00061EE0, cdecl, 1, u32, game_slot_tag_to_mask_table_index_or_minus_1)
{
    if ((guest_read32(0x007DE458u) & 0x20000200u) != 0u) {
        g_eax = 0xFFFFFFFFu;
        return;
    }
    const uint32_t slot = game_stack_arg(0u);
    g_eax = slot;
    g_edx = guest_read32(0x007356D8u);
    g_ecx = slot * 0xEA0u;
    if (guest_read32(g_ecx + g_edx + 0x678u) == 0xFFFFFFFFu) {
        g_eax = 0xFFFFFFFFu;
        return;
    }
    g_ecx = guest_read32(0x007B0C48u);
    g_eax = slot * 0x1584u;
    g_ecx = guest_read32(g_eax + g_ecx + 0x1568u);
    /* ECX carries the tag only (EDX is not an input of 000618C0). */
    call_guest(0x000618C0u, GAME_CC_thiscall, 0x00061F20u, 0u, g_ecx, NULL, 0u);
}

/* 00075F40: cdecl(object). 1 when no 0x30 bit is set in the byte at +0x40 and the chain
 * predicate 000747C0(object) is 0, else 0. */
GAME_REPLACE_EXACT(00075F40, cdecl, 1, u32, game_object_flags_clear_and_chain_predicate_zero)
{
    const uint32_t object = game_stack_arg(0u);
    g_eax = object;
    if ((guest_read8(object + 0x40u) & 0x30u) != 0u) {
        g_eax = 0u;
        return;
    }
    const uint32_t arguments[1] = {object};
    call_guest(0x000747C0u, GAME_CC_cdecl, 0x00075F50u, 0u, 0u, arguments, 1u);
    g_eax = g_eax == 0u ? 1u : 0u;
}

/* 00069EE0: cdecl(use_cached). Resolve the pool pointer (cached [0x6F1E14] when use_cached
 * and set, otherwise 0003E430 then an allocation 0003E440(0x32000) stored in the cache),
 * then make it current: when it differs from [0x6F1E10] store it there and clear +0xC.
 * ECX/EDX are scratch: 0003E440 is registered with scratch ECX/EDX, so its register
 * leftovers are not reproducible, and the single direct caller does not read them. */
static uint32_t game_pool_pointer_resolve_and_make_current(uint32_t use_cached)
{
    g_eax = use_cached;
    int resolved = 0;
    if (use_cached != 0u) {
        g_eax = guest_read32(0x006F1E14u);
        resolved = g_eax != 0u;
    }
    if (!resolved) {
        call_guest(0x0003E430u, GAME_CC_cdecl, 0x00069EF6u, 0u, 0u, NULL, 0u);
        if (g_eax != 0u) {
            const uint32_t arguments[1] = {0x00032000u};
            call_guest(0x0003E440u, GAME_CC_cdecl, 0x00069F04u, 0u, 0u, arguments, 1u);
            guest_write32(0x006F1E14u, g_eax);
        } else {
            g_eax = guest_read32(0x006F1E14u);
        }
        if (g_eax == 0u)
            return g_eax;
    }
    if (guest_read32(0x006F1E10u) == g_eax)
        return g_eax;
    guest_write32(0x006F1E10u, g_eax);
    guest_write32(g_eax + 0xCu, 0u);
    return g_eax;
}

GAME_REPLACE(00069EE0, cdecl, 1, u32, game_pool_pointer_resolve_and_make_current)

/* 000406A0: cdecl(text, limit). With [0x6B93A8] == 0 returns text + limit. Otherwise scans
 * at most `limit` (signed) bytes and calls 0003FC90 (EAX=cursor, one argument = the address
 * of the saved-ECX word, which it increments) at every ','; returns the final cursor.
 * ECX is the saved word popped back, so the callee's increments are visible in ECX. */
GAME_REPLACE_EXACT(000406A0, cdecl, 2, u32, game_comma_separated_scan_with_field_counter)
{
    const uint32_t entry = g_esp;
    guest_write32(entry - 4u, g_ecx);
    g_eax = guest_read32(0x006B93A8u);
    if (g_eax == 0u) {
        g_edx = game_stack_arg(0u);
        g_eax = g_edx + game_stack_arg(1u);
        return;
    }
    guest_write32(entry - 8u, g_esi);
    uint32_t cursor = game_stack_arg(0u);
    const uint32_t saved_esi = g_esi;
    g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(cursor);
    if ((g_eax & 0xFFu) != 0u) {
        guest_write32(entry - 12u, g_edi);
        const uint32_t saved_edi = g_edi;
        int32_t remaining = (int32_t)game_stack_arg(1u);
        while (remaining > 0) {
            if ((g_eax & 0xFFu) == 0x2Cu) {
                g_eax = cursor;
                guest_write32(entry - 4u, cursor);
                g_esi = cursor;
                g_edi = (uint32_t)remaining;
                const uint32_t arguments[1] = {entry - 4u};
                call_guest(0x0003FC90u, GAME_CC_cdecl, 0x000406D8u, 12u, 0u, arguments, 1u);
            }
            g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(cursor + 1u);
            cursor++;
            remaining--;
            if ((g_eax & 0xFFu) == 0u)
                break;
        }
        g_edi = saved_edi;
    }
    g_eax = cursor;
    g_esi = saved_esi;
    g_ecx = guest_read32(entry - 4u);
}

/* 00033DB0: stdcall(object, a, b, text). Stores a at +0x10, the magic 0x47524551 at +0 and b
 * at +4, copies `text` (length bytes, 003C8960 bounded copy) to the pointer at +8 and stores
 * strlen(text)+1 (measured after the copy) at +0xC and in EAX. */
GAME_REPLACE_EXACT(00033DB0, stdcall, 4, u32, game_header_store_magic_and_copy_text)
{
    const uint32_t entry = g_esp;
    const uint32_t first = game_stack_arg(1u);
    const uint32_t second = game_stack_arg(2u);
    const uint32_t saved_esi = g_esi;
    const uint32_t saved_edi = g_edi;
    guest_write32(entry - 4u, saved_esi);
    const uint32_t object = game_stack_arg(0u);
    guest_write32(entry - 8u, saved_edi);
    const uint32_t text = game_stack_arg(3u);
    guest_write32(object + 0x10u, first);
    guest_write32(object, 0x47524551u);
    guest_write32(object + 4u, second);
    const uint32_t length = guest_string_length(text);
    const uint32_t destination = guest_read32(object + 8u);
    g_esi = object;
    g_edi = text;
    g_eax = length;
    g_ecx = (second & 0xFFFFFF00u);
    g_edx = destination;
    const uint32_t arguments[3] = {destination, text, length};
    call_guest(0x003C8960u, GAME_CC_cdecl, 0x00033DE7u, 8u, 0u, arguments, 3u);
    const uint32_t stored = guest_string_length(text) + 1u;
    g_ecx &= 0xFFFFFF00u;
    g_edx = text + 1u;
    guest_write32(object + 0xCu, stored);
    g_eax = stored;
    g_edi = guest_read32(entry - 8u);
    g_esi = guest_read32(entry - 4u);
}
