#include "game_replace.h"
GAME_REPLACE_EXACT(003CFDBB, cdecl, 2, u32, game_encode_single_byte_or_error)
{
    g_ecx = game_stack_arg(0);
    if (g_ecx == 0u) {
        g_eax = 0u;
        return;
    }
    const uint32_t value = game_stack_arg(1) & 0xFFFFu;
    g_eax = (g_eax & 0xFFFF0000u) | value;
    if (value > 255u) {
        guest_write32(0x007729B8u, 42u);
        g_eax = 0xFFFFFFFFu;
        return;
    }
    guest_write8(g_ecx, (uint8_t)value);
    g_eax = 1u;
}
GAME_REPLACE_EXACT(003D1274, stdcall, 1, u32, game_initialize_tag_2bb5c755)
{
    g_eax = game_stack_arg(0);
    guest_write32(g_eax + 4u, 0u);
    guest_write32(g_eax, 0x2BB5C755u);
    g_eax = 1u;
}
GAME_REPLACE_EXACT(003E8836, stdcall, 3, u32, game_first_non_ff_signed_byte)
{
    uint32_t value = game_stack_arg(0) & 255u;
    if (value == 255u) {
        value = game_stack_arg(1) & 255u;
        if (value == 255u) value = game_stack_arg(2) & 255u;
    }
    g_eax = value < 128u ? value : value | 0xFFFFFF00u;
}
