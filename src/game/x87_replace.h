/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The x87 replacement ABI (T1510). A replacement that leaves a float on the guest x87 stack or
 * computes one needs more than game_replace.h gives it, so this header adds three things and
 * touches no shared header.
 *
 * 1. PRIMITIVES, x87_fld32 / x87_fmul32 / x87_fstp32 (m32 operand at a guest address) and the
 *    register-only x87_fxch1 / x87_fprem_step / x87_fstp1 (T1510, 3C9DE8: fxch st(1), ONE fprem
 *    iteration returning the status word, fstp st(1)).
 *    Exactly one backend is compiled:
 *      - production (default): the lifted code's own double-backed model (g_fp_stack, g_fp_top,
 *        g_fp_control_word), operation for operation what the lifter emits for fld / fmul /
 *        fstp m32 (fp_push, RECOMP_FP_PC, (float) store, fp_pop). A lifted caller reads its
 *        float result from exactly that model, so a replacement MUST write it there. Real
 *        host fld/fmul/fstp would leave the result where no lifted caller looks.
 *      - TSFP_X87_BACKEND_RAW (proof builds only): the isolated raw-state runtime
 *        (tools/harness/x87_runtime.c): 80-bit slots, tags, control and status word, which the
 *        model-arbitrated-x87-v2 proof compares strictly. The same readable bodies compile
 *        against either backend, so the proved source is the shipped source. The BACKEND
 *        ITSELF differs, and the equivalence of the double model to the raw runtime for these
 *        three operations (PC 53 or 64, round-to-nearest, no exception) is INFERRED, not
 *        proved, see docs/replace-x87-t1510.md.
 *
 * 2. THE CHECKED RETURN. A replacement body opens a frame with x87_frame_open() and leaves it
 *    ONLY through x87_checked_return(frame, net_push). It verifies that the body left guest esp
 *    where it found it (the adapter then pops the return slot and the cdecl-declared
 *    arguments), that the return slot still holds the caller's return address, and that the x87 TOP moved
 *    by exactly the declared net push count (CB3B0 +1, the others 0). Any violation traps
 *    (fail closed: a fault, never a repaired frame).
 *
 * 3. THE REGISTRY. Roots using this ABI are registered in src/game/x87_roots.inc (included by
 *    x87_roots.c) with GAME_REPLACE_EXACT, so tools/replace scans, wires, manifests and
 *    proves them like any replacement. The model-arbitrated-x87-v2 contract is the only one
 *    allowed to prove them (docs/replace-x87-t1510.md), and only that contract compiles them
 *    for the raw backend.
 */
#ifndef TSFP_GAME_X87_REPLACE_H
#define TSFP_GAME_X87_REPLACE_H

#include <stdint.h>

#include "game_replace.h"

#if defined(TSFP_X87_BACKEND_RAW)
/* tools/harness/x87_runtime.h (proof builds link x87_runtime.c and x87_native.c). */
void harness_x87_copy(unsigned char state[86]);
void harness_x87_fld32(uint32_t address);
void harness_x87_fmul32(uint32_t address);
void harness_x87_fstp32(uint32_t address);
void harness_x87_fxch1(void);
void harness_x87_fprem(void);
void harness_x87_fstp1(void);

static inline void x87_fld32(guest_addr address) { harness_x87_fld32(address); }
static inline void x87_fmul32(guest_addr address) { harness_x87_fmul32(address); }
static inline void x87_fstp32(guest_addr address) { harness_x87_fstp32(address); }
static inline void x87_fxch1(void) { harness_x87_fxch1(); }
static inline void x87_fstp1(void) { harness_x87_fstp1(); }
/* One real FPREM iteration (host x87, the proved path). Returns the status word, whose C2 bit
 * (0x400) says the reduction is incomplete and the caller must iterate, exactly as the
 * original's fnstsw ax / sahf / jp loop does. */
static inline uint32_t x87_fprem_step(void)
{
    unsigned char state[86];
    harness_x87_fprem();
    harness_x87_copy(state);
    return (uint32_t)((unsigned)state[85] << 8 | state[84]);
}

static inline unsigned x87_top(void)
{
    unsigned char state[86];
    harness_x87_copy(state);
    return (unsigned)(((unsigned)state[85] << 8 | state[84]) >> 11) & 7u;
}
#else
#include <math.h>

extern __thread double g_fp_stack[8];
extern __thread int g_fp_top;
extern __thread uint16_t g_fp_control_word;

static inline float x87_guest_float(guest_addr address)
{
    float value;
    uint32_t bits = guest_read32(address);
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* The lifter's PC=24 narrowing (recomp_fp_round24): significand only, exponent range kept. */
static inline double x87_round24(double value)
{
    double magnitude = fabs(value);
    int exponent;
    if ((magnitude <= 3.4028234663852886e38 && magnitude >= 1.1754943508222875e-38) ||
        magnitude == 0.0 || value != value || magnitude == INFINITY) {
        return (double)(float)value;
    }
    value = frexp(value, &exponent);
    return ldexp((double)(float)value, exponent);
}

static inline void x87_fld32(guest_addr address)
{
    double value = (double)x87_guest_float(address);
    g_fp_top = (int)(((unsigned)g_fp_top + 7u) & 7u);
    g_fp_stack[g_fp_top] = value;
}

static inline void x87_fmul32(guest_addr address)
{
    double product = g_fp_stack[g_fp_top] * (double)x87_guest_float(address);
    g_fp_stack[g_fp_top] = (g_fp_control_word & 0x300u) ? product : x87_round24(product);
}

static inline void x87_fstp32(guest_addr address)
{
    float value = (float)g_fp_stack[g_fp_top];
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    guest_write32(address, bits);
    g_fp_top = (int)(((unsigned)g_fp_top + 1u) & 7u);
}

static inline unsigned x87_top(void) { return (unsigned)g_fp_top & 7u; }

static inline void x87_fxch1(void)
{
    unsigned other = ((unsigned)g_fp_top + 1u) & 7u;
    double top = g_fp_stack[g_fp_top];
    g_fp_stack[g_fp_top] = g_fp_stack[other];
    g_fp_stack[other] = top;
}

static inline void x87_fstp1(void)
{
    g_fp_stack[((unsigned)g_fp_top + 1u) & 7u] = g_fp_stack[g_fp_top];
    g_fp_top = (int)(((unsigned)g_fp_top + 1u) & 7u);
}

/* Exact C99 fmod without libm (the game objects are linked into targets that do not link
 * libm, and fmod, unlike frexp/ldexp, lives there): shift and subtract on the significands,
 * the sign of the dividend, a NaN for a zero divisor, an infinite or NaN dividend or a NaN
 * divisor, the dividend for an infinite divisor. Tested against libm fmod in
 * tests/c/test_x87_roots.c. */
static inline double x87_fmod_exact(double x, double y)
{
    uint64_t xb, yb;
    memcpy(&xb, &x, sizeof xb);
    memcpy(&yb, &y, sizeof yb);
    const uint64_t sign = xb & (1ull << 63);
    const uint64_t ax = xb & ~(1ull << 63), ay = yb & ~(1ull << 63);
    const uint64_t infinity = 0x7FF0000000000000ull;
    if (ay == 0u || ax >= infinity || ay > infinity) {
        return (x * y) / (x * y); /* invalid operation: a NaN */
    }
    if (ay == infinity || ax < ay) {
        return x;
    }
    /* Both significands normalised to bit 52 (a denormal drives its exponent to 0 or below), so
     * each step of the long division needs one conditional subtraction. */
    int ex = (int)(ax >> 52), ey = (int)(ay >> 52);
    uint64_t mx = ax & ((1ull << 52) - 1u), my = ay & ((1ull << 52) - 1u);
    if (ex != 0) { mx |= 1ull << 52; } else { for (ex = 1; mx < (1ull << 52); --ex) { mx <<= 1; } }
    if (ey != 0) { my |= 1ull << 52; } else { for (ey = 1; my < (1ull << 52); --ey) { my <<= 1; } }
    for (; ex > ey; --ex) {
        if (mx >= my) { mx -= my; }
        mx <<= 1;
    }
    if (mx >= my) { mx -= my; }
    uint64_t bits = sign;
    if (mx != 0u) {
        for (; mx < (1ull << 52); --ey) { mx <<= 1; }
        if (ey > 0) {
            bits |= (mx & ((1ull << 52) - 1u)) | ((uint64_t)ey << 52);
        } else {
            bits |= mx >> (1 - ey); /* denormal result: the shifted-out bits are zero */
        }
    }
    double result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

/* Production backend of the FPREM iteration: the lifter's own model (an exact fmod, which
 * reduces completely, so the loop runs once) and a status word of the TOP field only. The lifted
 * original reported TOP | g_fp_cc (a compare condition code that is not an FPREM result); the
 * quotient bits C0/C1/C3 and C2 of a real FPREM are NOT represented by the double model. */
static inline uint32_t x87_fprem_step(void)
{
    g_fp_stack[g_fp_top] =
        x87_fmod_exact(g_fp_stack[g_fp_top], g_fp_stack[((unsigned)g_fp_top + 1u) & 7u]);
    return ((unsigned)g_fp_top & 7u) << 11;
}
#endif

/* An open replacement frame: where esp and the x87 TOP were on entry. */
typedef struct x87_frame {
    uint32_t entry_esp;
    uint32_t return_slot;
    unsigned entry_top;
} x87_frame;

static inline x87_frame x87_frame_open(void)
{
    x87_frame frame;
    frame.entry_esp = g_esp;
    frame.return_slot = guest_read32(g_esp);
    frame.entry_top = x87_top();
    return frame;
}

/* The only way a body returns. `net_push` is the number of values the original leaves on the
 * x87 stack (+1) or removes from it (-1) beyond what it consumed, i.e. the TOP decrement. */
static inline void x87_checked_return(x87_frame frame, int net_push)
{
    unsigned expected_top = (unsigned)((int)frame.entry_top - net_push) & 7u;
    if (g_esp != frame.entry_esp || (frame.entry_esp & 3u) != 0u ||
        guest_read32(frame.entry_esp) != frame.return_slot || x87_top() != expected_top) {
        __builtin_trap();
    }
}

#endif /* TSFP_GAME_X87_REPLACE_H */
