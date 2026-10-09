/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Call-free leaf functions in the 0x00000000-0x00150000 range of the retail XBE.
 * Names are neutral: no enclosing structure or subsystem meaning is inferred.
 * Receipt: docs/t77-low-range-leaves.md
 */
#include "game_replace.h"

/* 0x00017540: nonzero when (obj[+0x18] & mask_lo) | (obj[+0x1C] & mask_hi) != 0.
 * ECX ends as obj[+0x1C] & mask_hi, EDX as mask_lo. */
GAME_REPLACE_EXACT(00017540, cdecl, 3, u32, game_masked_pair_test)
{
    const guest_addr object = game_stack_arg(0u);
    const uint32_t mask_lo = game_stack_arg(1u);
    g_edx = mask_lo;
    g_ecx = guest_read32(object + 0x1Cu) & game_stack_arg(2u);
    const uint32_t combined = (guest_read32(object + 0x18u) & mask_lo) | g_ecx;
    g_eax = combined != 0u ? 1u : 0u;
}

/* 0x00012F50: -1 gives 0, else the dword at (table_ptr 0x004F9BAC) + index*0x29C + 0xC. */
GAME_REPLACE_EXACT(00012F50, cdecl, 1, u32, game_indexed_record_field_c)
{
    const uint32_t index = game_stack_arg(0u);
    if (index == 0xFFFFFFFFu) {
        g_eax = 0u;
        return;
    }
    g_ecx = guest_read32(0x004F9BACu);
    g_eax = guest_read32(index * 0x29Cu + g_ecx + 0xCu);
}

/* 0x00012F30: bounds-checked (signed) dword array read: obj[+0x1FC][index], with the
 * global at 0x007AD288 returned when index >= obj[+0x1E8]. */
GAME_REPLACE_EXACT(00012F30, cdecl, 2, u32, game_bounded_array_read)
{
    g_ecx = game_stack_arg(0u);
    g_eax = game_stack_arg(1u);
    if ((int32_t)g_eax >= (int32_t)guest_read32(g_ecx + 0x1E8u)) {
        g_eax = guest_read32(0x007AD288u);
        return;
    }
    g_ecx = guest_read32(g_ecx + 0x1FCu);
    g_eax = guest_read32(g_ecx + g_eax * 4u);
}

/* 0x0001E730: negative index returns the global at 0x004E8CF0, else the 16-bit field at
 * +0x26 of the 0x34-byte record array whose base pointer is *(*0x004B85C8). */
GAME_REPLACE_EXACT(0001E730, cdecl, 1, u32, game_record_half_26)
{
    g_eax = game_stack_arg(0u);
    if ((int32_t)g_eax < 0) {
        g_eax = guest_read32(0x004E8CF0u);
        return;
    }
    g_ecx = guest_read32(0x004B85C8u);
    g_eax *= 0x34u;
    g_edx = guest_read32(g_ecx);
    g_eax = (uint32_t)guest_read8(g_eax + g_edx + 0x26u) |
            ((uint32_t)guest_read8(g_eax + g_edx + 0x27u) << 8);
}

/* 0x00021740: 1 unless the global at 0x00563024 equals 0x00563014 (ECX keeps its value). */
GAME_REPLACE_EXACT(00021740, cdecl, 0, u32, game_global_ne_563014)
{
    g_ecx = guest_read32(0x00563024u);
    g_eax = g_ecx != 0x00563014u ? 1u : 0u;
}
