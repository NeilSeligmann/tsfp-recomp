/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1544 (T1534 list-001): 25 short roots, hand-drafted from the pinned disassembly. Leaves use
 * the register-exact macros with ECX inputs where the original reads ECX. Call-bearing roots
 * keep the real callees live through game_guest_call (callee leftovers in EAX/ECX/EDX stay
 * visible, so each adapter is register-exact). In-flight ESI values are placed in the guest
 * register around a call and restored afterwards, as the original push/pop does.
 * Record: docs/t-call-draft-list001.md. */
#include "game_replace.h"

extern __thread uint32_t g_esi;

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, uint32_t this_pointer, const uint32_t *arguments,
                       unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, this_pointer, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 003B8570: address ECX+0x190, or 0 when the byte there is zero. ECX leaves as the mask. */
GAME_REPLACE_EXACT_INPUTS(003B8570, cdecl, 0, u32, ecx, game_address_of_field_0x190_or_null)
{
    g_eax = g_ecx + 0x190u;
    const uint32_t byte = guest_read8(g_eax);
    g_ecx = byte != 0u ? 0xFFFFFFFFu : 0u;
    g_eax &= g_ecx;
}

/* 003BA810: stores vtable 0x4766D8 at +0 and zero at +4 and +8. Returns the object, ECX = 0. */
GAME_REPLACE_EXACT_INPUTS(003BA810, cdecl, 0, u32, ecx, game_construct_vtable_4766d8_two_zero_fields)
{
    g_eax = g_ecx;
    g_ecx = 0u;
    guest_write32(g_eax, 0x004766D8u);
    guest_write32(g_eax + 4u, g_ecx);
    guest_write32(g_eax + 8u, g_ecx);
}

/* 003BB930: ECX = list. Returns [list+4] + 4 when [list] is non-zero, else 0. */
GAME_REPLACE_EXACT_INPUTS(003BB930, cdecl, 0, u32, ecx, game_list_first_node_data_pointer_or_null)
{
    if (guest_read32(g_ecx) == 0u) {
        g_eax = 0u;
        return;
    }
    g_eax = guest_read32(g_ecx + 4u);
    g_eax += 4u;
}

/* 003BB710: stdcall(size), ECX = arena. [arena+4] = align4([arena+4]) + size. */
GAME_REPLACE_EXACT_INPUTS(003BB710, stdcall, 1, u32, ecx, game_arena_advance_offset_aligned)
{
    g_eax = guest_read32(g_ecx + 4u);
    g_edx = ((0u - g_eax) & 3u) + g_eax;
    g_edx += game_stack_arg(0u);
    guest_write32(g_ecx + 4u, g_edx);
}

/* 00012FA0: cdecl(a, b). Calls 0015BD00(a, b) with ECX = a and EAX = b, returns the signed byte
 * at the returned address. */
GAME_REPLACE_EXACT(00012FA0, cdecl, 2, u32, game_pair_table_entry_signed_byte)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    const uint32_t arguments[2] = {g_ecx, g_eax};
    call_guest(0x0015BD00u, GAME_CC_cdecl, 0x00012FAFu, 0u, 0u, arguments, 2u);
    g_eax = (uint32_t)(int32_t)(int8_t)guest_read8(g_eax);
}

/* 00082820: [0x7B979C] == -1 gives the default record 0x4766D8, else 00024EB0(value). */
GAME_REPLACE_EXACT(00082820, cdecl, 0, u32, game_memory_unit_slot_address_or_default)
{
    g_eax = guest_read32(0x007B979Cu);
    if (g_eax == 0xFFFFFFFFu) {
        g_eax = 0x004766D8u;
        return;
    }
    const uint32_t arguments[1] = {g_eax};
    call_guest(0x00024EB0u, GAME_CC_cdecl, 0x00082836u, 0u, 0u, arguments, 1u);
}

/* 00386F73: ECX = object. With [object+0x44] set, [[object+0x20]+0xC] = [object+0x40]. Else
 * 00389118 (this = [object+0x20], argument [object+0x24]). */
GAME_REPLACE_EXACT_INPUTS(00386F73, cdecl, 0, u32, ecx, game_audio_voice_activity_update)
{
    if (guest_read32(g_ecx + 0x44u) != 0u) {
        g_eax = guest_read32(g_ecx + 0x20u);
        g_ecx = guest_read32(g_ecx + 0x40u);
        guest_write32(g_eax + 0xCu, g_ecx);
        return;
    }
    const uint32_t arguments[1] = {guest_read32(g_ecx + 0x24u)};
    g_ecx = guest_read32(g_ecx + 0x20u);
    call_guest(0x00389118u, GAME_CC_thiscall, 0x00386F8Eu, 0u, g_ecx, arguments, 1u);
}

/* 0038608A: stdcall(value), ECX = object. Pushes kind 2 and the value on the pending stack at
 * object+0x6C (count at +0xFC), then the total at +0x100 grows by 1. ESI is saved and restored. */
GAME_REPLACE_EXACT_INPUTS(0038608A, stdcall, 1, u32, ecx, game_audio_pending_push_kind_2)
{
    g_eax = g_ecx + 0xFCu;
    g_edx = guest_read32(g_eax);
    guest_write32(g_ecx + g_edx * 8u + 0x6Cu, 2u);
    g_edx = guest_read32(g_eax);
    const uint32_t saved_esi = g_esi;
    g_esi = game_stack_arg(0u);
    guest_write32(g_ecx + g_edx * 8u + 0x70u, g_esi);
    guest_write32(g_eax, guest_read32(g_eax) + 1u);
    guest_write32(g_ecx + 0x100u, guest_read32(g_ecx + 0x100u) + 1u);
    g_esi = saved_esi;
}

/* 00386062: the same with kind 1 and the total at +0x100 growing by 2. */
GAME_REPLACE_EXACT_INPUTS(00386062, stdcall, 1, u32, ecx, game_audio_pending_push_kind_1)
{
    g_eax = g_ecx + 0xFCu;
    g_edx = guest_read32(g_eax);
    guest_write32(g_ecx + g_edx * 8u + 0x6Cu, 1u);
    g_edx = guest_read32(g_eax);
    const uint32_t saved_esi = g_esi;
    g_esi = game_stack_arg(0u);
    guest_write32(g_ecx + g_edx * 8u + 0x70u, g_esi);
    guest_write32(g_eax, guest_read32(g_eax) + 1u);
    guest_write32(g_ecx + 0x100u, guest_read32(g_ecx + 0x100u) + 2u);
    g_esi = saved_esi;
}

/* 001B2850: 00037380 returns a pair in EAX:EDX. 1 when it equals the dword pair at
 * 0x757068 + [0x756F10] * 0x170, else 0. */
GAME_REPLACE_EXACT(001B2850, cdecl, 0, u32, game_current_entry_pair_equals_global_pair)
{
    call_guest(0x00037380u, GAME_CC_cdecl, 0x001B2855u, 0u, 0u, NULL, 0u);
    g_ecx = guest_read32(0x00756F10u);
    g_ecx *= 0x170u;
    if (g_eax == guest_read32(g_ecx + 0x00757068u) && g_edx == guest_read32(g_ecx + 0x0075706Cu)) {
        g_eax = 1u;
        return;
    }
    g_eax = 0u;
}

/* 00188840: cdecl(value, object). Sets bit 0x10 of [object+0x28], calls 0009E720(object,
 * 0x1887F0), then stores the value at [object+0x1A0]. ESI is saved and restored. */
GAME_REPLACE_EXACT(00188840, cdecl, 2, u32, game_object_set_flag_install_callback_store_value)
{
    const uint32_t saved_esi = g_esi;
    g_esi = game_stack_arg(1u);
    g_eax = guest_read32(g_esi + 0x28u);
    g_eax |= 0x10u;
    guest_write32(g_esi + 0x28u, g_eax);
    const uint32_t arguments[2] = {g_esi, 0x001887F0u};
    call_guest(0x0009E720u, GAME_CC_cdecl, 0x00188859u, 4u, 0u, arguments, 2u);
    g_eax = game_stack_arg(0u);
    guest_write32(g_esi + 0x1A0u, g_eax);
    g_esi = saved_esi;
}

/* 000828C0, 000828F0, 00082920: slot = [0x7B9798]. A slot outside 0..4 gives the default
 * record 0x4766D8, else 0003D7B0(dword at table + slot * 20). */
static void slot_table_text(uint32_t table, uint32_t return_pc)
{
    g_eax = guest_read32(0x007B9798u);
    if ((int32_t)g_eax < 0 || (int32_t)g_eax >= 5) {
        g_eax = 0x004766D8u;
        return;
    }
    g_eax = g_eax + g_eax * 4u;
    g_ecx = guest_read32(g_eax * 4u + table);
    const uint32_t arguments[1] = {g_ecx};
    call_guest(0x0003D7B0u, GAME_CC_cdecl, return_pc, 0u, 0u, arguments, 1u);
}

GAME_REPLACE_EXACT(000828C0, cdecl, 0, u32, game_memory_unit_table_4d9698_text_by_slot)
{
    slot_table_text(0x004D9698u, 0x000828DEu);
}

GAME_REPLACE_EXACT(000828F0, cdecl, 0, u32, game_memory_unit_table_4d969c_text_by_slot)
{
    slot_table_text(0x004D969Cu, 0x0008290Eu);
}

GAME_REPLACE_EXACT(00082920, cdecl, 0, u32, game_memory_unit_table_4d96a0_text_by_slot)
{
    slot_table_text(0x004D96A0u, 0x0008293Eu);
}

/* 00064EF0: cdecl(destination, id). Copies the NUL-terminated text 0003D7B0(id + 0xB2B)
 * to the destination, terminator included. */
GAME_REPLACE_EXACT(00064EF0, cdecl, 2, u32, game_copy_text_id_plus_0xb2b_to_buffer)
{
    g_eax = game_stack_arg(1u);
    g_eax += 0xB2Bu;
    const uint32_t arguments[1] = {g_eax};
    call_guest(0x0003D7B0u, GAME_CC_cdecl, 0x00064EFFu, 0u, 0u, arguments, 1u);
    g_edx = game_stack_arg(0u);
    uint32_t byte;
    do {
        byte = guest_read8(g_eax);
        g_eax++;
        guest_write8(g_edx, (uint8_t)byte);
        g_edx++;
    } while (byte != 0u);
    g_ecx = (g_ecx & 0xFFFFFF00u) | byte;
}

/* 00387B27: stdcall(low, high), ECX = object. Stores the pair at +0x38 and +0x3C and
 * [object+0x48] = 003CAA20(low, high, [object+0xC], 0). ESI is saved and restored. */
GAME_REPLACE_EXACT_INPUTS(00387B27, stdcall, 2, u32, ecx, game_media_object_set_duration_pair)
{
    const uint32_t saved_esi = g_esi;
    g_eax = game_stack_arg(0u);
    g_esi = g_ecx;
    g_ecx = game_stack_arg(1u);
    const uint32_t packet_size = guest_read32(g_esi + 0xCu);
    guest_write32(g_esi + 0x38u, g_eax);
    guest_write32(g_esi + 0x3Cu, g_ecx);
    const uint32_t arguments[4] = {g_eax, g_ecx, packet_size, 0u};
    call_guest(0x003CAA20u, GAME_CC_stdcall, 0x00387B44u, 4u, 0u, arguments, 4u);
    guest_write32(g_esi + 0x48u, g_eax);
    g_esi = saved_esi;
}

/* 00037270: cdecl(a, record). 0003ABE60(this = record) decides; when non-zero 003BE4D0 also
 * runs on the record and the result is 1, else 0. ESI is saved and restored. */
GAME_REPLACE_EXACT(00037270, cdecl, 2, u32, game_record_error_check_returns_1_when_error)
{
    const uint32_t saved_esi = g_esi;
    g_esi = game_stack_arg(1u);
    g_ecx = g_esi;
    call_guest(0x003ABE60u, GAME_CC_thiscall, 0x0003727Cu, 4u, g_ecx, NULL, 0u);
    if ((g_eax & 0xFFu) == 0u) {
        g_eax = 0u;
        g_esi = saved_esi;
        return;
    }
    g_ecx = g_esi;
    call_guest(0x003BE4D0u, GAME_CC_thiscall, 0x00037287u, 4u, g_ecx, NULL, 0u);
    g_eax = 1u;
    g_esi = saved_esi;
}
