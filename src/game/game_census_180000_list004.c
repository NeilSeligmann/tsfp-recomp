/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1478 census list-004. See docs/t1478-census.md. */
#include "game_replace.h"

/* Copy the selected prefix, append argument text and selected suffix. Preserve
 * original dword-then-byte copy order for overlapping guest buffers. */
GAME_REPLACE_EXACT(00195810, cdecl, 2, u32, game_text_build_string_from_arg_with_language_prefix_and_suffix_tables_by_7497a8_bits_4_5)
{
    uint32_t select = (guest_read32(0x007497A8u) >> 4u) & 3u;
    g_ecx = guest_read32(0x004E8E28u + select * 4u);
    select = (guest_read32(0x007497A8u) >> 4u) & 3u;
    g_edx = select * 4u;
    const uint32_t suffix = guest_read32(0x004E8E40u - g_edx);
    g_eax = game_stack_arg(1u);
    uint32_t target = g_eax;
    do {
        const uint8_t value = guest_read8(g_ecx);
        g_edx = (g_edx & 0xFFFFFF00u) | value;
        guest_write8(target++, value);
        ++g_ecx;
        if (value == 0u) break;
    } while (1);
    uint32_t source = game_stack_arg(0u);
    g_ecx = source;
    do {
        const uint8_t value = guest_read8(g_ecx++);
        g_edx = (g_edx & 0xFFFFFF00u) | value;
        if (value == 0u) break;
    } while (1);
    g_edx = g_ecx - source;
    target = g_eax;
    while (guest_read8(target) != 0u) ++target;
    g_ecx = g_edx / 4u;
    while (g_ecx != 0u) { guest_write32(target, guest_read32(source)); target += 4u; source += 4u; --g_ecx; }
    g_ecx = g_edx & 3u;
    while (g_ecx != 0u) { guest_write8(target++, guest_read8(source++)); --g_ecx; }
    g_ecx = suffix;
    while (guest_read8(g_ecx++) != 0u) { }
    const uint32_t length = g_ecx - suffix;
    g_edx = suffix;
    target = g_eax;
    uint8_t value;
    do { value = guest_read8(target++); g_ecx = (g_ecx & 0xFFFFFF00u) | value; } while (value != 0u);
    --target;
    source = suffix;
    g_ecx = length / 4u;
    while (g_ecx != 0u) { guest_write32(target, guest_read32(source)); target += 4u; source += 4u; --g_ecx; }
    g_ecx = length & 3u;
    while (g_ecx != 0u) { guest_write8(target++, guest_read8(source++)); --g_ecx; }
}

/* Refresh a matching timestamp, else append unless count equals ten. Re-read
 * signed counts at each original read, including after possibly aliased writes. */
GAME_REPLACE_EXACT(00271B40, cdecl, 2, u32, game_list_stride8_at_0xdc_add_or_stamp_entry_with_global_7de318_max_10)
{
    g_eax = game_stack_arg(0u);
    const uint16_t count_bits = (uint16_t)(guest_read8(g_eax + 0x10u) | ((uint16_t)guest_read8(g_eax + 0x11u) << 8u));
    g_edx = (g_edx & 0xFFFF0000u) | count_bits;
    if (count_bits == 10u) return;
    const int32_t count = (int16_t)count_bits;
    const uint32_t key = game_stack_arg(1u);
    g_ecx = 0u;
    if (count > 0) {
        g_edx = g_eax + 0xDCu;
        do {
            if (guest_read32(g_edx) == key) {
                g_edx = guest_read32(0x007DE318u);
                guest_write32(g_eax + g_ecx * 8u + 0xE0u, g_edx);
                return;
            }
            ++g_ecx;
            g_edx += 8u;
        } while ((int32_t)g_ecx < (int16_t)(guest_read8(g_eax + 0x10u) | ((uint16_t)guest_read8(g_eax + 0x11u) << 8u)));
    }
    g_ecx = guest_read32(0x007DE318u);
    guest_write32(g_eax + (uint32_t)count * 8u + 0xE0u, g_ecx);
    g_edx = (uint32_t)(int32_t)(int16_t)(guest_read8(g_eax + 0x10u) | ((uint16_t)guest_read8(g_eax + 0x11u) << 8u));
    guest_write32(g_eax + g_edx * 8u + 0xDCu, key);
    const uint16_t next = (uint16_t)((uint32_t)(guest_read8(g_eax + 0x10u) | ((uint16_t)guest_read8(g_eax + 0x11u) << 8u)) + 1u);
    guest_write8(g_eax + 0x10u, (uint8_t)next);
    guest_write8(g_eax + 0x11u, (uint8_t)(next >> 8u));
}

