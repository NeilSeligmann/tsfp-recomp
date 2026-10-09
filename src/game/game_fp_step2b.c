/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1634: hand C drafts for the 28 fp-scalar-v1 step 2b candidate roots (divss, cvttss2si and
 * register movaps, admitted by T1622). Only the 8 roots that passed batch_prove
 * (000D1E20 001543A0 00154470 00238D90 002DAF90 00309120 00341980 003441A0) stay registered
 * here. All 28 drafts as proved (wave 1 and wave 2, 20 rejected) are kept verbatim in
 * docs/data/t1634-fp-step2b/proved-wave{1,2}-14-drafts.c.txt. Conventions are those of
 * game_fp_list002.c: register-exact EAX/ECX/EDX/ESI, original memory order, raw 128-bit XMM
 * TLS slots (g_xmm0..g_xmm7), movss load zeroes lanes 1..3, ss arithmetic and cvtsi2ss write
 * lane 0 only, fixed-destination inline-asm helpers (T1631) for addss subss mulss divss
 * cvttss2si and every comiss/ucomiss condition. Record: docs/t-fp-draft-step2b.md. */
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

/* 0x000D1E20: cdecl(obj, f1, f3, f4). [obj+0xB4] = f1; d = f3 - [obj+0xAC]. If [0x79094C] is
 * not > 0x64 (signed) and (c1 [0x478620] > d or d > c2 [0x475D10]): [obj+0xB8] = [obj+0xAC] + c1
 * when c1 > d, then [obj+0xAC] + c2 when d > c2. Otherwise [obj+0xB8] = f3. [obj+0xBC] = f4. */
GAME_REPLACE_EXACT(000D1E20, cdecl, 4, u32, game_store_clamped_delta_target_b8_and_f4_bc)
{
    g_eax = game_stack_arg(0u);
    fp_load_ss(&g_xmm1, g_esp + 8u);
    fp_load_ss(&g_xmm3, g_esp + 0xCu);
    fp_store_ss(g_eax + 0xB4u, &g_xmm1);
    fp_movaps_reg(&g_xmm0, &g_xmm3);
    fp_subss_mem(&g_xmm0, g_eax + 0xACu);
    if ((int32_t)guest_read32(0x0079094Cu) <= 0x64) {
        fp_load_ss(&g_xmm1, 0x00478620u);
        fp_load_ss(&g_xmm2, 0x00475D10u);
        if (fp_comiss_a(g_xmm1.f[0], g_xmm0.f[0]) || !fp_comiss_be(g_xmm0.f[0], g_xmm2.f[0])) {
            if (!fp_comiss_be(g_xmm1.f[0], g_xmm0.f[0])) {
                fp_load_ss(&g_xmm3, g_eax + 0xACu);
                g_xmm3.f[0] = fp_addss(g_xmm3.f[0], g_xmm1.f[0]);
                fp_store_ss(g_eax + 0xB8u, &g_xmm3);
            }
            if (!fp_comiss_be(g_xmm0.f[0], g_xmm2.f[0])) {
                fp_load_ss(&g_xmm0, g_eax + 0xACu);
                g_xmm0.f[0] = fp_addss(g_xmm0.f[0], g_xmm2.f[0]);
                fp_store_ss(g_eax + 0xB8u, &g_xmm0);
            }
            fp_load_ss(&g_xmm0, g_esp + 0x10u);
            fp_store_ss(g_eax + 0xBCu, &g_xmm0);
            return;
        }
    }
    fp_store_ss(g_eax + 0xB8u, &g_xmm3);
    fp_load_ss(&g_xmm0, g_esp + 0x10u);
    fp_store_ss(g_eax + 0xBCu, &g_xmm0);
}



/* 0x00238D90: cdecl(float value, float *out). [out] = value / divisor, divisor = [0x49EF8C]
 * when [0x4C0454] == 0, else [0x475DB8]. */
GAME_REPLACE_EXACT(00238D90, cdecl, 2, u32, game_store_float_divided_by_49ef8c_or_475db8_by_flag_4c0454)
{
    g_ecx = guest_read32(0x004C0454u);
    fp_load_ss(&g_xmm0, 0x0049EF8Cu);
    g_eax = 0u;
    g_eax = (g_ecx != 0u) ? 1u : 0u;
    if (g_eax != 0u) {
        fp_load_ss(&g_xmm0, 0x00475DB8u);
    }
    fp_load_ss(&g_xmm1, g_esp + 4u);
    g_ecx = guest_read32(g_esp + 8u);
    g_xmm1.f[0] = fp_divss(g_xmm1.f[0], g_xmm0.f[0]);
    fp_store_ss(g_ecx, &g_xmm1);
}

/* ---- part3 (T1634): VAs from vas_03 plus 0x1543A0 0x154470 0x154890 0x1899B0 ---- */

GAME_FP_HELPER void p3_movaps(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->q[0] = source->q[0];
    dest->q[1] = source->q[1];
}

GAME_FP_HELPER void p3_addss_mem(GameFpXmm *dest, uint32_t address)
{
    dest->f[0] = fp_addss(dest->f[0], fp_guest_float(address));
}

GAME_FP_HELPER void p3_mulss_mem(GameFpXmm *dest, uint32_t address)
{
    dest->f[0] = fp_mulss(dest->f[0], fp_guest_float(address));
}

GAME_FP_HELPER void p3_divss_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->f[0] = fp_divss(dest->f[0], source->f[0]);
}

GAME_FP_HELPER void p3_subss_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->f[0] = fp_subss(dest->f[0], source->f[0]);
}

/* [esp+4+4*index] as a float memory operand / movss load (entry-esp relative). */
GAME_FP_HELPER float p3_arg_f(unsigned index)
{
    return fp_bits_to_float(game_stack_arg(index));
}

GAME_FP_HELPER void p3_load_arg(GameFpXmm *reg, unsigned index)
{
    fp_load_ss(reg, g_esp + 4u + 4u * index);
}

/* 0x002DAF90: cdecl(obj, dt). [obj+0x20] += [obj+0x24]*dt with latch/wrap logic. */
GAME_REPLACE_EXACT(002DAF90, cdecl, 2, u32, game_menu_background_advance_phase_and_wrap)
{
    int condition;
    g_eax = game_stack_arg(0u);
    fp_load_ss(&g_xmm1, g_eax + 0x24u);
    g_xmm1.f[0] = fp_mulss(g_xmm1.f[0], p3_arg_f(1u));
    p3_movaps(&g_xmm0, &g_xmm1);
    p3_addss_mem(&g_xmm0, g_eax + 0x20u);
    condition = fp_comiss_be(g_xmm0.f[0], fp_guest_float(0x00475CACu));
    fp_store_ss(g_eax + 0x20u, &g_xmm0);
    g_ecx = 1u;
    if (!condition) guest_write32(g_eax + 0x3Cu, g_ecx);
    fp_load_ss(&g_xmm2, 0x00475C84u);
    if (fp_comiss_be(g_xmm0.f[0], g_xmm2.f[0])) return;
    fp_addss_reg(&g_xmm1, &g_xmm2);
    if (fp_comiss_b(g_xmm1.f[0], g_xmm0.f[0])) {
        fp_load_ss(&g_xmm1, 0x00475C88u);
        if (fp_comiss_b(g_xmm0.f[0], g_xmm1.f[0])) return;
        p3_subss_reg(&g_xmm0, &g_xmm1);
        fp_store_ss(g_eax + 0x20u, &g_xmm0);
        return;
    }
    if (guest_read32(g_eax + 0x3Cu) != g_ecx) return;
    g_edx = guest_read32(g_eax + 0x38u);
    g_edx = g_edx + 1u;
    g_ecx = g_edx;
    guest_write32(g_eax + 0x38u, g_edx);
    if ((int32_t)g_ecx < 2) return;
    guest_write32(g_eax + 0x38u, 0u);
}

/* 0x00309120: cdecl(x, y). [0x783F2C]=x [0x783F28]=y [0x783DD4]=one/x [0x783F20]=one/y. */
GAME_REPLACE_EXACT(00309120, cdecl, 2, u32, game_set_scale_pair_and_reciprocals)
{
    fp_load_ss(&g_xmm0, 0x00475C78u);
    p3_load_arg(&g_xmm1, 0u);
    p3_load_arg(&g_xmm2, 1u);
    p3_movaps(&g_xmm3, &g_xmm0);
    p3_divss_reg(&g_xmm3, &g_xmm1);
    p3_divss_reg(&g_xmm0, &g_xmm2);
    fp_store_ss(0x00783F2Cu, &g_xmm1);
    fp_store_ss(0x00783F28u, &g_xmm2);
    fp_store_ss(0x00783DD4u, &g_xmm3);
    fp_store_ss(0x00783F20u, &g_xmm0);
}

/* 0x00341980: cdecl(slot, state, value, duration). */
GAME_REPLACE_EXACT(00341980, cdecl, 4, u32, game_weather_slot_set_state_with_fade_duration)
{
    int condition;
    g_eax = game_stack_arg(0u);
    g_ecx = guest_read32(g_eax + 0x78u);
    g_edx = game_stack_arg(1u);
    if (g_ecx == g_edx) return;
    p3_load_arg(&g_xmm0, 3u);
    condition = fp_comiss_be(g_xmm0.f[0], fp_guest_float(0x00475CACu));
    guest_write32(g_eax + 0x74u, g_ecx);
    g_ecx = guest_read32(g_eax + 0x80u);
    guest_write32(g_eax + 0x7Cu, g_ecx);
    g_ecx = game_stack_arg(2u);
    guest_write32(g_eax + 0x78u, g_edx);
    guest_write32(g_eax + 0x80u, g_ecx);
    if (condition) {
        fp_load_ss(&g_xmm0, 0x00475C78u);
        fp_store_ss(g_eax + 0x84u, &g_xmm0);
        guest_write32(g_eax + 0x74u, g_edx);
        guest_write32(g_eax + 0x7Cu, g_ecx);
        fp_store_ss(g_eax + 0x88u, &g_xmm0);
        return;
    }
    fp_load_ss(&g_xmm1, g_eax + 0x88u);
    fp_load_ss(&g_xmm2, 0x00475C78u);
    fp_subss_mem(&g_xmm2, g_eax + 0x84u);
    p3_divss_reg(&g_xmm1, &g_xmm0);
    fp_mulss_reg(&g_xmm1, &g_xmm2);
    fp_store_ss(g_eax + 0x84u, &g_xmm1);
    fp_store_ss(g_eax + 0x88u, &g_xmm0);
}

/* 0x003441A0: cdecl(obj, target_vec3). Moves obj+0x20 to target and shifts the dependent
 * points (+0x14, +0x2C, +0x38 sets) by the same delta. */
GAME_REPLACE_EXACT(003441A0, cdecl, 2, u32, game_translate_object_anchor_and_dependent_points)
{
    g_eax = game_stack_arg(0u);
    g_ecx = game_stack_arg(1u);
    fp_load_ss(&g_xmm0, g_ecx);
    fp_load_ss(&g_xmm1, g_ecx + 4u);
    fp_load_ss(&g_xmm2, g_ecx + 8u);
    fp_subss_mem(&g_xmm0, g_eax + 0x20u);
    fp_subss_mem(&g_xmm1, g_eax + 0x24u);
    fp_subss_mem(&g_xmm2, g_eax + 0x28u);
    p3_movaps(&g_xmm3, &g_xmm0);
    p3_addss_mem(&g_xmm3, g_eax + 0x20u);
    fp_store_ss(g_eax + 0x20u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x24u);
    fp_addss_reg(&g_xmm3, &g_xmm1);
    fp_store_ss(g_eax + 0x24u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x28u);
    fp_addss_reg(&g_xmm3, &g_xmm2);
    fp_store_ss(g_eax + 0x28u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x14u);
    fp_addss_reg(&g_xmm3, &g_xmm0);
    fp_store_ss(g_eax + 0x14u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x18u);
    fp_addss_reg(&g_xmm3, &g_xmm1);
    fp_store_ss(g_eax + 0x18u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x1Cu);
    fp_addss_reg(&g_xmm3, &g_xmm2);
    fp_store_ss(g_eax + 0x1Cu, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x2Cu);
    fp_addss_reg(&g_xmm3, &g_xmm0);
    fp_store_ss(g_eax + 0x2Cu, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x30u);
    fp_addss_reg(&g_xmm3, &g_xmm1);
    fp_store_ss(g_eax + 0x30u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x34u);
    fp_addss_reg(&g_xmm3, &g_xmm2);
    fp_store_ss(g_eax + 0x34u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x38u);
    fp_addss_reg(&g_xmm3, &g_xmm0);
    fp_load_ss(&g_xmm0, g_eax + 0x3Cu);
    fp_addss_reg(&g_xmm0, &g_xmm1);
    fp_store_ss(g_eax + 0x3Cu, &g_xmm0);
    fp_load_ss(&g_xmm0, g_eax + 0x40u);
    fp_addss_reg(&g_xmm0, &g_xmm2);
    fp_store_ss(g_eax + 0x38u, &g_xmm3);
    fp_store_ss(g_eax + 0x40u, &g_xmm0);
}

/* 0x001543A0: cdecl(out, t, p0, p1, p2, p3). Cubic bezier blend for 2 floats. ESI/EDI are
 * push/pop saved (locals). */
GAME_REPLACE_EXACT(001543A0, cdecl, 6, u32, game_ui_curved_menu_bezier_sample_point)
{
    uint32_t edi;
    uint32_t esi;
    p3_load_arg(&g_xmm2, 1u);
    fp_load_ss(&g_xmm0, 0x00475C78u);
    g_eax = game_stack_arg(3u);
    g_ecx = game_stack_arg(4u);
    g_edx = game_stack_arg(2u);
    p3_subss_reg(&g_xmm0, &g_xmm2);
    p3_movaps(&g_xmm3, &g_xmm2);
    fp_mulss_reg(&g_xmm3, &g_xmm2);
    p3_movaps(&g_xmm1, &g_xmm0);
    fp_mulss_reg(&g_xmm1, &g_xmm0);
    p3_movaps(&g_xmm4, &g_xmm3);
    fp_mulss_reg(&g_xmm4, &g_xmm2);
    p3_movaps(&g_xmm5, &g_xmm1);
    fp_mulss_reg(&g_xmm1, &g_xmm2);
    fp_load_ss(&g_xmm2, 0x00475D18u);
    fp_mulss_reg(&g_xmm5, &g_xmm0);
    fp_mulss_reg(&g_xmm0, &g_xmm3);
    fp_load_ss(&g_xmm3, g_ecx);
    fp_mulss_reg(&g_xmm0, &g_xmm2);
    fp_mulss_reg(&g_xmm1, &g_xmm2);
    fp_load_ss(&g_xmm2, g_eax);
    fp_mulss_reg(&g_xmm3, &g_xmm0);
    fp_mulss_reg(&g_xmm2, &g_xmm1);
    fp_addss_reg(&g_xmm2, &g_xmm3);
    fp_load_ss(&g_xmm3, g_edx);
    esi = game_stack_arg(5u);
    fp_mulss_reg(&g_xmm3, &g_xmm5);
    fp_addss_reg(&g_xmm2, &g_xmm3);
    fp_load_ss(&g_xmm3, esi);
    edi = game_stack_arg(0u);
    fp_mulss_reg(&g_xmm3, &g_xmm4);
    fp_addss_reg(&g_xmm2, &g_xmm3);
    fp_store_ss(edi, &g_xmm2);
    fp_load_ss(&g_xmm2, g_ecx + 4u);
    fp_mulss_reg(&g_xmm2, &g_xmm0);
    fp_load_ss(&g_xmm0, g_eax + 4u);
    fp_mulss_reg(&g_xmm0, &g_xmm1);
    fp_addss_reg(&g_xmm2, &g_xmm0);
    fp_load_ss(&g_xmm0, g_edx + 4u);
    fp_mulss_reg(&g_xmm0, &g_xmm5);
    fp_addss_reg(&g_xmm2, &g_xmm0);
    fp_load_ss(&g_xmm0, esi + 4u);
    fp_mulss_reg(&g_xmm0, &g_xmm4);
    fp_addss_reg(&g_xmm2, &g_xmm0);
    fp_store_ss(edi + 4u, &g_xmm2);
}

/* 0x00154470: cdecl(out, t, p0, p1, p2, p3). Same blend as 0x1543A0 for 3 floats. */
GAME_REPLACE_EXACT(00154470, cdecl, 6, u32, game_bezier_sample_point_vec3)
{
    uint32_t edi;
    uint32_t esi;
    p3_load_arg(&g_xmm4, 1u);
    fp_load_ss(&g_xmm0, 0x00475C78u);
    g_eax = game_stack_arg(3u);
    g_ecx = game_stack_arg(4u);
    p3_subss_reg(&g_xmm0, &g_xmm4);
    g_edx = game_stack_arg(2u);
    p3_movaps(&g_xmm5, &g_xmm4);
    fp_mulss_reg(&g_xmm5, &g_xmm4);
    p3_movaps(&g_xmm2, &g_xmm5);
    fp_mulss_reg(&g_xmm2, &g_xmm4);
    p3_movaps(&g_xmm1, &g_xmm0);
    fp_mulss_reg(&g_xmm1, &g_xmm0);
    p3_movaps(&g_xmm3, &g_xmm1);
    fp_mulss_reg(&g_xmm1, &g_xmm4);
    fp_load_ss(&g_xmm4, 0x00475D18u);
    fp_mulss_reg(&g_xmm1, &g_xmm4);
    fp_mulss_reg(&g_xmm3, &g_xmm0);
    fp_mulss_reg(&g_xmm0, &g_xmm5);
    fp_load_ss(&g_xmm5, g_ecx);
    fp_mulss_reg(&g_xmm0, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax);
    fp_mulss_reg(&g_xmm5, &g_xmm0);
    fp_mulss_reg(&g_xmm4, &g_xmm1);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    fp_load_ss(&g_xmm5, g_edx);
    fp_mulss_reg(&g_xmm5, &g_xmm3);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    esi = game_stack_arg(5u);
    fp_load_ss(&g_xmm5, esi);
    fp_mulss_reg(&g_xmm5, &g_xmm2);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    edi = game_stack_arg(0u);
    fp_store_ss(edi, &g_xmm4);
    fp_load_ss(&g_xmm4, g_ecx + 4u);
    fp_load_ss(&g_xmm5, g_eax + 4u);
    fp_mulss_reg(&g_xmm4, &g_xmm0);
    fp_mulss_reg(&g_xmm5, &g_xmm1);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    fp_load_ss(&g_xmm5, g_edx + 4u);
    fp_mulss_reg(&g_xmm5, &g_xmm3);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    fp_load_ss(&g_xmm5, esi + 4u);
    fp_mulss_reg(&g_xmm5, &g_xmm2);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    fp_store_ss(edi + 4u, &g_xmm4);
    fp_load_ss(&g_xmm4, g_ecx + 8u);
    fp_mulss_reg(&g_xmm4, &g_xmm0);
    fp_load_ss(&g_xmm0, g_eax + 8u);
    fp_mulss_reg(&g_xmm0, &g_xmm1);
    fp_addss_reg(&g_xmm4, &g_xmm0);
    fp_load_ss(&g_xmm0, g_edx + 8u);
    fp_mulss_reg(&g_xmm0, &g_xmm3);
    fp_addss_reg(&g_xmm4, &g_xmm0);
    fp_load_ss(&g_xmm0, esi + 8u);
    fp_mulss_reg(&g_xmm0, &g_xmm2);
    fp_addss_reg(&g_xmm4, &g_xmm0);
    fp_store_ss(edi + 8u, &g_xmm4);
}
