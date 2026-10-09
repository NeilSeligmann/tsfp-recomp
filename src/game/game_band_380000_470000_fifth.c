/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480 fifth batch: call-bearing roots in 0x380000-0x470000. Each root keeps the
 * original nested-call frame (return PC, caller-live words) and register hand-off through
 * game_guest_call, so callee EAX/ECX/EDX leftovers are the callee's own, never constants. */
#include "game_replace.h"

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, 0u, 0u, arguments, count) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 003CC96B: cdecl(block). Size of a process-heap block: RtlSizeHeap(heap, 0, block) where
 * heap is the global returned by leaf 00382097. */
GAME_REPLACE_EXACT(003CC96B, cdecl, 1, u32, game_crt_msize_via_HeapSize_on_process_heap_wrapper)
{
    const uint32_t block = game_stack_arg(0u);
    const uint32_t entry_edx = g_edx;
    guest_write32(g_esp - 4u, block);
    guest_write32(g_esp - 8u, 0u);
    call_guest(0x00382097u, GAME_CC_cdecl, 0x003CC976u, 8u, NULL, 0u);
    const uint32_t arguments[3] = {g_eax, 0u, block};
    call_guest(0x00382CD6u, GAME_CC_stdcall, 0x003CC97Cu, 0u, arguments, 3u);
    /* The registered size replacement leaves ECX/EDX alone, the original leaves the block
     * pointer (flag bit 0 clear or bit 3 set), the header byte at -0xA (plain header) and
     * the header word at -0x10 in EDX (bit 3 set). Recompute those leftovers. */
    const uint32_t frame_block = guest_read32(g_esp - 4u);
    const uint8_t flags = guest_read8(frame_block - 0xBu);
    g_ecx = frame_block;
    g_edx = entry_edx;
    if ((flags & 1u) != 0u) {
        if ((flags & 8u) != 0u) {
            uint16_t header_word;
            memcpy(&header_word, game_host_ptr(frame_block - 0x10u), sizeof header_word);
            g_edx = header_word;
        } else {
            g_ecx = guest_read8(frame_block - 0xAu);
        }
    }
}

/* 0040C52C: stdcall(list_head, node). Adds the node's size field (+8) to the head's, then
 * unlinks the node (leaf 004067C0 resets it to a self-linked node). Returns the head. */
GAME_REPLACE_EXACT(0040C52C, stdcall, 2, u32, game_list_absorb_node_size_and_unlink)
{
    const uint32_t head = game_stack_arg(0u);
    const uint32_t node = game_stack_arg(1u);
    const uint32_t node_size = guest_read32(node + 8u);
    guest_write32(head + 8u, guest_read32(head + 8u) + node_size);
    g_eax = node;
    g_ecx = node_size;
    const uint32_t arguments[1] = {node};
    call_guest(0x004067C0u, GAME_CC_stdcall, 0x0040C541u, 4u, arguments, 1u);
    g_eax = head;
}

/* 0040B288: stdcall(anchor, list). Moves the list's first node behind the anchor with
 * 0040A4AE unless the list is empty (first == list). Returns the first node. */
GAME_REPLACE_EXACT(0040B288, stdcall, 2, u32, game_list_move_first_after_anchor)
{
    const uint32_t anchor = game_stack_arg(0u);
    const uint32_t list = game_stack_arg(1u);
    g_eax = list;
    const uint32_t first = guest_read32(list);
    if (first != list) {
        const uint32_t arguments[2] = {anchor, first};
        call_guest(0x0040A4AEu, GAME_CC_stdcall, 0x0040B29Du, 4u, arguments, 2u);
    }
    g_eax = first;
}

/* 0042F13F: stdcall(object). Stores the span (+0x24 minus +0x10) at +0x18. A span below
 * the limit at +0x14 flags error 0x3EA down the parent chain (0042EA8D). Returns 1. */
GAME_REPLACE_EXACT(0042F13F, stdcall, 1, u32, game_range_span_check_flag_error_0x3ea)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t span = guest_read32(object + 0x24u) - guest_read32(object + 0x10u);
    const uint32_t limit = guest_read32(object + 0x14u);
    guest_write32(object + 0x18u, span);
    g_eax = span;
    g_ecx = object;
    if (span < limit) {
        const uint32_t arguments[2] = {object, 0x3EAu};
        call_guest(0x0042EA8Du, GAME_CC_stdcall, 0x0042F15Cu, 0u, arguments, 2u);
    }
    g_eax = 1u;
}

/* 003CAB7B: cdecl(value, buffer, radix). _itoa: signed negative sign flag only for radix
 * 10. Leaves ECX as the second popped word (the sign flag) and returns the buffer. */
GAME_REPLACE_EXACT(003CAB7B, cdecl, 3, u32, game_crt_itoa)
{
    const uint32_t value = game_stack_arg(0u);
    const uint32_t radix = game_stack_arg(2u);
    const uint32_t negative = (radix == 10u && (int32_t)value < 0) ? 1u : 0u;
    const uint32_t arguments[2] = {radix, negative};
    g_eax = value;
    g_ecx = game_stack_arg(1u);
    call_guest(0x003CAB3Du, GAME_CC_cdecl, 0x003CAB9Eu, 4u, arguments, 2u);
    g_ecx = guest_read32(g_esp - 8u);
    g_eax = game_stack_arg(1u);
}

/* 0044B4AE: stdcall(reader, index, selector, output). Reads a field of table[row] bits
 * from the bit reader (00448F26), maps it through the row's pointer table and stores the
 * value at output[index]. row is index+9 for selectors below 13, else index. */
GAME_REPLACE_EXACT(0044B4AE, stdcall, 4, u32, game_bitstream_read_table_entry)
{
    const uint32_t reader = game_stack_arg(0u);
    const uint32_t index = game_stack_arg(1u);
    const uint32_t row = (((game_stack_arg(2u) < 0xDu) ? index + 9u : index)) << 3;
    uint16_t bits;
    memcpy(&bits, game_host_ptr(0x0046C470u + row), sizeof bits);
    g_eax = bits;
    const uint32_t arguments[2] = {reader, bits};
    call_guest(0x00448F26u, GAME_CC_stdcall, 0x0044B4D6u, 12u, arguments, 2u);
    g_ecx = guest_read32(0x0046C474u + row);
    g_eax = guest_read32(g_ecx + g_eax * 4u);
    g_ecx = game_stack_arg(3u);
    guest_write32(g_ecx + index * 4u, g_eax);
}

/* 003C3C20: cdecl(state, buffer, length). When the state is keyed ([state] != 0), runs the
 * 003C6890 stream cipher on buffer over the state at +8, then marks +4. EAX/ECX/EDX stay
 * the caller's when the state is unkeyed. */
GAME_REPLACE_EXACT(003C3C20, cdecl, 3, u32, game_fesl_stream_cipher_decrypt_buffer_with_rc4_state_at_field_8_when_enabled_and_set_flag_field_4)
{
    const uint32_t state = game_stack_arg(0u);
    if (guest_read32(state) != 0u) {
        const uint32_t buffer = game_stack_arg(1u);
        const uint32_t length = game_stack_arg(2u);
        const uint32_t arguments[3] = {state + 8u, buffer, length};
        g_eax = length;
        g_ecx = buffer;
        g_edx = state + 8u;
        call_guest(0x003C6890u, GAME_CC_cdecl, 0x003C3C3Du, 4u, arguments, 3u);
        guest_write32(state + 4u, 1u);
    }
}

/* 003E968A: stdcall(packed, base). Classifies the packed operand through 003E95F5 and
 * 003E9651. When flagged, ORs the operand's bits 16-19 into the byte at base + (packed &
 * 0xFFF); EAX is that address, otherwise the classifier's zero. */
GAME_REPLACE_EXACT(003E968A, stdcall, 2, u32, game_register_field_or_high_nibble)
{
    const uint32_t first[1] = {game_stack_arg(0u)};
    call_guest(0x003E95F5u, GAME_CC_stdcall, 0x003E9693u, 0u, first, 1u);
    const uint32_t second[1] = {g_eax};
    call_guest(0x003E9651u, GAME_CC_stdcall, 0x003E9699u, 0u, second, 1u);
    if ((g_eax & 0xFFu) != 0u) {
        const uint32_t packed = game_stack_arg(0u);
        const uint32_t address = (packed & 0xFFFu) + game_stack_arg(1u);
        const uint32_t high = packed >> 16;
        g_eax = address;
        g_ecx = (high & ~0xFFu) | (high & 0xFu);
        guest_write8(address, (uint8_t)(guest_read8(address) | (uint8_t)(g_ecx & 0xFFu)));
    }
}
