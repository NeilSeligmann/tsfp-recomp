/* T1480 appendix: only all-six fixed admissions; docs/t1480-appendix-w4.md. */
#include "game_replace.h"

GAME_REPLACE_EXACT_INPUTS(003ABE60, cdecl, 0, u32, ecx, game_fesl_message_record_has_error)
{
    g_edx = guest_read32(g_ecx + 0x24u);
    g_eax = g_edx != 0;
}
GAME_REPLACE_EXACT(003B9F00, cdecl, 2, u32, game_fesl_parse_decimal_int_with_optional_sign_or_default_when_string_is_null)
{
    g_edx = game_stack_arg(0);
    if (g_edx == 0) { g_eax = game_stack_arg(1); return; }
    uint8_t byte = guest_read8(g_edx);
    int negative = byte == '-';
    if (byte == '+' || negative) ++g_edx;
    g_ecx = guest_read8(g_edx); g_eax = 0;
    while ((int8_t)(uint8_t)g_ecx >= '0' && (int8_t)(uint8_t)g_ecx <= '9') {
        g_ecx &= 15u; g_eax = g_eax * 10u + g_ecx; ++g_edx;
        g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_edx);
    }
    if (negative) g_eax = 0u - g_eax;
}
