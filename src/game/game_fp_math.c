/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1672: hand C drafts for pure scalar-SSE engine/math roots (no x87, no calls, no packed
 * ops): 00152120 00150580 00151190. Only the
 * roots that passed batch_prove stay registered here, the file as proved is kept verbatim in
 * docs/data/t1672-engine-math/. Conventions are those of game_fp_step2b.c (register-exact
 * EAX/ECX/EDX, original memory order, raw 128-bit XMM TLS slots, movss load zeroes lanes 1..3,
 * ss arithmetic writes lane 0 only, fixed-destination inline-asm helpers). Record:
 * docs/t-engine-math.md. */
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

/* ---- T1672 engine/math bodies ---- */

/* xmm_dest = [load] ; xmm_dest *= [mul] (movss load then mulss with a memory source). */
GAME_FP_HELPER void m1_load_mul(GameFpXmm *reg, uint32_t load, uint32_t mul)
{
    fp_load_ss(reg, load);
    p3_mulss_mem(reg, mul);
}

/* Shared tail of 0x00152090 and 0x00152120: direction transform of the vec3 at ecx by the 3x3
 * rotation part of the matrix at eax (row 0/1/2 at +0x00/+0x10/+0x20, no translation), written
 * in place over the vec3 (x last-computed, stored first). */
GAME_FP_HELPER void m1_direction_tail(void)
{
    fp_load_ss(&g_xmm3, g_ecx + 4u);
    fp_load_ss(&g_xmm4, g_ecx + 8u);
    fp_load_ss(&g_xmm0, g_eax + 0x24u);
    fp_load_ss(&g_xmm1, g_eax + 0x14u);
    fp_load_ss(&g_xmm2, g_ecx);
    fp_load_ss(&g_xmm5, g_eax + 0x18u);
    fp_mulss_reg(&g_xmm1, &g_xmm3);
    fp_mulss_reg(&g_xmm0, &g_xmm4);
    fp_addss_reg(&g_xmm0, &g_xmm1);
    fp_load_ss(&g_xmm1, g_eax + 4u);
    fp_mulss_reg(&g_xmm5, &g_xmm3);
    fp_mulss_reg(&g_xmm1, &g_xmm2);
    fp_addss_reg(&g_xmm0, &g_xmm1);
    fp_load_ss(&g_xmm1, g_eax + 0x28u);
    fp_mulss_reg(&g_xmm1, &g_xmm4);
    fp_addss_reg(&g_xmm1, &g_xmm5);
    fp_load_ss(&g_xmm5, g_eax + 8u);
    fp_mulss_reg(&g_xmm5, &g_xmm2);
    fp_addss_reg(&g_xmm1, &g_xmm5);
    fp_load_ss(&g_xmm5, g_eax + 0x20u);
    fp_mulss_reg(&g_xmm5, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax + 0x10u);
    fp_mulss_reg(&g_xmm4, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax);
    fp_addss_reg(&g_xmm5, &g_xmm4);
    fp_mulss_reg(&g_xmm3, &g_xmm2);
    fp_addss_reg(&g_xmm5, &g_xmm3);
    fp_store_ss(g_ecx, &g_xmm5);
    fp_store_ss(g_ecx + 4u, &g_xmm0);
    fp_store_ss(g_ecx + 8u, &g_xmm1);
}

/* 0x00152120: cdecl(matrix, src4, dest4). Copies the four dwords at src4 to dest4 through ESI
 * (saved and restored, so not modelled), then runs the 0x00152090 transform on dest4. EDX ends
 * as dest4, EAX as the matrix. */
GAME_REPLACE_EXACT(00152120, cdecl, 3, u32, game_matrix4x4_transform_direction_vec4_copy_to_out_scalar)
{
    uint32_t value;

    g_ecx = game_stack_arg(2u);
    g_eax = game_stack_arg(1u);
    value = guest_read32(g_eax);
    g_edx = g_ecx;
    guest_write32(g_edx, value);
    value = guest_read32(g_eax + 4u);
    guest_write32(g_edx + 4u, value);
    value = guest_read32(g_eax + 8u);
    guest_write32(g_edx + 8u, value);
    g_eax = guest_read32(g_eax + 0xCu);
    guest_write32(g_edx + 0xCu, g_eax);
    g_eax = game_stack_arg(0u);
    m1_direction_tail();
}


/* 0x00150580: cdecl(a, b, out). Quaternion product of the xyzw quads at a and b into out
 * (three-component cross/dot form, w computed from partial sums), original association. */
GAME_REPLACE_EXACT(00150580, cdecl, 3, u32, game_quat_multiply_to_out_scalar)
{
    g_eax = game_stack_arg(0u);
    g_ecx = game_stack_arg(1u);
    m1_load_mul(&g_xmm0, g_ecx, g_eax + 0xCu);
    m1_load_mul(&g_xmm1, g_eax + 8u, g_ecx + 4u);
    m1_load_mul(&g_xmm2, g_eax + 0xCu, g_ecx + 4u);
    m1_load_mul(&g_xmm3, g_ecx, g_eax + 4u);
    m1_load_mul(&g_xmm4, g_eax + 8u, g_ecx + 8u);
    p3_subss_reg(&g_xmm0, &g_xmm1);
    m1_load_mul(&g_xmm1, g_ecx + 8u, g_eax + 4u);
    fp_addss_reg(&g_xmm0, &g_xmm1);
    m1_load_mul(&g_xmm1, g_eax, g_ecx + 8u);
    p3_subss_reg(&g_xmm2, &g_xmm1);
    m1_load_mul(&g_xmm1, g_eax + 8u, g_ecx);
    fp_addss_reg(&g_xmm2, &g_xmm1);
    m1_load_mul(&g_xmm1, g_ecx + 8u, g_eax + 0xCu);
    p3_subss_reg(&g_xmm1, &g_xmm3);
    m1_load_mul(&g_xmm3, g_eax, g_ecx + 4u);
    fp_addss_reg(&g_xmm1, &g_xmm3);
    m1_load_mul(&g_xmm3, g_eax, g_ecx);
    fp_addss_reg(&g_xmm3, &g_xmm4);
    m1_load_mul(&g_xmm4, g_eax + 4u, g_ecx + 4u);
    g_ecx = game_stack_arg(2u);
    fp_addss_reg(&g_xmm3, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax);
    fp_mulss_reg(&g_xmm4, &g_xmm3);
    p3_movaps(&g_xmm5, &g_xmm1);
    p3_mulss_mem(&g_xmm5, g_eax + 4u);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    p3_movaps(&g_xmm5, &g_xmm0);
    p3_mulss_mem(&g_xmm5, g_eax + 0xCu);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    fp_load_ss(&g_xmm5, g_eax + 8u);
    fp_mulss_reg(&g_xmm5, &g_xmm2);
    p3_subss_reg(&g_xmm4, &g_xmm5);
    fp_store_ss(g_ecx, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax + 8u);
    fp_mulss_reg(&g_xmm4, &g_xmm0);
    p3_movaps(&g_xmm5, &g_xmm3);
    p3_mulss_mem(&g_xmm5, g_eax + 4u);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    p3_movaps(&g_xmm5, &g_xmm2);
    p3_mulss_mem(&g_xmm5, g_eax + 0xCu);
    fp_addss_reg(&g_xmm4, &g_xmm5);
    fp_load_ss(&g_xmm5, g_eax);
    fp_mulss_reg(&g_xmm5, &g_xmm1);
    p3_subss_reg(&g_xmm4, &g_xmm5);
    fp_store_ss(g_ecx + 4u, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax + 8u);
    p3_mulss_mem(&g_xmm1, g_eax + 0xCu);
    p3_mulss_mem(&g_xmm0, g_eax + 4u);
    fp_mulss_reg(&g_xmm4, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax);
    fp_mulss_reg(&g_xmm3, &g_xmm2);
    fp_addss_reg(&g_xmm4, &g_xmm3);
    fp_addss_reg(&g_xmm4, &g_xmm1);
    p3_subss_reg(&g_xmm4, &g_xmm0);
    fp_store_ss(g_ecx + 8u, &g_xmm4);
}

/* comiss a, 0 ; jbe fail ; value 1 else 0 (one edge-function sign). */
GAME_FP_HELPER uint32_t m2_positive(const GameFpXmm *value, const GameFpXmm *zero)
{
    return fp_comiss_be(value->f[0], zero->f[0]) ? 0u : 1u;
}

/* 0x00151190: cdecl(f0..f9) -> EAX. Four 2D edge-function sign tests of a point (f0,f1) against
 * the edges built from f2..f9, packed as bits 0..3 of the result (bit n set when strictly
 * positive, so unordered gives 0). Locals are XMM TLS slots 0..7 like the original. */
GAME_REPLACE_EXACT(00151190, cdecl, 10, u32, game_math_point_in_triangle_2d_scalar)
{
    p3_load_arg(&g_xmm2, 8u);
    p3_load_arg(&g_xmm0, 1u);
    p3_load_arg(&g_xmm3, 9u);
    p3_load_arg(&g_xmm4, 2u);
    p3_load_arg(&g_xmm6, 3u);
    p3_subss_reg(&g_xmm4, &g_xmm2);
    p3_movaps(&g_xmm1, &g_xmm0);
    p3_subss_reg(&g_xmm1, &g_xmm3);
    fp_mulss_reg(&g_xmm4, &g_xmm1);
    p3_load_arg(&g_xmm1, 0u);
    p3_movaps(&g_xmm5, &g_xmm1);
    p3_subss_reg(&g_xmm6, &g_xmm3);
    p3_subss_reg(&g_xmm5, &g_xmm2);
    fp_mulss_reg(&g_xmm5, &g_xmm6);
    fp_zero(&g_xmm6);
    p3_subss_reg(&g_xmm4, &g_xmm5);
    g_ecx = m2_positive(&g_xmm4, &g_xmm6);
    p3_load_arg(&g_xmm4, 7u);
    p3_load_arg(&g_xmm5, 6u);
    p3_subss_reg(&g_xmm2, &g_xmm5);
    p3_movaps(&g_xmm7, &g_xmm0);
    p3_subss_reg(&g_xmm7, &g_xmm4);
    fp_mulss_reg(&g_xmm7, &g_xmm2);
    p3_movaps(&g_xmm2, &g_xmm1);
    p3_subss_reg(&g_xmm2, &g_xmm5);
    p3_subss_reg(&g_xmm3, &g_xmm4);
    fp_mulss_reg(&g_xmm2, &g_xmm3);
    p3_subss_reg(&g_xmm7, &g_xmm2);
    g_eax = m2_positive(&g_xmm7, &g_xmm6);
    p3_load_arg(&g_xmm2, 5u);
    p3_load_arg(&g_xmm3, 4u);
    p3_subss_reg(&g_xmm5, &g_xmm3);
    p3_movaps(&g_xmm7, &g_xmm0);
    p3_subss_reg(&g_xmm7, &g_xmm2);
    fp_mulss_reg(&g_xmm7, &g_xmm5);
    g_eax = g_eax + g_eax;
    p3_movaps(&g_xmm5, &g_xmm1);
    p3_subss_reg(&g_xmm5, &g_xmm3);
    p3_subss_reg(&g_xmm4, &g_xmm2);
    fp_mulss_reg(&g_xmm5, &g_xmm4);
    g_ecx |= g_eax;
    p3_subss_reg(&g_xmm7, &g_xmm5);
    g_eax = m2_positive(&g_xmm7, &g_xmm6);
    p3_load_arg(&g_xmm5, 3u);
    p3_load_arg(&g_xmm4, 2u);
    p3_subss_reg(&g_xmm0, &g_xmm5);
    g_edx = g_eax * 4u;
    p3_subss_reg(&g_xmm3, &g_xmm4);
    p3_subss_reg(&g_xmm1, &g_xmm4);
    p3_subss_reg(&g_xmm2, &g_xmm5);
    fp_mulss_reg(&g_xmm0, &g_xmm3);
    fp_mulss_reg(&g_xmm1, &g_xmm2);
    g_ecx |= g_edx;
    p3_subss_reg(&g_xmm0, &g_xmm1);
    g_eax = m2_positive(&g_xmm0, &g_xmm6);
    g_eax = (g_eax << 3) | g_ecx;
}
