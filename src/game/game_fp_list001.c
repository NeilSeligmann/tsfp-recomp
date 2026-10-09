/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1623 half 1: hand C for the 7 roots that were ADMITTED by `batch_prove --live-vector-state
 * --vector-mode fp-scalar-v1` out of the first 16 (ascending VA) of the 32 fp-scalar-v1 step 1
 * candidates of docs/evidence/t1620/unlock.json. The 9 rejected drafts (0x00012860, 0x00012A90,
 * 0x00047560, 0x00071650, 0x00073750, 0x0007B3D0, 0x00087450, 0x000A2B50, 0x000C6150) are kept
 * verbatim, together with these 7 bodies exactly as proved, in
 * docs/data/t1623-fp-list001/proved-all-16-drafts.c.txt. Drafted from the retail disassembly of
 * the pinned XBE (sha256 3cfd001a...). Every body is register-exact: all eight GPRs are
 * reproduced, memory is read and written in the original order, and XMM0/XMM1 are modelled
 * through the runtime's raw 128-bit TLS slots. `movss xmm, m32` zeroes bits 32..127,
 * `addss`/`subss`/`mulss` touch only the low lane, `xorps` clears all 128 bits.
 *
 * Float semantics: every addss/subss/mulss and comiss/ucomiss goes through the fixed-destination
 * helpers of the GAME_FP_SCALAR_HELPERS block (T1631): one inline-asm SSE instruction in the
 * original operand order on x86-64, so no compiler can commute operands (x86 returns the FIRST
 * operand's NaN payload when both are NaN) and the MXCSR flags are the original's. Elsewhere a
 * portable model of the same rule is used. Single precision, no fused multiply-add, no x87 (the
 * harness compiles with -ffp-contract=off -fno-fast-math and scans the disassembly).
 * comiss/ucomiss flag mapping used below (unordered sets ZF PF CF):
 *   ja  (CF=0 and ZF=0)  <=>  a > b            (false for NaN)
 *   jbe (CF=1 or ZF=1)   <=>  !(a > b)         (true for NaN)
 *   jb  (CF=1)           <=>  !(a >= b)        (true for NaN)
 * `ucomiss; lahf; test ah,0x44; jnp` (0x0005A150) returns 0 only for an ordered equal pair.
 * Record: docs/t-fp-draft-list001.md, operand-order fix docs/t1631-fp-nan-order.md. */
#include <string.h>

#include "game_replace.h"

typedef union GameFpXmm {
    float f[4];
    uint32_t u[4];
    uint64_t q[2];
} GameFpXmm;

/* The runtime (src/host/recomp_runtime.c, tools/harness/runtime_min.c) owns the strong slots.
 * These weak definitions only let registry-only links resolve the symbols. */
__thread GameFpXmm g_xmm0 __attribute__((weak, aligned(8)));
__thread GameFpXmm g_xmm1 __attribute__((weak, aligned(8)));

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

static float fp_mem(uint32_t address)
{
    return fp_bits_to_float(guest_read32(address));
}

/* movss xmm, dword ptr [address]: low lane loaded, lanes 1..3 zeroed. */
static void movss_load(GameFpXmm *x, uint32_t address)
{
    x->u[0] = guest_read32(address);
    x->u[1] = 0u;
    x->u[2] = 0u;
    x->u[3] = 0u;
}

/* xorps xmm, xmm. */
static void xorps_clear(GameFpXmm *x)
{
    x->q[0] = 0u;
    x->q[1] = 0u;
}

static void addss_mem(GameFpXmm *x, uint32_t address)
{
    x->f[0] = fp_addss(x->f[0], fp_mem(address));
}

static void subss_mem(GameFpXmm *x, uint32_t address)
{
    x->f[0] = fp_subss(x->f[0], fp_mem(address));
}

static void mulss_mem(GameFpXmm *x, uint32_t address)
{
    x->f[0] = fp_mulss(x->f[0], fp_mem(address));
}

/* comiss a, b then `ja`: taken only for an ordered a > b. */
static int comiss_above(const GameFpXmm *a, float b)
{
    return fp_comiss_above(a->f[0], b);
}

/* 0x00046280: cdecl(value). xmm0 = [0x4D1848] * [0x4D1840]; [0x4D184C] = [0x4D1850] = value;
 * [0x4D183C] = xmm0. EAX = value. */
GAME_REPLACE_EXACT(00046280, cdecl, 1, u32, game_store_arg_to_4d184c_4d1850_and_product_to_4d183c)
{
    g_eax = guest_read32(g_esp + 4u);
    movss_load(&g_xmm0, 0x004D1848u);
    mulss_mem(&g_xmm0, 0x004D1840u);
    guest_write32(0x004D184Cu, g_eax);
    guest_write32(0x004D1850u, g_eax);
    guest_write32(0x004D183Cu, g_xmm0.u[0]);
}

/* 0x0005A150: cdecl(obj). xmm0 = [obj+0x198]; ucomiss xmm0, [0x475CAC]; EAX = 0 when the two
 * floats compare equal, else 1 (unordered counts as not equal). */
GAME_REPLACE_EXACT(0005A150, cdecl, 1, u32, game_float_198_differs_from_475cac)
{
    g_eax = guest_read32(g_esp + 4u);
    movss_load(&g_xmm0, g_eax + 0x198u);
    g_eax = fp_ucomiss_equal(g_xmm0.f[0], fp_mem(0x00475CACu)) ? 0u : 1u;
}

/* 0x0005A1E0: cdecl(obj, key). o = [obj+4]. Returns 1 when [o+0x198] > 0 and [o+0x12C] == key,
 * or else when 0 >= [o+0x198] (re-read) and [o+0xD4] == key. xmm1 = [o+0x198], xmm0 = 0. */
GAME_REPLACE_EXACT(0005A1E0, cdecl, 2, u32, game_float_198_positive_matches_key_12c_or_nonpositive_key_d4)
{
    const uint32_t entry = g_esp;
    g_eax = guest_read32(entry + 4u);
    g_eax = guest_read32(g_eax + 4u);
    movss_load(&g_xmm1, g_eax + 0x198u);
    xorps_clear(&g_xmm0);
    const int above = comiss_above(&g_xmm1, g_xmm0.f[0]);
    g_ecx = guest_read32(entry + 8u);
    if (above && guest_read32(g_eax + 0x12Cu) == g_ecx) goto found;
    if (fp_comiss_below(g_xmm0.f[0], fp_mem(g_eax + 0x198u))) goto missing;
    if (guest_read32(g_eax + 0xD4u) != g_ecx) goto missing;
found:
    g_eax = 1u;
    return;
missing:
    g_eax = 0u;
}

/* 0x00073530: cdecl(obj). xmm0 = [obj+0x88]; [obj+0x84] = xmm0; xmm0 *= [0x7BA188];
 * [obj+0xC0] = xmm0. */
GAME_REPLACE_EXACT(00073530, cdecl, 1, u32, game_float_88_copy_to_84_and_scaled_to_c0)
{
    g_eax = guest_read32(g_esp + 4u);
    movss_load(&g_xmm0, g_eax + 0x88u);
    guest_write32(g_eax + 0x84u, g_xmm0.u[0]);
    mulss_mem(&g_xmm0, 0x007BA188u);
    guest_write32(g_eax + 0xC0u, g_xmm0.u[0]);
}

/* 0x000A4CA0: cdecl(obj). Returns 1 when [obj+0xAC] >= [obj+0x218] and
 * [obj+0x218] + [0x475CE8] > [obj+0xAC]. xmm0 holds the last value computed. */
GAME_REPLACE_EXACT(000A4CA0, cdecl, 1, u32, game_float_ac_within_218_plus_475ce8)
{
    g_eax = guest_read32(g_esp + 4u);
    movss_load(&g_xmm0, g_eax + 0xACu);
    if (fp_comiss_below(g_xmm0.f[0], fp_mem(g_eax + 0x218u))) goto no;
    movss_load(&g_xmm0, g_eax + 0x218u);
    addss_mem(&g_xmm0, 0x00475CE8u);
    if (!comiss_above(&g_xmm0, fp_mem(g_eax + 0xACu))) goto no;
    g_eax = 1u;
    return;
no:
    g_eax = 0u;
}

/* 0x000AEE20: cdecl(actor). o = [actor+0x7C]. If [o+0xAC] > [0x47D5A8]: [o+0xAC] -= [0x4E794C]
 * and bit 0 is set in dword ([actor+4])[+0x30]. Otherwise bit 5 of dword [o+4] is cleared. */
GAME_REPLACE_EXACT(000AEE20, cdecl, 1, u32, game_actor_float_ac_decrement_or_clear_flag_20)
{
    g_ecx = guest_read32(g_esp + 4u);
    g_eax = guest_read32(g_ecx + 0x7Cu);
    movss_load(&g_xmm0, g_eax + 0xACu);
    if (!comiss_above(&g_xmm0, fp_mem(0x0047D5A8u))) {
        guest_write32(g_eax + 4u, guest_read32(g_eax + 4u) & 0xFFFFFFDFu);
        return;
    }
    subss_mem(&g_xmm0, 0x004E794Cu);
    guest_write32(g_eax + 0xACu, g_xmm0.u[0]);
    g_eax = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 0x30u, guest_read32(g_eax + 0x30u) | 1u);
}

/* 0x000C9BC0: cdecl(). EAX = 1 when [0x733108] is nonzero and 0 >= [0x7330BC], else 0. xmm0 is
 * cleared only when [0x733108] is nonzero. */
GAME_REPLACE_EXACT(000C9BC0, cdecl, 0, u32, game_camera_733108_set_and_timer_7330bc_nonpositive)
{
    g_eax = guest_read32(0x00733108u);
    if (g_eax == 0u) {
        g_eax = 0u;
        return;
    }
    xorps_clear(&g_xmm0);
    if (fp_comiss_below(g_xmm0.f[0], fp_mem(0x007330BCu))) {
        g_eax = 0u;
        return;
    }
    g_eax = 1u;
}
