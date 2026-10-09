/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480/T1476 sixth batch: call-bearing roots with nested guest calls. Each keeps the
 * original frame (return PC, caller-live words) and register hand-off through
 * game_guest_call. Registered non-exact callees leave ECX/EDX alone, so any leftover the
 * original callee produced that a caller could read is recomputed here. */
#include "game_replace.h"

extern __thread int g_df;

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, 0u, 0u, arguments, count) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* REP MOVSD of size/4 dwords, then REP MOVSB of size%4 bytes, in the direction of the
 * guest direction flag (descending from the first addresses when set). */
static void rep_copy(uint32_t destination, uint32_t source, uint32_t size)
{
    const uint32_t dword_step = g_df != 0 ? 0xFFFFFFFCu : 4u;
    const uint32_t byte_step = g_df != 0 ? 0xFFFFFFFFu : 1u;
    for (uint32_t count = size >> 2; count != 0u; count--) {
        guest_write32(destination, guest_read32(source));
        destination += dword_step;
        source += dword_step;
    }
    for (uint32_t count = size & 3u; count != 0u; count--) {
        guest_write8(destination, guest_read8(source));
        destination += byte_step;
        source += byte_step;
    }
}

/* 00094390: cdecl(object, extra). Snapshots the 0x10-stride table at [[object+4]+0x10]
 * into a fresh bump allocation kept at [object+0x1EC] (once), and marks the owner flag. */
GAME_REPLACE_EXACT(00094390, cdecl, 2, u32, game_cutscene_prop_alloc_copy_of_record_table_0x10_with_extra_slots_into_slot_0x1ec)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t table = guest_read32(guest_read32(object + 4u) + 0x10u);
    if (table != 0u) {
        uint32_t count = 0u;
        if ((int32_t)guest_read32(table) >= 0) {
            uint32_t cursor = table;
            int32_t entry;
            do {
                entry = (int32_t)guest_read32(cursor + 0x10u);
                cursor += 0x10u;
                count++;
            } while (entry >= 0);
        }
        const uint32_t size = (count + game_stack_arg(1u) + 1u) << 4;
        const uint32_t arguments[1] = {(size + 0xFu) & 0xFFFFFFF0u};
        call_guest(0x0003E970u, GAME_CC_cdecl, 0x000943D4u, 8u, arguments, 1u);
        const uint32_t pointer = g_eax;
        const uint32_t owner_table = guest_read32(object + 4u);
        guest_write32(object + 0x1ECu, pointer);
        rep_copy(pointer, guest_read32(owner_table + 0x10u), size);
        const uint32_t flags_owner = guest_read32(object);
        g_ecx = guest_read32(flags_owner + 0x18u) | 4u;
        guest_write32(flags_owner + 0x18u, g_ecx);
        g_edx = owner_table;
    }
    g_eax = guest_read32(object + 0x1ECu);
}

/* 00094410: cdecl(object). Same snapshot for the 0x30-stride table at [[object+4]+0x14],
 * kept at [object+0x1F0]. An absent table leaves ECX zero. */
GAME_REPLACE_EXACT(00094410, cdecl, 1, u32, game_cutscene_prop_alloc_copy_of_record_table_0x14_stride_0x30_into_slot_0x1f0)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t table = guest_read32(guest_read32(object + 4u) + 0x14u);
    if (table == 0u) {
        g_ecx = 0u;
    } else {
        uint32_t count = 0u;
        if ((int32_t)guest_read32(table + 0x10u) >= 0) {
            uint32_t cursor = table + 0x10u;
            int32_t entry;
            do {
                entry = (int32_t)guest_read32(cursor + 0x30u);
                cursor += 0x30u;
                count++;
            } while (entry >= 0);
        }
        const uint32_t size = (count * 3u + 3u) << 4;
        const uint32_t arguments[1] = {(size + 0xFu) & 0xFFFFFFF0u};
        call_guest(0x0003E970u, GAME_CC_cdecl, 0x00094458u, 8u, arguments, 1u);
        const uint32_t pointer = g_eax;
        const uint32_t owner_table = guest_read32(object + 4u);
        guest_write32(object + 0x1F0u, pointer);
        rep_copy(pointer, guest_read32(owner_table + 0x14u), size);
        const uint32_t flags_owner = guest_read32(object);
        g_ecx = guest_read32(flags_owner + 0x18u) | 4u;
        guest_write32(flags_owner + 0x18u, g_ecx);
        g_edx = owner_table;
    }
    g_eax = guest_read32(object + 0x1F0u);
}

/* 000DB420: cdecl(list, owner). Takes a node from the list (000DB340, registered with
 * scratch ECX/EDX) and fills it from the owner record. ECX/EDX are root computed. */
GAME_REPLACE_EXACT(000DB420, cdecl, 2, u32, game_list_rotate_tail_to_head_then_fill_node_from_object_0x7c_record)
{
    const uint32_t arguments[1] = {game_stack_arg(0u)};
    call_guest(0x000DB340u, GAME_CC_cdecl, 0x000DB42Au, 0u, arguments, 1u);
    const uint32_t node = g_eax;
    const uint32_t owner = game_stack_arg(1u);
    uint32_t record = guest_read32(owner + 0x7Cu);
    guest_write32(node + 0x10u, owner);
    g_edx = guest_read32(record + 0xCu);
    guest_write32(node, g_edx);
    record = guest_read32(record + 0x14u);
    guest_write32(node + 4u, record);
    g_ecx = record * 0x54u + 0x004DDAE8u;
    guest_write32(node + 0xCu, g_ecx);
}

/* 004098CE: stdcall(destination, source). Dispatches on the source record's 16-bit tag:
 * 1 and 0x69 build a destination record (result 0), 0xFFFE builds one and returns the
 * callee's value. */
GAME_REPLACE_EXACT(004098CE, stdcall, 2, u32, game_record_build_by_tag)
{
    const uint32_t destination = game_stack_arg(0u);
    const uint32_t source = game_stack_arg(1u);
    uint16_t tag;
    memcpy(&tag, game_host_ptr(source), sizeof tag);
    const uint32_t arguments[2] = {destination, source};
    uint32_t result = 0u;
    g_ecx = source;
    if (tag == 1u) {
        call_guest(0x00409835u, GAME_CC_stdcall, 0x0040990Bu, 4u, arguments, 2u);
    } else if (tag == 0x69u) {
        call_guest(0x0040985Eu, GAME_CC_stdcall, 0x004098FFu, 4u, arguments, 2u);
    } else if (tag == 0xFFFEu) {
        call_guest(0x00409885u, GAME_CC_stdcall, 0x004098F1u, 4u, arguments, 2u);
        result = g_eax;
    }
    g_eax = result;
}

/* 003E95F5: stdcall(packed). Maps a packed operand to an index: class 4 is computed
 * inline, others go through 003E9551 and, for result 0x20, 003E95D5. */
GAME_REPLACE_EXACT(003E95F5, stdcall, 1, u32, game_packed_operand_index)
{
    const uint32_t packed = game_stack_arg(0u);
    if ((packed & 0x70000000u) == 0x40000000u) {
        const uint32_t low = packed & 0xFFFu;
        g_eax = ((low != 0u) ? 4u : 0u) + 0x10u + low;
        g_edx = low;
        return;
    }
    uint32_t edx = packed;
    g_edx = edx;
    const uint32_t first[1] = {packed};
    call_guest(0x003E9551u, GAME_CC_stdcall, 0x003E9622u, 0u, first, 1u);
    const uint32_t kind = g_eax;
    if (kind == 0x20u) {
        edx = packed & 0xFFFu;
        g_edx = edx;
        const uint32_t second[1] = {edx};
        call_guest(0x003E95D5u, GAME_CC_stdcall, 0x003E9633u, 0u, second, 1u);
        g_eax += 0x20u;
    } else if (kind == 0x17u) {
        edx = (packed & 1u) + kind;
        g_eax = edx;
    } else {
        edx = packed & 0xFFFu;
        g_eax = kind + edx;
    }
    g_edx = edx;
}

/* 003E8763: stdcall(object, mode). Splits a packed byte of the object into four 2-bit
 * fields (only the low byte of each pushed word is masked) for the 003E871B combiner.
 * Modes 0 and 2 use the fields at bit 4 of +8 or +4, mode 1 bit 0x13 of +4. */
GAME_REPLACE_EXACT(003E8763, stdcall, 2, u32, game_packed_fields_combine)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t mode = game_stack_arg(1u);
    if (mode > 2u) {
        g_eax = (mode - 2u) & 0xFFFFFF00u;
        return;
    }
#define LOW_BYTE_TWO_BITS(value) (((value) & 0xFFFFFF00u) | ((value) & 3u))
    const uint32_t packed = guest_read32(object + (mode == 0u ? 8u : 4u));
    const unsigned base = mode == 1u ? 0x13u : 4u;
    const unsigned step_a = mode == 1u ? 0x15u : 6u;
    const unsigned step_b = mode == 1u ? 0x17u : 8u;
    const unsigned step_c = mode == 1u ? 0x19u : 10u;
    const uint32_t arguments[4] = {LOW_BYTE_TWO_BITS(packed >> step_c),
                                   LOW_BYTE_TWO_BITS(packed >> step_b),
                                   LOW_BYTE_TWO_BITS(packed >> step_a),
                                   LOW_BYTE_TWO_BITS(packed >> base)};
#undef LOW_BYTE_TWO_BITS
    g_eax = (object & 0xFFFFFF00u) | 3u;
    g_ecx = arguments[0];
    g_edx = arguments[1];
    call_guest(0x003E871Bu, GAME_CC_stdcall, 0x003E87CEu, 0u, arguments, 4u);
}

/* 0038A411: stdcall(object). Result codes for an object with no buffered data: 0x80070057
 * for null, 4 or 0x80040004 or 0x80004005 by the buffered counts, and the state word at
 * +0x28 (2 or 5). The count helper 0038A27A writes a stack local. */
GAME_REPLACE_EXACT(0038A411, stdcall, 1, u32, game_audio_decoder_check_pending_input_and_set_state_2_or_5)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t local = g_esp - 8u;
    guest_write32(local, 0u);
    if (object == 0u) {
        g_eax = 0x80070057u;
        return;
    }
    const uint32_t buffered = guest_read32(object + 0x7Cu);
    int failed = 1;
    if (buffered == 0u) {
        if (guest_read32(object + 0x54u) != 0u) {
            g_eax = 0x80004005u;
        } else {
            const uint32_t arguments[2] = {object, local};
            call_guest(0x0038A27Au, GAME_CC_cdecl, 0x0038A43Eu, 12u, arguments, 2u);
            g_ecx = guest_read32(g_esp - 16u);
            if (guest_read32(local) != 0u) {
                failed = 0;
            } else {
                g_eax = 4u;
            }
        }
    } else if ((int32_t)guest_read32(object + 0x54u) > 0) {
        guest_write32(object + 0x88u, 1u);
        failed = 0;
    } else {
        g_eax = 0x80004005u;
    }
    if (failed) {
        guest_write32(object + 0x28u, 2u);
    } else {
        guest_write32(object + 0x28u, 5u);
        g_eax = 0x80040004u;
    }
}

/* 0008E920: cdecl(object, first, amount, second, extra). Adds `amount` to every
 * non-negative counter of the 0x30-stride chain at [object+0x14] (the chain ends after an
 * entry whose next +0x10 dword is negative), advances the object through 0008E800 and
 * stores the two pair words at +0x10 and +0x14. T1476 reproof with the namespace-81
 * 124-case chain domain after the sixth-batch gate rejection (60 verdicts). */
GAME_REPLACE_EXACT(0008E920, cdecl, 5, u32, game_geometry_rebase_indices_then_publish_arrays)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t amount = game_stack_arg(2u);
    uint32_t ecx = g_ecx;
    uint32_t cursor = guest_read32(object + 0x14u);
    if ((int32_t)guest_read32(cursor + 0x10u) > -1) {
        do {
            ecx = guest_read32(cursor);
            if ((int32_t)ecx >= 0) {
                ecx += amount;
                guest_write32(cursor, ecx);
            }
            ecx = guest_read32(cursor + 0x40u);
            cursor += 0x30u;
        } while ((int32_t)ecx > -1);
    }
    const uint32_t extra = game_stack_arg(4u);
    const uint32_t arguments[4] = {object, 0u, amount, extra};
    g_eax = extra;
    g_ecx = ecx;
    g_edx = amount;
    call_guest(0x0008E800u, GAME_CC_cdecl, 0x0008E955u, 4u, arguments, 4u);
    g_ecx = game_stack_arg(1u);
    g_edx = game_stack_arg(3u);
    guest_write32(object + 0x10u, g_ecx);
    guest_write32(object + 0x14u, g_edx);
}
