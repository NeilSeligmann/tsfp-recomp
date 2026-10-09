/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1477 appendix draft 0x001515E0 (fp-scalar-v1: movss movaps(reg) subss comiss addss; cdecl, 3
 * stack args, no register input). Run with --live-vector-state --vector-mode fp-scalar-v1 and
 * build with -ffp-contract=off -fno-fast-math. The helper block is the fixed-destination scalar
 * helper set of game_fp_step2b.c (same names and semantics, x86 two-NaN operand order and the real
 * MXCSR flags by inline asm on x86-64 SSE2, portable model elsewhere), plus fp_movaps_reg.
 * Original behavior: arg0 = pointer P to a float "current" value, arg1 = float target T (stack
 * slot [esp+8]), arg2 = float step S ([esp+12]). Moves *P toward T by at most S:
 *   1. xmm0 = T (movss load: lanes 1..3 zero). eax = P. xmm1 = copy of xmm0 (movaps register
 *      form, all 128 bits). xmm1 = xmm1 - *P (subss, destination T).
 *   2. The difference is stored over the arg1 slot [esp+8] (a REAL guest write). ecx = that slot
 *      reloaded, xmm1 = S (movss load, lanes 1..3 zero), ecx &= 0x7FFFFFFF, the absolute value
 *      bits are stored back to [esp+8]. The stack arg1 slot therefore leaves the call holding
 *      |T - *P| (bits); ECX leaves holding the same bits.
 *   3. comiss S, [esp+8] (S vs |T - *P|), `jbe` taken for S <= |diff| or unordered. Otherwise
 *      (ordered S > |diff|, arrived): *P = xmm0 (T bits), EAX = 1, return.
 *   4. Else: comiss T, *P (reads *P first), xmm0 = *P (movss load, a second read of *P, lanes
 *      1..3 zero). Ordered T > *P (`ja`): xmm0 = *P + S (addss, destination *P) stored to *P.
 *      Otherwise (T <= *P or unordered): xmm0 = *P - S (subss, destination *P) stored to *P.
 *      EAX = 0, return.
 * P may alias the stack (arg slots, return slot): every guest read and store keeps the original
 * order, so *P is reread after the [esp+8] stores. No guest PUSH/POP, no guest globals.
 * Register exit contract: EAX = 1 or 0, ECX = |T - *P| bits (low 31 bits of the difference),
 * EDX unchanged. XMM0 = {T,0,0,0} (arrived) or {*P +- S,0,0,0}, XMM1 = {S,0,0,0}, XMM2..7
 * untouched. Reads: one dword each of [esp+4], [esp+8], [esp+12], *P (x3 on the move path).
 * Stores: [esp+8] twice, then *P once.
 * The overlay name is the curated INFERRED name and matches the body. */
#include <string.h>

#include "game_replace.h"

typedef union GameFpXmm {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} GameFpXmm;

/* The runtime owns the strong slots; the weak definitions let registry-only links resolve them. */
__thread GameFpXmm g_xmm0 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm1 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm2 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm3 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm4 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm5 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm6 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm7 __attribute__((weak, aligned(8)));

#if defined(__x86_64__) && defined(__SSE2__) && !defined(GAME_FP_FORCE_PORTABLE)
#define GAME_FP_FIXED_DEST 1
#else
#define GAME_FP_FIXED_DEST 0
#endif
#define GAME_FP_HELPER static __attribute__((unused))

GAME_FP_HELPER float fp_bits_to_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

#if GAME_FP_FIXED_DEST
/* addss/subss xmm_dest, xmm_or_m32_source: dest = dest OP source. */
GAME_FP_HELPER float fp_addss(float dest, float source)
{
    __asm__ volatile("addss %1, %0" : "+x"(dest) : "xm"(source));
    return dest;
}

GAME_FP_HELPER float fp_subss(float dest, float source)
{
    __asm__ volatile("subss %1, %0" : "+x"(dest) : "xm"(source));
    return dest;
}

/* comiss a, b ; ja : taken only for an ordered a > b (CF=0 and ZF=0). */
GAME_FP_HELPER int fp_comiss_a(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@cca"(taken) : "x"(a), "xm"(b));
    return taken;
}
#else
/* x86 two-NaN rule: first operand's NaN (quieted), else second's, else the default NaN for an
 * invalid operation, else the arithmetic result. */
GAME_FP_HELPER float fp_x86_ss_result(float dest, float source, float arithmetic)
{
    uint32_t dest_bits;
    uint32_t source_bits;
    uint32_t result_bits;
    memcpy(&dest_bits, &dest, sizeof dest_bits);
    memcpy(&source_bits, &source, sizeof source_bits);
    memcpy(&result_bits, &arithmetic, sizeof result_bits);
    if ((dest_bits & 0x7FFFFFFFu) > 0x7F800000u) return fp_bits_to_float(dest_bits | 0x00400000u);
    if ((source_bits & 0x7FFFFFFFu) > 0x7F800000u) return fp_bits_to_float(source_bits | 0x00400000u);
    if ((result_bits & 0x7FFFFFFFu) > 0x7F800000u) return fp_bits_to_float(0xFFC00000u);
    return arithmetic;
}

GAME_FP_HELPER float fp_addss(float dest, float source)
{
    return fp_x86_ss_result(dest, source, dest + source);
}

GAME_FP_HELPER float fp_subss(float dest, float source)
{
    return fp_x86_ss_result(dest, source, dest - source);
}

GAME_FP_HELPER int fp_comiss_a(float a, float b)
{
    return a > b;
}
#endif

GAME_FP_HELPER float fp_guest_float(uint32_t address)
{
    return fp_bits_to_float(guest_read32(address));
}

/* movss xmm, dword ptr [address]: lane 0 loaded, lanes 1..3 zeroed. */
GAME_FP_HELPER void fp_load_ss(GameFpXmm *reg, uint32_t address)
{
    reg->u[0] = guest_read32(address);
    reg->u[1] = 0u;
    reg->u[2] = 0u;
    reg->u[3] = 0u;
}

/* movss dword ptr [address], xmm: lane 0 stored (raw bits). */
GAME_FP_HELPER void fp_store_ss(uint32_t address, const GameFpXmm *reg)
{
    guest_write32(address, reg->u[0]);
}

/* movaps xmm_dest, xmm_source (register form): all 128 bits copied. */
GAME_FP_HELPER void fp_movaps_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->q[0] = source->q[0];
    dest->q[1] = source->q[1];
}

GAME_REPLACE_EXACT(001515E0, cdecl, 3, u32, game_float_approach_target_step_returns_arrived)
{
    fp_load_ss(&g_xmm0, g_esp + 8u);                           /* movss xmm0, [esp+8]: T */
    g_eax = game_stack_arg(0u);                                /* mov eax, [esp+4]: P */
    fp_movaps_reg(&g_xmm1, &g_xmm0);                           /* movaps xmm1, xmm0 */
    g_xmm1.f[0] = fp_subss(g_xmm1.f[0], fp_guest_float(g_eax)); /* subss xmm1, [eax]: T - *P */
    fp_store_ss(g_esp + 8u, &g_xmm1);                          /* movss [esp+8], xmm1 */
    g_ecx = guest_read32(g_esp + 8u);                          /* mov ecx, [esp+8] */
    fp_load_ss(&g_xmm1, g_esp + 0xCu);                         /* movss xmm1, [esp+12]: S */
    g_ecx &= 0x7FFFFFFFu;                                      /* |diff| bits */
    guest_write32(g_esp + 8u, g_ecx);                          /* mov [esp+8], ecx */
    if (fp_comiss_a(g_xmm1.f[0], fp_guest_float(g_esp + 8u))) { /* comiss xmm1, [esp+8]; ja */
        fp_store_ss(g_eax, &g_xmm0);                           /* *P = T */
        g_eax = 1u;
        return;
    }
    const int target_above = fp_comiss_a(g_xmm0.f[0], fp_guest_float(g_eax)); /* comiss xmm0, [eax] */
    fp_load_ss(&g_xmm0, g_eax);                                /* movss xmm0, [eax]: *P */
    if (target_above) {
        g_xmm0.f[0] = fp_addss(g_xmm0.f[0], g_xmm1.f[0]);      /* addss xmm0, xmm1 */
    } else {
        g_xmm0.f[0] = fp_subss(g_xmm0.f[0], g_xmm1.f[0]);      /* subss xmm0, xmm1 */
    }
    fp_store_ss(g_eax, &g_xmm0);
    g_eax = 0u;
}
