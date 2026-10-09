/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>

#include "game_replace.h"

extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;
extern __thread uint16_t g_fp_control_word;

/* __ctrlfp (0x003CC8F1): merge `value` into the old x87 control word under
 * `mask`, write the merged word back to the second stack slot, load it, and
 * return the old word sign-extended. Ghidra's generated C omitted both args. */
GAME_REPLACE_EXACT(003CC8F1, cdecl, 2, u32, game_crt_ctrlfp)
{
    uint32_t value = guest_read32(g_esp + 4u);
    uint32_t mask = guest_read32(g_esp + 8u);
    uint16_t old_control = g_fp_control_word;
    /* PUSH ECX reserves [EBP-4], then FNSTCW overwrites only its low word. The
     * following 32-bit read therefore includes the saved ECX high word. */
    uint32_t saved_local = (g_ecx & UINT32_C(0xffff0000)) | old_control;
    uint32_t merged = (saved_local & ~mask) | (value & mask);
    guest_write32(g_esp + 8u, merged);
    g_ecx = value & mask;
    g_fp_control_word = (uint16_t)merged;
    g_eax = (uint32_t)(int32_t)(int16_t)old_control;
}

/* __sptype (0x003CC7CB): 1=+infinity, 2=-infinity, 3=quiet NaN,
 * 4=signaling NaN, 0=finite. The original accepts the low/high words. */
static uint32_t game_crt_sptype(uint32_t low, uint32_t high)
{
    uint64_t bits = (uint64_t)low | ((uint64_t)high << 32);
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    uint64_t exponent = magnitude & UINT64_C(0x7ff0000000000000);
    uint64_t fraction = magnitude & UINT64_C(0x000fffffffffffff);

    if (bits == UINT64_C(0x7ff0000000000000)) return 1u;
    if (bits == UINT64_C(0xfff0000000000000)) return 2u;
    if (exponent != UINT64_C(0x7ff0000000000000) || fraction == 0u) return 0u;
    return (fraction & UINT64_C(0x0008000000000000)) != 0u ? 3u : 4u;
}

GAME_REPLACE(003CC7CB, cdecl, 2, u32, game_crt_sptype)

static double game_crt_frndint(double value, uint16_t control)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &value, sizeof bits);
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    unsigned sign = (unsigned)(bits >> 63);
    unsigned exponent = (unsigned)(magnitude >> 52);
    if (exponent == 0x7ffu) return value;
    if (magnitude == 0u) return value;

    int unbiased = exponent == 0u ? -1022 : (int)exponent - 1023;
    if (unbiased >= 52) return value;
    unsigned mode = (control >> 10) & 3u;
    if (unbiased < 0) {
        int round_away = mode == 1u ? sign != 0u
                        : mode == 2u ? sign == 0u
                        : mode == 0u &&
                              (unbiased == -1 && magnitude > UINT64_C(0x3fe0000000000000));
        if (round_away) {
            uint64_t result = ((uint64_t)sign << 63) | UINT64_C(0x3ff0000000000000);
            __builtin_memcpy(&value, &result, sizeof value);
            return value;
        }
        bits &= UINT64_C(0x8000000000000000);
        __builtin_memcpy(&value, &bits, sizeof value);
        return value;
    }

    unsigned fractional_bits = 52u - (unsigned)unbiased;
    uint64_t fraction_mask = (UINT64_C(1) << fractional_bits) - 1u;
    uint64_t fraction = magnitude & fraction_mask;
    uint64_t truncated = magnitude & ~fraction_mask;
    int round_away = 0;
    if (mode == 1u) round_away = sign != 0u && fraction != 0u;
    else if (mode == 2u) round_away = sign == 0u && fraction != 0u;
    else if (mode == 0u) {
        uint64_t half = UINT64_C(1) << (fractional_bits - 1u);
        round_away = fraction > half ||
                     (fraction == half && ((truncated >> fractional_bits) & 1u) != 0u);
    }

    uint64_t rounded_bits = truncated | ((uint64_t)sign << 63);
    __builtin_memcpy(&value, &rounded_bits, sizeof value);
    if (round_away) value += sign != 0u ? -1.0 : 1.0;
    return value;
}

/* __frnd (0x003CC790) loads its double argument, applies FRNDINT under the guest
 * control word, and leaves that value in ST(0). T704: it is register exact. The
 * original is `push ecx; push ecx; fld [esp+0xc]; frndint; fstp [esp]; fld [esp];
 * pop ecx; pop ecx; ret`, so it leaves EAX and EDX alone, never writes EFLAGS
 * (push, pop, x87 only: whatever flags the caller had survive the call) and
 * leaves ECX = the high dword of the rounded double as the FSTP stored it (the
 * last pop). FLD of a signaling NaN quiets it (bit 51), so ECX shows that. */
static uint32_t game_crt_frnd(uint32_t low, uint32_t high)
{
    uint64_t bits = (uint64_t)low | ((uint64_t)high << 32);
    double value;
    __builtin_memcpy(&value, &bits, sizeof value);
    value = game_crt_frndint(value, g_fp_control_word);
    g_fp_top = (g_fp_top - 1) & 7;
    g_fp_stack[g_fp_top] = value;
    uint64_t stored;
    __builtin_memcpy(&stored, &value, sizeof stored);
    if ((stored & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000) &&
        (stored & UINT64_C(0x000fffffffffffff)) != 0u)
        stored |= UINT64_C(0x0008000000000000);
    return (uint32_t)(stored >> 32);
}

GAME_REPLACE_EXACT(003CC790, cdecl, 2, void, game_crt_frnd)
{
    g_ecx = game_crt_frnd(game_stack_arg(0), game_stack_arg(1));
}
