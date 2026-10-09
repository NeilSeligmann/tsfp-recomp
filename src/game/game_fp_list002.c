/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1623 half 2: hand C drafts for the last 16 (ascending VA) of the 32 fp-scalar-v1 step 1
 * candidates of docs/evidence/t1620/unlock.json. Only the 3 roots that passed the batch gate
 * (00120220, 00151F20, 00347FA0) stay registered in this file. The exact file proved with all
 * 16 drafts (13 rejected on the fp_scalar_gate, kept as drafts only) is kept verbatim in
 * docs/data/t1623-fp-list002/proved-all-16-drafts.c.txt. Drafted from the retail disassembly
 * of the pinned XBE (sha256 3cfd001a...). Every body is register-exact: EAX/ECX/EDX are
 * written exactly where the original writes them, memory is read and written in the original
 * order, and each XMM register is a raw 128-bit TLS slot (g_xmm0..g_xmm4):
 *   movss xmm, m32   writes lane 0 and ZEROES lanes 1..3
 *   xorps xmm, xmm   clears all 128 bits
 *   addss/subss/mulss and cvtsi2ss write lane 0 only, lanes 1..3 stay as they were.
 * Float arithmetic is single precision with the original operand order (destination is the
 * left operand), no fused multiply-add and no x87 (built with -ffp-contract=off -fno-fast-math,
 * checked by tools/replace/fp_build.py). T1631: addss/subss/mulss and comiss/ucomiss go through
 * the fixed-destination helpers of the GAME_FP_SCALAR_HELPERS block (inline asm on x86-64), so
 * the compiler cannot commute operands (two-NaN payload order) and the MXCSR flags are the
 * original instruction's. comiss/ucomiss flag mapping used by
 * the branch conditions (unordered sets ZF=PF=CF=1):
 *   ja  = a >  b          jbe = !(a >  b)
 *   jae = a >= b          jb  = !(a >= b)
 *   ucomiss + lahf + test ah,0x44 + jp falls through only for an ordered equal (a == b).
 * Names describe memory effects only. Record: docs/t-fp-draft-list002.md, operand-order fix
 * docs/t1631-fp-nan-order.md. */
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

static float fp_guest_float(uint32_t address)
{
    return fp_bits_to_float(guest_read32(address));
}

/* movss xmm, dword ptr [address]: lane 0 loaded, lanes 1..3 zeroed. */
static void fp_load_ss(GameFpXmm *reg, uint32_t address)
{
    reg->u[0] = guest_read32(address);
    reg->u[1] = 0u;
    reg->u[2] = 0u;
    reg->u[3] = 0u;
}

/* movss dword ptr [address], xmm: lane 0 stored. */
static void fp_store_ss(uint32_t address, const GameFpXmm *reg)
{
    guest_write32(address, reg->u[0]);
}

/* xorps xmm, xmm: all 128 bits cleared. */
static void fp_zero(GameFpXmm *reg)
{
    reg->q[0] = 0u;
    reg->q[1] = 0u;
}

static void fp_addss_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->f[0] = fp_addss(dest->f[0], source->f[0]);
}

static void fp_mulss_reg(GameFpXmm *dest, const GameFpXmm *source)
{
    dest->f[0] = fp_mulss(dest->f[0], source->f[0]);
}

static void fp_subss_mem(GameFpXmm *dest, uint32_t address)
{
    dest->f[0] = fp_subss(dest->f[0], fp_guest_float(address));
}

/* 0x00120220: cdecl(object, float a, float b). Unless flag 0x200 of [object+4] is set and
 * [object+0xA0] > a (ordered): [object+0xA0] = a, [object+0xA4] = b, [object+4] |= 0x200,
 * [object+0x9C] = 0. */
GAME_REPLACE_EXACT(00120220, cdecl, 3, u32, game_store_float_pair_a0_a4_clear_9c_set_flag_200)
{
    g_eax = game_stack_arg(0u);
    g_ecx = guest_read32(g_eax + 4u);
    fp_load_ss(&g_xmm0, g_esp + 8u);
    if ((g_ecx & 0x200u) != 0u) {
        fp_load_ss(&g_xmm1, g_eax + 0xA0u);
        if (fp_comiss_above(g_xmm1.f[0], g_xmm0.f[0])) {
            return;
        }
    }
    fp_store_ss(g_eax + 0xA0u, &g_xmm0);
    fp_load_ss(&g_xmm0, g_esp + 0xCu);
    g_ecx |= 0x200u;
    fp_store_ss(g_eax + 0xA4u, &g_xmm0);
    fp_zero(&g_xmm0);
    guest_write32(g_eax + 4u, g_ecx);
    fp_store_ss(g_eax + 0x9Cu, &g_xmm0);
}

/* 0x00151F20: cdecl(matrix, vector). Writes the 3x3 matrix transform of (v - [m+0x30..0x38])
 * back over the vector: out0 = m8*d2 + m4*d1 + m0*d0 (d = v - translation), out1 uses
 * m18,m14,m10, out2 m28,m24,m20. Multiplications and additions keep the original order. */
GAME_REPLACE_EXACT(00151F20, cdecl, 2, u32, game_transform_vector_by_matrix_30_38)
{
    g_eax = game_stack_arg(0u);
    g_ecx = game_stack_arg(1u);
    fp_load_ss(&g_xmm3, g_eax + 8u);
    fp_load_ss(&g_xmm4, g_eax + 4u);
    fp_load_ss(&g_xmm1, g_ecx + 4u);
    fp_subss_mem(&g_xmm1, g_eax + 0x34u);
    fp_load_ss(&g_xmm2, g_ecx + 8u);
    fp_subss_mem(&g_xmm2, g_eax + 0x38u);
    fp_load_ss(&g_xmm0, g_ecx);
    fp_subss_mem(&g_xmm0, g_eax + 0x30u);
    fp_mulss_reg(&g_xmm3, &g_xmm2);
    fp_mulss_reg(&g_xmm4, &g_xmm1);
    fp_addss_reg(&g_xmm3, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax);
    fp_mulss_reg(&g_xmm4, &g_xmm0);
    fp_addss_reg(&g_xmm3, &g_xmm4);
    fp_store_ss(g_ecx, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x18u);
    fp_load_ss(&g_xmm4, g_eax + 0x14u);
    fp_mulss_reg(&g_xmm3, &g_xmm2);
    fp_mulss_reg(&g_xmm4, &g_xmm1);
    fp_addss_reg(&g_xmm3, &g_xmm4);
    fp_load_ss(&g_xmm4, g_eax + 0x10u);
    fp_mulss_reg(&g_xmm4, &g_xmm0);
    fp_addss_reg(&g_xmm3, &g_xmm4);
    fp_store_ss(g_ecx + 4u, &g_xmm3);
    fp_load_ss(&g_xmm3, g_eax + 0x28u);
    fp_mulss_reg(&g_xmm3, &g_xmm2);
    fp_load_ss(&g_xmm2, g_eax + 0x24u);
    fp_mulss_reg(&g_xmm2, &g_xmm1);
    fp_load_ss(&g_xmm1, g_eax + 0x20u);
    fp_addss_reg(&g_xmm3, &g_xmm2);
    fp_mulss_reg(&g_xmm1, &g_xmm0);
    fp_addss_reg(&g_xmm3, &g_xmm1);
    fp_store_ss(g_ecx + 8u, &g_xmm3);
}

/* 0x00347FA0: cdecl(float x). [0x7670FC] = clamp(x, 0, [0x475C78]): the limit when x > limit,
 * 0 when 0 > x, otherwise x (a NaN is stored unchanged). */
GAME_REPLACE_EXACT(00347FA0, cdecl, 1, u32, game_store_float_clamped_0_to_475c78_to_7670fc)
{
    fp_load_ss(&g_xmm0, g_esp + 4u);
    fp_load_ss(&g_xmm1, 0x00475C78u);
    if (!fp_comiss_above(g_xmm0.f[0], g_xmm1.f[0])) {
        fp_zero(&g_xmm2);
        if (fp_comiss_above(g_xmm2.f[0], g_xmm0.f[0])) {
            fp_store_ss(0x007670FCu, &g_xmm2);
            return;
        }
        if (!fp_comiss_above(g_xmm0.f[0], g_xmm1.f[0])) {
            fp_store_ss(0x007670FCu, &g_xmm0);
            return;
        }
    }
    fp_store_ss(0x007670FCu, &g_xmm1);
}
