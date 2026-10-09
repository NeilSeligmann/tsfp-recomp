/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Three small leaf routines of the title's statically linked runtime that touch the x87
 * control word or the top of the x87 stack (T420). Written from our own reading of the
 * retail bytes (default.xbe, build 5849), register, stack, memory and x87 exact.
 *
 *   003CCE0D  set the control word to (argument & 0x300) | 0x7F: keep the precision
 *             control bits, force round-to-nearest and mask every exception
 *   003CCE24  if EAX bit 19 is clear, ST(0) += the double at 0x004B7164, EAX = 1,
 *             else EAX = 7 and the stack is untouched
 *   003CCE80  EAX = the high word of a double argument, masked to its exponent field
 *             unless the exponent is all ones, in which case the whole high word
 *
 * The lifted stack holds C doubles, so the rounding of the add is reproduced in integer
 * arithmetic for the precision and rounding control of the CURRENT control word: the exact
 * sum is rounded to 24, 53 or 64 mantissa bits under the control word's mode and then
 * narrowed to the nearest double, which is all the lifted stack can keep. EFLAGS is not
 * reproduced, except that 003CCE80 publishes its flags through the flag bridge (T554).
 */
#include "game_replace.h"
#include "x87_flags.h"

extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;
extern __thread uint16_t g_fp_control_word;
/* The flag bridge (T554, tools/config/flag_bridge.json): 003CCE80 is the provider. */
extern __thread uint32_t g_flag_bridge_eflags;
extern __thread uint32_t g_flag_bridge_mask;

#define FP_CONSTANT_ADDRESS 0x004B7164u
#define EXPONENT_FIELD 0x7FF00000u
#define SKIP_ADD_MASK 0x80000u

enum { RC_NEAREST = 0, RC_DOWN = 1, RC_UP = 2, RC_CHOP = 3 };

__extension__ typedef unsigned __int128 wide_uint;

static unsigned bit_length(wide_uint value)
{
    unsigned length = 0u;
    while (value != 0u) {
        length++;
        value >>= 1;
    }
    return length;
}

static uint64_t double_bits(double value)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double bits_double(uint64_t bits)
{
    double value;
    __builtin_memcpy(&value, &bits, sizeof value);
    return value;
}

/* Magnitude `quotient` * 2^`exponent` rounded to the nearest double, ties to even. */
static double pack_double(unsigned negative, wide_uint quotient, int exponent)
{
    uint64_t sign = (uint64_t)negative << 63;
    if (quotient == 0u) {
        return bits_double(sign);
    }
    int top = (int)bit_length(quotient) - 1 + exponent;
    if (top > 1023) {
        return bits_double(sign | 0x7FF0000000000000ull);
    }
    int shift; /* bits dropped from the right so that the kept mantissa has 53 bits */
    if (top >= -1022) {
        shift = (int)bit_length(quotient) - 53;
    } else {
        shift = -1074 - exponent; /* the grid of the subnormals */
    }
    wide_uint kept;
    if (shift <= 0) {
        kept = quotient << (unsigned)(-shift);
    } else if (shift >= 127) {
        kept = 0u;
    } else {
        kept = quotient >> (unsigned)shift;
        wide_uint remainder = quotient & ((((wide_uint)1u) << (unsigned)shift) - 1u);
        wide_uint half = ((wide_uint)1u) << (unsigned)(shift - 1);
        if (remainder > half || (remainder == half && (kept & 1u) != 0u)) {
            kept += 1u;
        }
    }
    if (top >= -1022) {
        if (kept == (((wide_uint)1u) << 53)) {
            kept >>= 1;
            top++;
            if (top > 1023) {
                return bits_double(sign | 0x7FF0000000000000ull);
            }
        }
        return bits_double(sign | ((uint64_t)(top + 1023) << 52) |
                           ((uint64_t)kept & 0xFFFFFFFFFFFFFull));
    }
    return bits_double(sign | (uint64_t)kept); /* a carry into 2^52 is the minimum normal */
}

/* ST(0) + `other` as the x87 computes it under control word `control`: the exact sum
 * rounded to the precision-control width with the rounding control, then narrowed to
 * the nearest double. */
static double x87_add(double left, double other, unsigned control)
{
    uint64_t left_bits = double_bits(left);
    uint64_t other_bits = double_bits(other);
    if ((left_bits >> 52 & 0x7FFu) == 0x7FFu || (other_bits >> 52 & 0x7FFu) == 0x7FFu) {
        return left + other; /* infinities and NaNs: the same class either way */
    }
    unsigned mode = (control >> 10) & 3u;
    unsigned precision_bits = ((control >> 8) & 3u) == 0u   ? 24u
                              : ((control >> 8) & 3u) == 2u ? 53u
                                                            : 64u;
    unsigned sign_left = (unsigned)(left_bits >> 63);
    unsigned sign_other = (unsigned)(other_bits >> 63);
    uint64_t mantissa_left = left_bits & 0xFFFFFFFFFFFFFull;
    uint64_t mantissa_other = other_bits & 0xFFFFFFFFFFFFFull;
    int exponent_left = (int)(left_bits >> 52 & 0x7FFu);
    int exponent_other = (int)(other_bits >> 52 & 0x7FFu);
    if (exponent_left != 0) mantissa_left |= 1ull << 52;
    if (exponent_other != 0) mantissa_other |= 1ull << 52;
    exponent_left = (exponent_left != 0 ? exponent_left : 1) - 1075;
    exponent_other = (exponent_other != 0 ? exponent_other : 1) - 1075;

    if (mantissa_left == 0u && mantissa_other == 0u) {
        unsigned negative = (sign_left == sign_other) ? sign_left : (mode == RC_DOWN);
        return bits_double((uint64_t)negative << 63);
    }
    /* Put the operand with the higher top bit first. A zero operand never leads, and
     * adding it still rounds the other operand to the precision-control width. */
    int top_left = mantissa_left == 0u ? -100000 : (int)bit_length(mantissa_left) + exponent_left;
    int top_other = mantissa_other == 0u ? -100000 : (int)bit_length(mantissa_other) + exponent_other;
    if (top_other > top_left) {
        uint64_t swap_mantissa = mantissa_left;
        mantissa_left = mantissa_other;
        mantissa_other = swap_mantissa;
        int swap_exponent = exponent_left;
        exponent_left = exponent_other;
        exponent_other = swap_exponent;
        unsigned swap_sign = sign_left;
        sign_left = sign_other;
        sign_other = swap_sign;
    }
    if (mantissa_other == 0u) {
        exponent_other = exponent_left;
    }

    /* Common grid 2^(exponent_left - 70): the big operand shifted up by 70 bits, the small
     * one aligned to it with a sticky flag for what falls off the right. */
    int grid = exponent_left - 70;
    wide_uint big = ((wide_uint)mantissa_left) << 70;
    wide_uint small;
    unsigned sticky = 0u;
    int offset = exponent_other - grid;
    if (offset >= 0) {
        small = ((wide_uint)mantissa_other) << (unsigned)offset;
    } else if (offset <= -64) {
        small = 0u;
        sticky = 1u;
    } else {
        small = ((wide_uint)mantissa_other) >> (unsigned)(-offset);
        sticky = (mantissa_other & ((1ull << (unsigned)(-offset)) - 1u)) != 0u;
    }

    unsigned negative = sign_left;
    wide_uint magnitude;
    if (sign_left == sign_other) {
        magnitude = big + small;
    } else if (big >= small) {
        magnitude = big - small - sticky; /* the dropped part becomes a fraction in (0, 1) */
    } else {
        magnitude = small - big;
        negative = sign_other;
    }
    if (magnitude == 0u && sticky == 0u) {
        return bits_double((uint64_t)(mode == RC_DOWN) << 63); /* exact cancellation */
    }

    int drop = (int)bit_length(magnitude) - (int)precision_bits;
    wide_uint quotient = magnitude;
    int scale = grid;
    if (drop > 0) {
        quotient = magnitude >> (unsigned)drop;
        wide_uint remainder = magnitude & ((((wide_uint)1u) << (unsigned)drop) - 1u);
        wide_uint half = ((wide_uint)1u) << (unsigned)(drop - 1);
        int inexact = remainder != 0u || sticky != 0u;
        int round_up = 0;
        if (mode == RC_NEAREST) {
            round_up = remainder > half || (remainder == half && sticky != 0u) ||
                       (remainder == half && (quotient & 1u) != 0u);
        } else if (mode == RC_DOWN) {
            round_up = inexact && negative;
        } else if (mode == RC_UP) {
            round_up = inexact && !negative;
        }
        if (round_up) {
            quotient += 1u;
        }
        scale += drop;
    }
    return pack_double(negative, quotient, scale);
}

/* ------------------------------------------------------------------------------------ */
/* 003CCE0D: control word = (argument & 0x300) | 0x7F. The original builds it in EDX and   */
/* stores the 16-bit word into the high half of the argument slot (esp + 6) to load it.   */
/* ------------------------------------------------------------------------------------ */
static uint32_t x87_control_word_for(uint32_t argument)
{
    return (argument & 0x300u) | 0x7Fu;
}

GAME_REPLACE_EXACT(003CCE0D, cdecl, 1, void, x87_control_word_for)
{
    uint32_t word = x87_control_word_for(guest_read32(g_esp + 4u));
    g_edx = word;
    guest_write8(g_esp + 6u, (uint8_t)word);
    guest_write8(g_esp + 7u, (uint8_t)(word >> 8));
    g_fp_control_word = (uint16_t)word;
}

/* ------------------------------------------------------------------------------------ */
/* 003CCE24: add the double at 0x004B7164 to ST(0) unless EAX bit 19 is set. Returns the  */
/* new EAX.                                                                               */
/* ------------------------------------------------------------------------------------ */
static uint32_t x87_add_constant(uint32_t flags)
{
    if ((flags & SKIP_ADD_MASK) != 0u) {
        return 7u;
    }
    uint64_t bits = (uint64_t)guest_read32(FP_CONSTANT_ADDRESS) |
                    ((uint64_t)guest_read32(FP_CONSTANT_ADDRESS + 4u) << 32);
    double *top = &g_fp_stack[g_fp_top & 7];
    *top = x87_add(*top, bits_double(bits), g_fp_control_word);
    return 1u;
}

GAME_REPLACE_EXACT(003CCE24, cdecl, 0, u32, x87_add_constant)
{
    g_eax = x87_add_constant(g_eax);
}

/* ------------------------------------------------------------------------------------ */
/* 003CCE80: exponent field of the high word of a double argument (second stack word).    */
/* Both exits of the original return the flags of `cmp eax, 0x7FF00000` (eax = the masked  */
/* field, the `mov` of the all-ones exit keeps them). The CRT acos and asin bodies         */
/* 003C9CE1 and 003C9E15 branch on that ZF, so a lift with the flag bridge reads the       */
/* word published here, and an older lift never reads it.                                  */
/* ------------------------------------------------------------------------------------ */
static uint32_t x87_exponent_word(uint32_t high)
{
    uint32_t field = high & EXPONENT_FIELD;
    return field == EXPONENT_FIELD ? high : field;
}

GAME_REPLACE_EXACT(003CCE80, cdecl, 2, u32, x87_exponent_word)
{
    uint32_t high = guest_read32(g_esp + 8u);
    g_eax = x87_exponent_word(high);
    g_flag_bridge_eflags = x87_cmp_flags(high & EXPONENT_FIELD, EXPONENT_FIELD);
    g_flag_bridge_mask = FLAG_BRIDGE_ALL;
}
