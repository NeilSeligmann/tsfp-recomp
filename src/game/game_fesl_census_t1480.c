/* T1480 T1772 list-001: fixed six-tuple admissions. See docs/t1480-census-w4.md. */
#include "game_replace.h"
extern __thread int g_df;

GAME_REPLACE_EXACT(003B5ED0, cdecl, 0, u32, game_fesl_online_clear_16_entries_table_7720e8_and_field_772178)
{
    g_ecx = 0;
    for (g_eax = 0x7720E8u; g_eax < 0x772168u; g_eax += 8u) {
        if (guest_read32(g_eax) != 0) {
            guest_write32(g_eax, 0); guest_write32(g_eax + 4u, 0);
        }
    }
    guest_write32(0x772178u, 0);
}

GAME_REPLACE_EXACT(003BCD20, cdecl, 5, u32, game_fesl_set_four_fields_0x44_0x3c_0x48_0x4c_from_stack_args_return_zero)
{
    uint32_t object = game_stack_arg(0);
    g_ecx = game_stack_arg(2); guest_write32(object + 0x44u, g_ecx);
    g_edx = object; g_eax = game_stack_arg(1); guest_write32(g_edx + 0x3Cu, g_eax);
    g_ecx = object; g_edx = game_stack_arg(3); guest_write32(g_ecx + 0x48u, g_edx);
    g_eax = object; g_ecx = game_stack_arg(4); guest_write32(g_eax + 0x4Cu, g_ecx);
    g_eax = 0;
}

GAME_REPLACE_EXACT(003BD170, cdecl, 3, u32, game_fesl_format_four_bytes_as_dotted_decimal)
{
    g_eax = game_stack_arg(2);
    uint32_t destination = game_stack_arg(1);
    g_ecx = destination;
    if ((int32_t)g_eax <= 0) { g_eax = 0; return; }
    if ((int32_t)g_eax < 16) { guest_write8(destination, 0); g_eax = 0; return; }
    uint32_t source = game_stack_arg(0);
    for (uint32_t octet = 0; octet < 4; ++octet) {
        g_eax = guest_read8(source + 4u + octet);
        if (g_eax > 99u) {
            uint32_t value = g_eax;
            g_edx = value % 100u;
            g_eax = value / 100u;
            guest_write8(g_ecx++, (uint8_t)(g_eax + 48u));
            value = g_edx; g_edx = value % 10u; g_eax = value / 10u;
            guest_write8(g_ecx++, (uint8_t)(g_eax + 48u));
            g_eax = g_edx;
        }
        if (g_eax > 9u) {
            uint32_t value = g_eax;
            g_edx = value % 10u; g_eax = value / 10u;
            guest_write8(g_ecx++, (uint8_t)(g_eax + 48u)); g_eax = g_edx;
        }
        guest_write8(g_ecx, (uint8_t)(g_eax + 48u));
        if (octet != 3u) { ++g_ecx; guest_write8(g_ecx++, '.'); }
    }
    guest_write8(g_ecx + 1u, 0); g_eax = destination;
}

GAME_REPLACE_EXACT(003C0D00, cdecl, 1, u32, game_fesl_set_field_8_to_zero_on_arg_or_on_global_772378_when_arg_is_null)
{
    g_eax = game_stack_arg(0); g_ecx = 0;
    if (g_eax == 0) g_eax = 0x772378u;
    guest_write32(g_eax + 8u, 0);
}

GAME_REPLACE_EXACT(003C23C0, cdecl, 2, u32, game_fesl_asn1_oid_table_lookup_by_length_and_bytes_in_table_4b657c_stride_0x18)
{
    uint32_t length = game_stack_arg(1), index = 0;
    g_edx = 0x4B657Cu;
    for (;;) {
        g_ecx = guest_read32(g_edx);
        if ((int32_t)length >= (int32_t)g_ecx) {
            uint32_t source = game_stack_arg(0), target = g_edx + 4u;
            g_eax = 0;
            int equal = 1;
            while (g_ecx != 0) {
                equal = guest_read8(source) == guest_read8(target);
                uint32_t step = g_df ? 0xFFFFFFFFu : 1u;
                source += step; target += step; --g_ecx;
                if (!equal) break;
            }
            /* zero REP count inherits the preceding XOR's equality. */
            if (equal) { g_ecx = index * 3u; g_eax = guest_read32(0x4B6578u + g_ecx * 8u); return; }
        }
        g_eax = guest_read32(g_edx + 0x14u); g_edx += 0x18u; ++index;
        if (g_eax == 0) return;
    }
}

GAME_REPLACE_EXACT(003C3B90, cdecl, 3, u32, game_fesl_copy_32_and_52_byte_blocks_from_state_to_out_pointers_when_non_null_return_1)
{
    g_eax = game_stack_arg(0);
    for (uint32_t section = 0; section < 2; ++section) {
        uint32_t destination = game_stack_arg(section + 1u);
        if (destination != 0) {
            uint32_t source = g_eax + (section ? 0x20u : 0);
            g_ecx = section ? 13u : 8u;
            uint32_t step = g_df ? 0xFFFFFFFCu : 4u;
            while (g_ecx != 0) { guest_write32(destination, guest_read32(source)); source += step; destination += step; --g_ecx; }
        }
    }
    g_eax = 1;
}

GAME_REPLACE_EXACT(003C3DA0, cdecl, 3, u32, game_fesl_parse_dollar_prefixed_hex_string_into_64_bit_value_requiring_length_8_or_more)
{
    if (game_stack_arg(1) < 8u) { g_eax = 0; return; }
    uint32_t source = game_stack_arg(2) + 1u, high = 0;
    g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(source);
    g_ecx = 0;
    while ((g_eax & 255u) != 0) {
        high = (high << 4) | (g_ecx >> 28); g_ecx <<= 4;
        uint8_t byte = (uint8_t)g_eax;
        g_edx = byte >= '0' && byte < ':' ? 0x30u : 0x57u;
        g_eax = (uint8_t)(byte - (uint8_t)g_edx); g_edx = 0;
        g_ecx |= g_eax;
        g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(++source);
    }
    g_eax = game_stack_arg(0);
    guest_write32(g_eax + 4u, high); guest_write32(g_eax, g_ecx); g_eax = 1;
}

GAME_REPLACE_EXACT(003C3E00, cdecl, 2, u32, game_fesl_format_64_bit_value_as_dollar_prefixed_16_hex_digits_via_table_54d798)
{
    g_ecx = game_stack_arg(1);
    if (g_ecx == 0 || (g_ecx & 3u) != 0) { g_eax = 0; return; }
    g_eax = guest_read32(g_ecx);
    uint32_t high = guest_read32(g_ecx + 4u), destination = game_stack_arg(0);
    g_ecx = 0; g_edx = destination;
    for (uint32_t i = 0; i < 5; ++i) guest_write32(g_edx + i * 4u, 0);
    for (g_ecx = 16; g_ecx != 0; --g_ecx) {
        g_edx = g_eax; g_eax = (g_eax >> 4) | (high << 28);
        g_edx &= 15u; g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(0x54D798u + g_edx);
        high >>= 4; guest_write8(destination + g_ecx, (uint8_t)g_edx);
    }
    guest_write8(destination, '$'); g_eax = 1;
}
