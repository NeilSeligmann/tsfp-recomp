/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "game_replace.h"

/* x87 float-to-int64 helper at 0x003C848C. Takes ST(0), pops it and returns the
 * truncated 64-bit integer in EDX:EAX. The original rounds with the current control
 * word, then corrects the result by one using the float-rounded remainder, which
 * gives truncation toward zero in every rounding mode this game uses. */
extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;
extern __thread uint16_t g_fp_control_word;

enum { RC_NEAREST = 0, RC_DOWN = 1, RC_UP = 2 };

/* FISTP to 64 bits under rounding control `mode`; the integer indefinite on NaN or range.
 * Written with integer casts only so the replacement needs no libm. */
static int64_t game_round_int64(long double value, unsigned mode)
{
    if (value != value || value >= 9223372036854775808.0L || value <= -9223372036854775808.0L) {
        return INT64_MIN;
    }
    int64_t whole = (int64_t)value;
    long double fraction = value - (long double)whole;
    long double magnitude = fraction < 0.0L ? -fraction : fraction;
    int64_t step = value < 0.0L ? -1 : 1;
    if (mode == RC_NEAREST) {
        if (magnitude > 0.5L || (magnitude == 0.5L && (whole & 1) != 0)) {
            return whole + step;
        }
    } else if (mode == RC_DOWN) {
        if (fraction < 0.0L) {
            return whole - 1;
        }
    } else if (mode == RC_UP) {
        if (fraction > 0.0L) {
            return whole + 1;
        }
    }
    return whole;
}

static uint32_t game_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* The neighbouring float below (`up` = 0) or above (`up` = 1) a float given as bits. */
static uint32_t game_float_neighbour(uint32_t bits, int up)
{
    if ((bits & 0x7FFFFFFFu) == 0u) {
        return up ? 0x00000001u : 0x80000001u;
    }
    int negative = (bits >> 31) != 0u;
    return (negative == !up) ? bits + 1u : bits - 1u;
}

/* The x87 sequence `fild r; fsubp; fstp dword` for `input - r`, as float bits. The
 * subtraction rounds to 64 bits and the store to 24, both under the control word's mode.
 * Nearest is the host's own long double arithmetic. The directed modes need the exact
 * difference, which TwoSum recovers as `sum + error`, so the float is picked on the right
 * side of it. Rounding to 64 bits first and then to 24 in one direction equals rounding
 * once, because the float grid nests inside the extended one. */
static uint32_t game_remainder_bits(double input, int64_t integer, unsigned mode)
{
    long double left = (long double)input;
    long double right = -(long double)integer;
    long double sum = left + right;
    if (sum == 0.0L) {
        /* Equal operands: the zero is negative only when rounding toward minus infinity. */
        return mode == RC_DOWN ? 0x80000000u : 0u;
    }
    float narrowed = (float)sum;
    if (mode == RC_NEAREST) {
        return game_float_bits(narrowed);
    }
    long double virtual_right = sum - left;
    long double error = (left - (sum - virtual_right)) + (right - virtual_right);
    long double back = (long double)narrowed;
    int below = back < sum || (back == sum && error > 0.0L);
    int above = back > sum || (back == sum && error < 0.0L);
    int exact_positive = sum > 0.0L || (sum == 0.0L && error > 0.0L);
    uint32_t bits = game_float_bits(narrowed);
    int want_floor = mode == RC_DOWN || (mode == 3u && exact_positive);
    if (want_floor) {
        /* Largest float not above the exact value. */
        return above ? game_float_neighbour(bits, 0) : bits;
    }
    /* Smallest float not below the exact value. */
    return below ? game_float_neighbour(bits, 1) : bits;
}

/* Returns the truncated value and reports the ECX the original leaves behind. */
static uint64_t game_ftol_adjust(double input, uint32_t *ecx_out, int *ecx_written)
{
    unsigned mode = (g_fp_control_word >> 10) & 3u;
    uint64_t rounded = (uint64_t)game_round_int64((long double)input, mode);
    uint32_t low = (uint32_t)rounded;
    uint32_t high = (uint32_t)(rounded >> 32);
    *ecx_written = 0;

    uint32_t sign_word;
    if (low == 0u) {
        if ((high & 0x7FFFFFFFu) == 0u) {
            return rounded;
        }
        sign_word = high;
    } else {
        uint64_t input_bits;
        memcpy(&input_bits, &input, sizeof input_bits);
        sign_word = (uint32_t)(input_bits >> 32) & 0x80000000u;
    }

    uint32_t bits = game_remainder_bits(input, (int64_t)rounded, mode);
    *ecx_written = 1;
    if ((int32_t)sign_word < 0) {
        uint64_t sum = (uint64_t)(bits ^ 0x80000000u) + 0x7FFFFFFFu;
        *ecx_out = (uint32_t)sum;
        return rounded + (sum >> 32);
    }
    uint64_t sum = (uint64_t)bits + 0x7FFFFFFFu;
    *ecx_out = (uint32_t)sum;
    return rounded - (sum >> 32);
}

/* ECX is left holding the float bits of the remainder on the paths that adjust. The
 * whole x87 operand is consumed, so the stack depth drops by one. */
GAME_REPLACE_EXACT(003C848C, cdecl, 0, u32, game_ftol_adjust)
{
    double input = g_fp_stack[g_fp_top & 7];
    uint32_t ecx_value = 0u;
    int ecx_written = 0;
    uint64_t result = game_ftol_adjust(input, &ecx_value, &ecx_written);
    g_fp_top = (g_fp_top + 1) & 7;
    g_eax = (uint32_t)result;
    g_edx = (uint32_t)(result >> 32);
    if (ecx_written) {
        g_ecx = ecx_value;
    }
}
