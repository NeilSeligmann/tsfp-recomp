/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480/T1476 seventh batch: MD5 stack-context wrappers, a column table copy with thiscall
 * setters and a keyed list lookup. Each keeps the original nested-call frames (the
 * 0x54-byte context and argument words stay live below the entry ESP) and the register
 * hand-off, so callee EAX/ECX/EDX leftovers are the callees' own. */
#include "game_replace.h"

extern __thread int g_df;
extern __thread uint32_t g_ebx, g_esi, g_edi;

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, uint32_t this_pointer, const uint32_t *arguments,
                       unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, this_pointer, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 003C3D20: cdecl(object, data, total). When the object is keyed ([object] != 0) writes the
 * first 8 bytes of the MD5 of data[0 .. total-8) over the last 8 bytes. -1 when total < 8,
 * 0 otherwise. */
GAME_REPLACE_EXACT(003C3D20, cdecl, 3, u32,
                   game_fesl_md5_write_8_byte_digest_of_buffer_prefix_into_its_last_8_bytes)
{
    const uint32_t object = game_stack_arg(0u);
    g_ecx = guest_read32(object);
    if (g_ecx == 0u) {
        g_eax = 0u;
        return;
    }
    const uint32_t body = game_stack_arg(2u) - 8u;
    if ((int32_t)body < 0) {
        g_eax = 0xFFFFFFFFu;
        return;
    }
    const uint32_t context = g_esp - 0x54u;
    const uint32_t init[1] = {context};
    g_eax = object;
    g_ecx = context;
    call_guest(0x003BC270u, GAME_CC_cdecl, 0x003C3D4Cu, 0x5Cu, 0u, init, 1u);
    const uint32_t data = game_stack_arg(1u);
    const uint32_t update[3] = {context, data, body};
    g_edx = context;
    call_guest(0x003BC2A0u, GAME_CC_cdecl, 0x003C3D5Cu, 0x60u, 0u, update, 3u);
    const uint32_t final_block[3] = {context, body + data, 8u};
    g_eax = context;
    call_guest(0x003BC300u, GAME_CC_cdecl, 0x003C3D6Bu, 0x6Cu, 0u, final_block, 3u);
    g_eax = 0u;
}

/* 003C3C50: cdecl(object, data, length). Verifies the trailing 8 bytes of the buffer
 * against the first 8 bytes of the MD5 of the preceding length-8 bytes (digest buffer at
 * [entry ESP - 0x64]): 0 when equal, 0xFFFFFFFE when different, -1 when length < 8, 0 for
 * an unkeyed object. The two-dword compare follows the guest direction flag. */
GAME_REPLACE_EXACT(003C3C50, cdecl, 3, u32,
                   game_fesl_md5_verify_trailing_8_byte_digest_of_buffer_returning_minus_2_when_mismatch)
{
    const uint32_t object = game_stack_arg(0u);
    g_ecx = guest_read32(object);
    if (g_ecx == 0u) {
        g_eax = 0u;
        return;
    }
    g_ecx = guest_read32(object + 4u);
    if (g_ecx == 0u) {
        g_eax = 0u;
        return;
    }
    const uint32_t length = game_stack_arg(2u);
    if ((int32_t)length < 8) {
        g_eax = 0xFFFFFFFFu;
        return;
    }
    const uint32_t context = g_esp - 0x54u;
    const uint32_t digest = g_esp - 0x64u;
    const uint32_t init[1] = {context};
    /* The saved ESI word sits just below the digest buffer and is read by a descending
     * (direction flag set) second compare. */
    guest_write32(g_esp - 0x68u, g_esi);
    guest_write32(g_esp - 0x6Cu, g_ebx);
    guest_write32(g_esp - 0x70u, g_edi);
    g_eax = context;
    call_guest(0x003BC270u, GAME_CC_cdecl, 0x003C3C82u, 0x70u, 0u, init, 1u);
    const uint32_t data = game_stack_arg(1u);
    const uint32_t update[3] = {context, data, length - 8u};
    g_ecx = length - 8u;
    g_edx = context;
    call_guest(0x003BC2A0u, GAME_CC_cdecl, 0x003C3C95u, 0x74u, 0u, update, 3u);
    const uint32_t final_block[3] = {context, digest, 0x10u};
    g_eax = digest;
    g_ecx = context;
    call_guest(0x003BC300u, GAME_CC_cdecl, 0x003C3CA6u, 0x80u, 0u, final_block, 3u);
    const uint32_t step = g_df != 0 ? 0xFFFFFFFCu : 4u;
    uint32_t trailer = data + length - 8u;
    uint32_t computed = digest;
    int equal = guest_read32(trailer) == guest_read32(computed);
    g_ecx = 1u;
    if (equal) {
        trailer += step;
        computed += step;
        equal = guest_read32(trailer) == guest_read32(computed);
        g_ecx = 0u;
    }
    g_edx = 0u;
    g_eax = equal ? 0u : 0xFFFFFFFEu;
}

/* 003FB761: stdcall(source, destination). Copies eight columns of six table words and nine
 * single words from source to destination, then sets the destination's nibble fields
 * through the thiscall setters 003FA6A7 (twice) and 003FA6F2. */
GAME_REPLACE_EXACT(003FB761, stdcall, 2, u32, game_column_tables_copy_then_set_nibble_fields)
{
    static const uint16_t singles[9][2] = {{0x80u, 0x20u},   {0x84u, 0x24u},   {0x288u, 0xA8u},
                                           {0x28Cu, 0xACu},  {0x290u, 0xB0u},  {0x314u, 0xD4u},
                                           {0x318u, 0xD8u},  {0x31Cu, 0xDCu},  {0x320u, 0xE0u}};
    const uint32_t source = game_stack_arg(0u);
    const uint32_t destination = game_stack_arg(1u);
    for (uint32_t column = 0u; column < 8u; column++) {
        const uint32_t step = 4u * column;
        guest_write32(destination + step, guest_read32(source + step));
        guest_write32(destination + 0x88u + step, guest_read32(source + 0x28u + step));
        guest_write32(destination + 0x108u + step, guest_read32(source + 0x48u + step));
        guest_write32(destination + 0x188u + step, guest_read32(source + 0x68u + step));
        guest_write32(destination + 0x208u + step, guest_read32(source + 0x88u + step));
        guest_write32(destination + 0x294u + step, guest_read32(source + 0xB4u + step));
    }
    for (unsigned index = 0u; index < 9u; index++) {
        guest_write32(destination + singles[index][0], guest_read32(source + singles[index][1]));
    }
    const uint32_t first[2] = {0u, guest_read32(source + 0xE4u)};
    call_guest(0x003FA6A7u, GAME_CC_thiscall, 0x003FB824u, 12u, destination, first, 2u);
    const uint32_t second[2] = {1u, guest_read32(source + 0xE8u)};
    call_guest(0x003FA6A7u, GAME_CC_thiscall, 0x003FB833u, 12u, destination, second, 2u);
    const uint32_t third[1] = {guest_read32(source + 0xECu)};
    call_guest(0x003FA6F2u, GAME_CC_thiscall, 0x003FB840u, 12u, destination, third, 1u);
    g_eax = 0u;
}

/* 000DB480: cdecl(object, first_key, second_key). Walks the list at [object+0x230] for the
 * node whose two key words match and moves it to the tail with 000DB3B0 (registered with
 * scratch ECX/EDX). The original leaves EAX=object, ECX=the new count and EDX=the new tail
 * after that call, so the root sets exactly those. Not found: EAX=0, ECX=second, EDX=first. */
GAME_REPLACE_EXACT(000DB480, cdecl, 3, u32,
                   game_object_list_0x230_find_node_by_key_pair_and_move_to_tail)
{
    const uint32_t object = game_stack_arg(0u);
    uint32_t node = guest_read32(object + 0x230u);
    g_eax = node;
    if (node == 0u)
        return;
    const uint32_t first_key = game_stack_arg(1u);
    const uint32_t second_key = game_stack_arg(2u);
    g_ecx = second_key;
    g_edx = first_key;
    while (guest_read32(node) != first_key || guest_read32(node + 4u) != second_key) {
        node = guest_read32(node + 0x18u);
        g_eax = node;
        if (node == 0u)
            return;
    }
    const uint32_t arguments[2] = {object, node};
    call_guest(0x000DB3B0u, GAME_CC_cdecl, 0x000DB4B0u, 4u, 0u, arguments, 2u);
    g_eax = object;
    g_ecx = guest_read32(object + 0x238u);
    g_edx = guest_read32(object + 0x234u);
}
