/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576: hand C for guarded-jump roots, proved with the opt-in guarded-jump-schema3 contract
 * (`tools.replace prove --guarded-jump-contract guarded-jump-schema3`, fp-scalar-v1 state).
 * The helper prefix below is byte-identical to the one in game_fp_step2b.c (T1631 fixed-destination
 * scalar helpers, raw 128-bit XMM TLS slots). Record: docs/evidence/t1576/wiring.md. */
#include <string.h>

#include "game_replace.h"

typedef union GameFpXmm {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} GameFpXmm;

/* The runtime (src/host/recomp_runtime.c, tools/harness/runtime_min.c) owns the strong slots.
 * The weak definitions only let registry-only links (game_manifest, test_game_replacements)
 * resolve the symbols, exactly as tools/replace/manifest_vector_state.c does. */
__thread GameFpXmm g_xmm0 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm1 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm2 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm3 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm4 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm5 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm6 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm7 __attribute__((weak, aligned(8)));

/* BEGIN GAME_FP_SCALAR_HELPERS (byte-identical in game_fp_list001.c and game_fp_list002.c,
 * pinned by tests/test_t1631_fp_nan_order.py).
 *
 * T1631 operand-order contract. x86 mulss/addss/subss with TWO NaN inputs return the FIRST
 * (destination) operand's payload, quieted. A plain C `a * b` lets the compiler commute the
 * operands (MEASURED: gcc -O0 for 0x00046280, 0x00073530 and 0x00151F20, gcc and clang -O3
 * for 0x00151F20), which changes the stored NaN payload. Every scalar op below therefore has a fixed destination:
 *   - x86-64 with SSE2 (the only host the fp-scalar-v1 proofs run on): one inline-asm
 *     instruction, `mulss %1, %0` with "+x" dest and "xm" source. The destination value is bound
 *     to the written register, so no compiler can commute it, and the MXCSR sticky flags are
 *     the ones the original instruction sets. comiss/ucomiss are asm too (flag outputs), so the
 *     invalid-operation flag follows the instruction (comiss signals on a QNaN, ucomiss does
 *     not) and not the compiler's choice for a C relational operator (gcc comiss, clang ucomiss).
 *   - any other host or -DGAME_FP_FORCE_PORTABLE: a portable model of the same x86 rule (first
 *     NaN operand wins and is quieted, an invalid operation with no NaN input gives the x86
 *     default NaN 0xFFC00000). It is exact for NaN payloads on every compiler because the NaN
 *     choice is made before the arithmetic and is independent of any commuting. It does not set
 *     MXCSR flags, honour FTZ/DAZ, or distinguish comiss from ucomiss (documented limits). */
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
/* addss/subss/mulss xmm_dest, xmm_or_m32_source: dest = dest OP source. */
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

GAME_FP_HELPER float fp_mulss(float dest, float source)
{
    __asm__ volatile("mulss %1, %0" : "+x"(dest) : "xm"(source));
    return dest;
}

/* comiss a, b ; ja : taken only for an ordered a > b (CF=0 and ZF=0). */
GAME_FP_HELPER int fp_comiss_above(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@cca"(taken) : "x"(a), "xm"(b));
    return taken;
}

/* comiss a, b ; jb : taken for a < b and for unordered (CF=1). */
GAME_FP_HELPER int fp_comiss_below(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccb"(taken) : "x"(a), "xm"(b));
    return taken;
}

/* ucomiss a, b ; lahf ; test ah, 0x44 ; jnp : the jump (ordered equal) means ZF=1 and PF=0. */
GAME_FP_HELPER int fp_ucomiss_equal(float a, float b)
{
    int zero_flag;
    int no_parity;
    __asm__ volatile("ucomiss %3, %2" : "=@ccz"(zero_flag), "=@ccnp"(no_parity) : "x"(a), "xm"(b));
    return zero_flag && no_parity;
}
#else
/* x86 two-NaN rule: first operand's NaN (quieted), else second's, else the default NaN for an
 * invalid operation, else the arithmetic result (commutative cases are exact either way). */
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

GAME_FP_HELPER float fp_mulss(float dest, float source)
{
    return fp_x86_ss_result(dest, source, dest * source);
}

GAME_FP_HELPER int fp_comiss_above(float a, float b)
{
    return a > b;
}

GAME_FP_HELPER int fp_comiss_below(float a, float b)
{
    return !(a >= b);
}

GAME_FP_HELPER int fp_ucomiss_equal(float a, float b)
{
    return a == b;
}
#endif
/* END GAME_FP_SCALAR_HELPERS */

/* T1634: full comiss/ucomiss condition set. The suffix is the x86 condition code read
 * from the flags the instruction sets (unordered gives ZF=PF=CF=1). */
#if GAME_FP_FIXED_DEST
GAME_FP_HELPER int fp_comiss_z(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccz"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_nz(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccnz"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_a(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@cca"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_ae(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccae"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_b(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccb"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_be(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccbe"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_p(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccp"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_comiss_np(float a, float b)
{
    int taken;
    __asm__ volatile("comiss %2, %1" : "=@ccnp"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_z(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccz"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_nz(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccnz"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_a(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@cca"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_ae(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccae"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_b(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccb"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_be(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccbe"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_p(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccp"(taken) : "x"(a), "xm"(b));
    return taken;
}

GAME_FP_HELPER int fp_ucomiss_np(float a, float b)
{
    int taken;
    __asm__ volatile("ucomiss %2, %1" : "=@ccnp"(taken) : "x"(a), "xm"(b));
    return taken;
}
/* divss dest, source: dest = dest / source. */
GAME_FP_HELPER float fp_divss(float dest, float source)
{
    __asm__ volatile("divss %1, %0" : "+x"(dest) : "xm"(source));
    return dest;
}

/* cvttss2si r32, xmm/m32: truncate, out of range or NaN gives 0x80000000 (integer indefinite). */
GAME_FP_HELPER int32_t fp_cvttss2si(float source)
{
    int32_t result;
    __asm__ volatile("cvttss2si %1, %0" : "=r"(result) : "xm"(source));
    return result;
}
#else
GAME_FP_HELPER int fp_comiss_z(float a, float b)
{
    return (a == b) || (a != a) || (b != b);
}

GAME_FP_HELPER int fp_comiss_nz(float a, float b)
{
    return (a < b) || (a > b);
}

GAME_FP_HELPER int fp_comiss_a(float a, float b)
{
    return a > b;
}

GAME_FP_HELPER int fp_comiss_ae(float a, float b)
{
    return a >= b;
}

GAME_FP_HELPER int fp_comiss_b(float a, float b)
{
    return !(a >= b);
}

GAME_FP_HELPER int fp_comiss_be(float a, float b)
{
    return !(a > b);
}

GAME_FP_HELPER int fp_comiss_p(float a, float b)
{
    return (a != a) || (b != b);
}

GAME_FP_HELPER int fp_comiss_np(float a, float b)
{
    return (a == a) && (b == b);
}

GAME_FP_HELPER int fp_ucomiss_z(float a, float b)
{
    return (a == b) || (a != a) || (b != b);
}

GAME_FP_HELPER int fp_ucomiss_nz(float a, float b)
{
    return (a < b) || (a > b);
}

GAME_FP_HELPER int fp_ucomiss_a(float a, float b)
{
    return a > b;
}

GAME_FP_HELPER int fp_ucomiss_ae(float a, float b)
{
    return a >= b;
}

GAME_FP_HELPER int fp_ucomiss_b(float a, float b)
{
    return !(a >= b);
}

GAME_FP_HELPER int fp_ucomiss_be(float a, float b)
{
    return !(a > b);
}

GAME_FP_HELPER int fp_ucomiss_p(float a, float b)
{
    return (a != a) || (b != b);
}

GAME_FP_HELPER int fp_ucomiss_np(float a, float b)
{
    return (a == a) && (b == b);
}
GAME_FP_HELPER float fp_divss(float dest, float source)
{
    return fp_x86_ss_result(dest, source, dest / source);
}

GAME_FP_HELPER int32_t fp_cvttss2si(float source)
{
    if (!(source > -2147483648.0f && source < 2147483648.0f)) return (int32_t)0x80000000u;
    return (int32_t)source;
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

/* movss dword ptr [address], xmm: lane 0 stored. */
GAME_FP_HELPER void fp_store_ss(uint32_t address, const GameFpXmm *reg)
{
    guest_write32(address, reg->u[0]);
}

/* xorps xmm, xmm: all 128 bits cleared. */
GAME_FP_HELPER void fp_zero(GameFpXmm *reg)
{
    reg->q[0] = 0u;
    reg->q[1] = 0u;
}

GAME_FP_HELPER void fp_addss_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->f[0] = fp_addss(dest->f[0], source->f[0]);
}

GAME_FP_HELPER void fp_mulss_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->f[0] = fp_mulss(dest->f[0], source->f[0]);
}

GAME_FP_HELPER void fp_subss_mem(GameFpXmm *dest, uint32_t address)
{
    dest->f[0] = fp_subss(dest->f[0], fp_guest_float(address));
}


/* part0 helpers: cvtsi2ss lane 0 (round-to-nearest, lanes 1..3 untouched), movaps reg,reg and a
 * 16-bit guest store (little endian, two byte writes, low byte first). */
GAME_FP_HELPER void fp_cvtsi2ss(GameFpXmm *dest, uint32_t value)
{
    dest->f[0] = (float)(int32_t)value;
}

GAME_FP_HELPER void fp_movaps_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->q[0] = source->q[0];
    dest->q[1] = source->q[1];
}

GAME_FP_HELPER void fp_write16(uint32_t address, uint32_t value)
{
    guest_write8(address, (uint8_t)(value & 0xFFu));
    guest_write8(address + 1u, (uint8_t)((value >> 8) & 0xFFu));
}

/* T1576 draft, never registered (append to the first 440 lines of src/game/game_fp_step2b.c).
 * 0x002777E0: cdecl(obj), same body as the T1741 draft, but the two-level original switch
 *   cmp eax,0x50 / ja 0x27780c / movzx eax,byte [eax+0x277840] / jmp [eax*4+0x277838]
 * is a C switch so the arm witness can observe it. Slot 0 (0x277804) loads [475D18] into xmm1
 * and FALLS THROUGH to 0x27780c; slot 1 and the JA default both target 0x27780c, so
 * `case 1: default:` are stacked. The selector is 0xFFFFFFFF when the unsigned bound fails, so
 * the default group is entered exactly when the original's JA is taken. */
GAME_REPLACE_EXACT(002777E0, cdecl, 1, u32, game_health_drain_per_frame_obj_1d8_scaled_by_mode)
{
    g_eax = guest_read32(0x0079094Cu) - 1u;
    fp_load_ss(&g_xmm0, 0x00475C78u);
    fp_movaps_reg(&g_xmm1, &g_xmm0);
    uint32_t slot = 0xFFFFFFFFu;
    if (g_eax <= 0x50u) {
        g_eax = guest_read8(0x00277840u + g_eax);
        slot = g_eax;
    }
    switch (slot) {
    case 0:
        fp_load_ss(&g_xmm1, 0x00475D18u);
        __attribute__((fallthrough));
    case 1:
    default:
        break;
    }
    g_xmm2.f[0] = (float)(int32_t)guest_read32(0x004E8CECu);
    g_ecx = game_stack_arg(0u);
    g_xmm0.f[0] = fp_divss(g_xmm0.f[0], g_xmm2.f[0]);
    fp_load_ss(&g_xmm2, g_ecx + 0x1D8u);
    g_xmm2.f[0] = fp_divss(g_xmm2.f[0], g_xmm1.f[0]);
    g_xmm0.f[0] = fp_mulss(g_xmm0.f[0], g_xmm2.f[0]);
    g_xmm0.f[0] = fp_mulss(g_xmm0.f[0], fp_guest_float(0x004E794Cu));
}
