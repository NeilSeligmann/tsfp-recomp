/* SPDX-License-Identifier: GPL-3.0-or-later */
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


extern __thread uint32_t g_ebx, g_ebp, g_edi;
extern __thread int g_df;
GAME_FP_HELPER uint32_t read16(uint32_t a) { return guest_read8(a) | ((uint32_t)guest_read8(a+1u)<<8); }
GAME_FP_HELPER uint32_t flags32(uint32_t a,uint32_t b,unsigned width,unsigned kind,uint32_t *result)
{
    uint32_t mask=width==32 ? UINT32_MAX : (1u<<width)-1u, sign=1u<<(width-1);
    a &= mask; b &= mask;
    uint32_t r; unsigned c=0,o=0;
    if(kind==0) {r=(a-b)&mask; c=a<b; o=((a^b)&(a^r)&sign)!=0;}
    else if(kind==1) {r=(a+b)&mask; c=((uint64_t)a+b)>mask; o=((~(a^b))&(a^r)&sign)!=0;}
    else {r=a&b;}
    *result=r;
    unsigned parity=__builtin_parity(r&255u)==0;
    return c | (parity<<2) | ((uint32_t)(r==0)<<6) | ((uint32_t)((r&sign)!=0)<<7) | (o<<11);
}
GAME_FP_HELPER uint32_t compare_scalar(float a,float b,int quiet)
{
    unsigned char z,c,p;
    if(quiet) __asm__ volatile("ucomiss %4, %3" : "=@ccz"(z), "=@ccb"(c), "=@ccp"(p) : "x"(a), "xm"(b));
    else __asm__ volatile("comiss %4, %3" : "=@ccz"(z), "=@ccb"(c), "=@ccp"(p) : "x"(a), "xm"(b));
    return c | ((uint32_t)p<<2) | ((uint32_t)z<<6);
}
/* Original 0x002ad370;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(002AD370, cdecl, 1, u32, game_t1479_vector_002ad370)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 002AD370: mov eax, dword ptr [esp + 4] */ g_eax = guest_read32((uint32_t)(g_esp + 4));
    /* 002AD374: movss xmm0, dword ptr [0x47d610] */ fp_load_ss(&g_xmm0, (uint32_t)(0x47d610));
    /* 002AD37C: xorps xmm1, xmm1 */ g_xmm1.q[0]=0u;
    g_xmm1.q[1]=0u;
    /* 002AD37F: movss xmm2, dword ptr [0x4760cc] */ fp_load_ss(&g_xmm2, (uint32_t)(0x4760cc));
    /* 002AD387: mov ecx, 0x12c */ g_ecx = ((uint32_t)0x12c);
    /* 002AD38C: lea esp, [esp] */ g_esp = (uint32_t)(g_esp);
L_2ad390:;
    /* 002AD390: movss xmm3, dword ptr [eax] */ fp_load_ss(&g_xmm3, (uint32_t)(g_eax));
    /* 002AD394: comiss xmm3, xmm0 */ flags=compare_scalar(g_xmm3.f[0],g_xmm0.f[0],0);
    /* 002AD397: jbe 0x2ad39d */ if ((flags&65u)) { goto L_2ad39d;
    }
    /* 002AD399: movss dword ptr [eax], xmm1 */ fp_store_ss((uint32_t)(g_eax), &g_xmm1);
L_2ad39d:;
    /* 002AD39D: movss xmm3, dword ptr [eax] */ fp_load_ss(&g_xmm3, (uint32_t)(g_eax));
    /* 002AD3A1: addss xmm3, dword ptr [eax + 4] */ g_xmm3.f[0]=fp_addss(g_xmm3.f[0],fp_guest_float((uint32_t)(g_eax + 4)));
    /* 002AD3A6: movss dword ptr [eax], xmm3 */ fp_store_ss((uint32_t)(g_eax), &g_xmm3);
    /* 002AD3AA: movss xmm3, dword ptr [eax + 4] */ fp_load_ss(&g_xmm3, (uint32_t)(g_eax + 4));
    /* 002AD3AF: mulss xmm3, xmm2 */ g_xmm3.f[0]=fp_mulss(g_xmm3.f[0],g_xmm2.f[0]);
    /* 002AD3B3: addss xmm3, dword ptr [eax + 8] */ g_xmm3.f[0]=fp_addss(g_xmm3.f[0],fp_guest_float((uint32_t)(g_eax + 8)));
    /* 002AD3B8: movss dword ptr [eax + 8], xmm3 */ fp_store_ss((uint32_t)(g_eax + 8), &g_xmm3);
    /* 002AD3BD: add eax, 0xc */ flags=flags32(g_eax,((uint32_t)0xc),32u,1u,&result);g_eax = result;
    /* 002AD3C0: dec ecx */ left=flags&1u;
    flags=flags32(g_ecx,1u,32u,0u,&result);g_ecx = result;
    flags=(flags&~1u)|left;
    /* 002AD3C1: jne 0x2ad390 */ if (!(flags&64u)) { goto L_2ad390;
    }
    /* 002AD3C3: ret  */ return;
}
/* Original 0x002e5900;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(002E5900, cdecl, 4, u32, game_t1479_vector_002e5900)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 002E5900: push esi */ left=g_esi;
    g_esp-=4u;
    guest_write32(g_esp,left);
    /* 002E5901: movss xmm3, dword ptr [0x475c78] */ fp_load_ss(&g_xmm3, (uint32_t)(0x475c78));
    /* 002E5909: movss xmm2, dword ptr [0x475cd4] */ fp_load_ss(&g_xmm2, (uint32_t)(0x475cd4));
    /* 002E5911: movss xmm5, dword ptr [0x475ce0] */ fp_load_ss(&g_xmm5, (uint32_t)(0x475ce0));
    /* 002E5919: movss xmm4, dword ptr [esp + 0xc] */ fp_load_ss(&g_xmm4, (uint32_t)(g_esp + 0xc));
    /* 002E591F: movaps xmm0, xmm3 */ fp_movaps_reg(&g_xmm0, &g_xmm3);
    /* 002E5922: subss xmm0, dword ptr [esp + 0x10] */ g_xmm0.f[0]=fp_subss(g_xmm0.f[0],fp_guest_float((uint32_t)(g_esp + 0x10)));
    /* 002E5928: subss xmm0, xmm2 */ g_xmm0.f[0]=fp_subss(g_xmm0.f[0],g_xmm2.f[0]);
    /* 002E592C: mulss xmm0, dword ptr [0x49f6d4] */ g_xmm0.f[0]=fp_mulss(g_xmm0.f[0],fp_guest_float((uint32_t)(0x49f6d4)));
    /* 002E5934: mulss xmm0, xmm5 */ g_xmm0.f[0]=fp_mulss(g_xmm0.f[0],g_xmm5.f[0]);
    /* 002E5938: addss xmm0, xmm2 */ g_xmm0.f[0]=fp_addss(g_xmm0.f[0],g_xmm2.f[0]);
    /* 002E593C: cvttss2si eax, xmm0 */ g_eax = (uint32_t)fp_cvttss2si(g_xmm0.f[0]);
    /* 002E5940: lea ecx, [eax + 0x200] */ g_ecx = (uint32_t)(g_eax + 0x200);
    /* 002E5946: and ecx, 0x7ff */ g_ecx = g_ecx & ((uint32_t)0x7ff);
    flags=flags32(g_ecx,UINT32_MAX,32u,2u,&result);
    /* 002E594C: movss xmm0, dword ptr [ecx*4 + 0x7a38a0] */ fp_load_ss(&g_xmm0, (uint32_t)(g_ecx*4 + 0x7a38a0));
    /* 002E5955: mulss xmm0, dword ptr [0x475d18] */ g_xmm0.f[0]=fp_mulss(g_xmm0.f[0],fp_guest_float((uint32_t)(0x475d18)));
    /* 002E595D: lea ecx, [ecx*4 + 0x7a38a0] */ g_ecx = (uint32_t)(g_ecx*4 + 0x7a38a0);
    /* 002E5964: movss dword ptr [esp + 0xc], xmm0 */ fp_store_ss((uint32_t)(g_esp + 0xc), &g_xmm0);
    /* 002E596A: mov edx, dword ptr [esp + 0xc] */ g_edx = guest_read32((uint32_t)(g_esp + 0xc));
    /* 002E596E: movaps xmm1, xmm3 */ fp_movaps_reg(&g_xmm1, &g_xmm3);
    /* 002E5971: subss xmm1, xmm4 */ g_xmm1.f[0]=fp_subss(g_xmm1.f[0],g_xmm4.f[0]);
    /* 002E5975: subss xmm1, xmm2 */ g_xmm1.f[0]=fp_subss(g_xmm1.f[0],g_xmm2.f[0]);
    /* 002E5979: mulss xmm1, dword ptr [0x49f6d0] */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],fp_guest_float((uint32_t)(0x49f6d0)));
    /* 002E5981: mulss xmm1, xmm5 */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],g_xmm5.f[0]);
    /* 002E5985: addss xmm1, xmm2 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm2.f[0]);
    /* 002E5989: and edx, 0x7fffffff */ g_edx = g_edx & ((uint32_t)0x7fffffff);
    flags=flags32(g_edx,UINT32_MAX,32u,2u,&result);
    /* 002E598F: mov dword ptr [esp + 0xc], edx */ guest_write32((uint32_t)(g_esp + 0xc), g_edx);
    /* 002E5993: cvttss2si edx, xmm1 */ g_edx = (uint32_t)fp_cvttss2si(g_xmm1.f[0]);
    /* 002E5997: movss xmm1, dword ptr [esp + 0xc] */ fp_load_ss(&g_xmm1, (uint32_t)(g_esp + 0xc));
    /* 002E599D: addss xmm1, xmm3 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm3.f[0]);
    /* 002E59A1: mov esi, edx */ g_esi = g_edx;
    /* 002E59A3: and esi, 0x7ff */ g_esi = g_esi & ((uint32_t)0x7ff);
    flags=flags32(g_esi,UINT32_MAX,32u,2u,&result);
    /* 002E59A9: mulss xmm1, dword ptr [esi*4 + 0x7a38a0] */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],fp_guest_float((uint32_t)(g_esi*4 + 0x7a38a0)));
    /* 002E59B2: mov esi, dword ptr [esp + 8] */ g_esi = guest_read32((uint32_t)(g_esp + 8));
    /* 002E59B6: movaps xmm0, xmm3 */ fp_movaps_reg(&g_xmm0, &g_xmm3);
    /* 002E59B9: subss xmm0, dword ptr [esp + 0x14] */ g_xmm0.f[0]=fp_subss(g_xmm0.f[0],fp_guest_float((uint32_t)(g_esp + 0x14)));
    /* 002E59BF: mulss xmm1, xmm0 */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 002E59C3: mulss xmm0, dword ptr [0x47d6d4] */ g_xmm0.f[0]=fp_mulss(g_xmm0.f[0],fp_guest_float((uint32_t)(0x47d6d4)));
    /* 002E59CB: movss dword ptr [esi], xmm1 */ fp_store_ss((uint32_t)(g_esi), &g_xmm1);
    /* 002E59CF: subss xmm4, xmm2 */ g_xmm4.f[0]=fp_subss(g_xmm4.f[0],g_xmm2.f[0]);
    /* 002E59D3: movaps xmm1, xmm4 */ fp_movaps_reg(&g_xmm1, &g_xmm4);
    /* 002E59D6: mulss xmm1, xmm4 */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],g_xmm4.f[0]);
    /* 002E59DA: addss xmm1, xmm3 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm3.f[0]);
    /* 002E59DE: and eax, 0x7ff */ g_eax = g_eax & ((uint32_t)0x7ff);
    flags=flags32(g_eax,UINT32_MAX,32u,2u,&result);
    /* 002E59E3: mulss xmm1, dword ptr [eax*4 + 0x7a38a0] */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],fp_guest_float((uint32_t)(g_eax*4 + 0x7a38a0)));
    /* 002E59EC: mulss xmm1, dword ptr [0x476040] */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],fp_guest_float((uint32_t)(0x476040)));
    /* 002E59F4: movss dword ptr [esi + 4], xmm1 */ fp_store_ss((uint32_t)(g_esi + 4), &g_xmm1);
    /* 002E59F9: add edx, 0x200 */ flags=flags32(g_edx,((uint32_t)0x200),32u,1u,&result);g_edx = result;
    /* 002E59FF: and edx, 0x7ff */ g_edx = g_edx & ((uint32_t)0x7ff);
    flags=flags32(g_edx,UINT32_MAX,32u,2u,&result);
    /* 002E5A05: movss xmm1, dword ptr [edx*4 + 0x7a38a0] */ fp_load_ss(&g_xmm1, (uint32_t)(g_edx*4 + 0x7a38a0));
    /* 002E5A0E: addss xmm1, dword ptr [ecx] */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],fp_guest_float((uint32_t)(g_ecx)));
    /* 002E5A12: mulss xmm1, xmm2 */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],g_xmm2.f[0]);
    /* 002E5A16: mulss xmm1, xmm0 */ g_xmm1.f[0]=fp_mulss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 002E5A1A: movss dword ptr [esi + 8], xmm1 */ fp_store_ss((uint32_t)(g_esi + 8), &g_xmm1);
    /* 002E5A1F: pop esi */ g_esi = guest_read32(g_esp);
    g_esp+=4u;
    /* 002E5A20: ret  */ return;
}
/* Original 0x00307e60;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(00307E60, cdecl, 3, u32, game_t1479_vector_00307e60)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00307E60: cvttss2si eax, dword ptr [esp + 0xc] */ g_eax = (uint32_t)fp_cvttss2si(fp_guest_float((uint32_t)(g_esp + 0xc)));
    /* 00307E66: cvttss2si ecx, dword ptr [esp + 4] */ g_ecx = (uint32_t)fp_cvttss2si(fp_guest_float((uint32_t)(g_esp + 4)));
    /* 00307E6C: cvttss2si edx, dword ptr [esp + 8] */ g_edx = (uint32_t)fp_cvttss2si(fp_guest_float((uint32_t)(g_esp + 8)));
    /* 00307E72: add eax, 2 */ flags=flags32(g_eax,((uint32_t)2),32u,1u,&result);g_eax = result;
    /* 00307E75: test ecx, ecx */ flags=flags32(g_ecx,g_ecx,32u,2u,&result);
    /* 00307E77: jl 0x307eb1 */ if ((((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307E79: cmp ecx, 0x28 */ flags=flags32(g_ecx,((uint32_t)0x28),32u,0u,&result);
    /* 00307E7C: jge 0x307eb1 */ if (!(((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307E7E: test edx, edx */ flags=flags32(g_edx,g_edx,32u,2u,&result);
    /* 00307E80: jl 0x307eb1 */ if ((((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307E82: cmp edx, 0x28 */ flags=flags32(g_edx,((uint32_t)0x28),32u,0u,&result);
    /* 00307E85: jge 0x307eb1 */ if (!(((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307E87: test eax, eax */ flags=flags32(g_eax,g_eax,32u,2u,&result);
    /* 00307E89: jl 0x307eb1 */ if ((((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307E8B: cmp eax, 6 */ flags=flags32(g_eax,((uint32_t)6),32u,0u,&result);
    /* 00307E8E: jge 0x307eb1 */ if (!(((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307E90: lea eax, [eax + eax*4] */ g_eax = (uint32_t)(g_eax + g_eax*4);
    /* 00307E93: lea eax, [edx + eax*8] */ g_eax = (uint32_t)(g_edx + g_eax*8);
    /* 00307E96: lea edx, [eax + eax*4] */ g_edx = (uint32_t)(g_eax + g_eax*4);
    /* 00307E99: lea eax, [ecx + edx*8] */ g_eax = (uint32_t)(g_ecx + g_edx*8);
    /* 00307E9C: test eax, eax */ flags=flags32(g_eax,g_eax,32u,2u,&result);
    /* 00307E9E: jl 0x307eb1 */ if ((((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307EA0: cmp eax, 0x2580 */ flags=flags32(g_eax,((uint32_t)0x2580),32u,0u,&result);
    /* 00307EA5: jge 0x307eb1 */ if (!(((flags>>7)^(flags>>11))&1u)) { goto L_307eb1;
    }
    /* 00307EA7: mov ecx, dword ptr [0x784004] */ g_ecx = guest_read32((uint32_t)(0x784004));
    /* 00307EAD: mov eax, dword ptr [ecx + eax*4] */ g_eax = guest_read32((uint32_t)(g_ecx + g_eax*4));
    /* 00307EB0: ret  */ return;
L_307eb1:;
    /* 00307EB1: xor eax, eax */ g_eax = g_eax ^ g_eax;
    flags=flags32(g_eax,UINT32_MAX,32u,2u,&result);
    /* 00307EB3: ret  */ return;
}
/* Original 0x00309160;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(00309160, cdecl, 2, u32, game_t1479_vector_00309160)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00309160: movss xmm0, dword ptr [0x783dd8] */ fp_load_ss(&g_xmm0, (uint32_t)(0x783dd8));
    /* 00309168: addss xmm0, dword ptr [esp + 4] */ g_xmm0.f[0]=fp_addss(g_xmm0.f[0],fp_guest_float((uint32_t)(g_esp + 4)));
    /* 0030916E: movss xmm1, dword ptr [0x783cc0] */ fp_load_ss(&g_xmm1, (uint32_t)(0x783cc0));
    /* 00309176: movss xmm2, dword ptr [0x783de0] */ fp_load_ss(&g_xmm2, (uint32_t)(0x783de0));
    /* 0030917E: addss xmm1, dword ptr [esp + 8] */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],fp_guest_float((uint32_t)(g_esp + 8)));
    /* 00309184: addss xmm2, xmm0 */ g_xmm2.f[0]=fp_addss(g_xmm2.f[0],g_xmm0.f[0]);
    /* 00309188: movss dword ptr [0x783dd8], xmm0 */ fp_store_ss((uint32_t)(0x783dd8), &g_xmm0);
    /* 00309190: movss xmm0, dword ptr [0x783ddc] */ fp_load_ss(&g_xmm0, (uint32_t)(0x783ddc));
    /* 00309198: addss xmm0, xmm1 */ g_xmm0.f[0]=fp_addss(g_xmm0.f[0],g_xmm1.f[0]);
    /* 0030919C: movss dword ptr [0x783cc0], xmm1 */ fp_store_ss((uint32_t)(0x783cc0), &g_xmm1);
    /* 003091A4: movss dword ptr [0x783dc0], xmm2 */ fp_store_ss((uint32_t)(0x783dc0), &g_xmm2);
    /* 003091AC: movss dword ptr [0x783dcc], xmm0 */ fp_store_ss((uint32_t)(0x783dcc), &g_xmm0);
    /* 003091B4: ret  */ return;
}
/* Original 0x00319440;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(00319440, cdecl, 2, u32, game_t1479_vector_00319440)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00319440: mov ecx, dword ptr [esp + 4] */ g_ecx = guest_read32((uint32_t)(g_esp + 4));
    /* 00319444: movss xmm0, dword ptr [ecx] */ fp_load_ss(&g_xmm0, (uint32_t)(g_ecx));
    /* 00319448: mov eax, dword ptr [esp + 8] */ g_eax = guest_read32((uint32_t)(g_esp + 8));
    /* 0031944C: movss xmm2, dword ptr [0x4760dc] */ fp_load_ss(&g_xmm2, (uint32_t)(0x4760dc));
    /* 00319454: comiss xmm0, xmm2 */ flags=compare_scalar(g_xmm0.f[0],g_xmm2.f[0],0);
    /* 00319457: movss xmm1, dword ptr [eax] */ fp_load_ss(&g_xmm1, (uint32_t)(g_eax));
    /* 0031945B: jbe 0x319460 */ if ((flags&65u)) { goto L_319460;
    }
    /* 0031945D: movaps xmm0, xmm2 */ fp_movaps_reg(&g_xmm0, &g_xmm2);
L_319460:;
    /* 00319460: movss dword ptr [ecx], xmm0 */ fp_store_ss((uint32_t)(g_ecx), &g_xmm0);
    /* 00319464: movss xmm0, dword ptr [0x4760cc] */ fp_load_ss(&g_xmm0, (uint32_t)(0x4760cc));
    /* 0031946C: comiss xmm0, xmm1 */ flags=compare_scalar(g_xmm0.f[0],g_xmm1.f[0],0);
    /* 0031946F: jbe 0x319476 */ if ((flags&65u)) { goto L_319476;
    }
    /* 00319471: movss dword ptr [eax], xmm0 */ fp_store_ss((uint32_t)(g_eax), &g_xmm0);
    /* 00319475: ret  */ return;
L_319476:;
    /* 00319476: movss dword ptr [eax], xmm1 */ fp_store_ss((uint32_t)(g_eax), &g_xmm1);
    /* 0031947A: ret  */ return;
}
/* Original 0x00341630;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(00341630, cdecl, 0, u32, game_t1479_vector_00341630)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00341630: xorps xmm0, xmm0 */ g_xmm0.q[0]=0u;
    g_xmm0.q[1]=0u;
    /* 00341633: movss dword ptr [0x7789dc], xmm0 */ fp_store_ss((uint32_t)(0x7789dc), &g_xmm0);
    /* 0034163B: mov dword ptr [0x7789f4], 0 */ guest_write32((uint32_t)(0x7789f4), ((uint32_t)0));
    /* 00341645: movss dword ptr [0x7789e4], xmm0 */ fp_store_ss((uint32_t)(0x7789e4), &g_xmm0);
    /* 0034164D: ret  */ return;
}
/* Original 0x00347b80;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(00347B80, cdecl, 0, u32, game_t1479_vector_00347b80)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00347B80: movss xmm0, dword ptr [0x4760d0] */ fp_load_ss(&g_xmm0, (uint32_t)(0x4760d0));
    /* 00347B88: push edi */ left=g_edi;
    g_esp-=4u;
    guest_write32(g_esp,left);
    /* 00347B89: mov ecx, 0x160 */ g_ecx = ((uint32_t)0x160);
    /* 00347B8E: xor eax, eax */ g_eax = g_eax ^ g_eax;
    flags=flags32(g_eax,UINT32_MAX,32u,2u,&result);
    /* 00347B90: mov edi, 0x767100 */ g_edi = ((uint32_t)0x767100);
    /* 00347B95: rep stosd dword ptr es:[edi], eax */ while(g_ecx){guest_write32(g_edi,g_eax);g_edi+=g_df?(uint32_t)-4:4u;--g_ecx;}
    /* 00347B97: mov dword ptr [0x7670f8], 0 */ guest_write32((uint32_t)(0x7670f8), ((uint32_t)0));
    /* 00347BA1: movss dword ptr [0x7670fc], xmm0 */ fp_store_ss((uint32_t)(0x7670fc), &g_xmm0);
    /* 00347BA9: pop edi */ g_edi = guest_read32(g_esp);
    g_esp+=4u;
    /* 00347BAA: ret  */ return;
}
/* Original 0x00349570;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT_INPUTS(00349570, cdecl, 0, u32, ecx, game_t1479_vector_00349570)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00349570: xorps xmm0, xmm0 */ g_xmm0.q[0]=0u;
    g_xmm0.q[1]=0u;
    /* 00349573: xor eax, eax */ g_eax = g_eax ^ g_eax;
    flags=flags32(g_eax,UINT32_MAX,32u,2u,&result);
    /* 00349575: mov dword ptr [0x767694], eax */ guest_write32((uint32_t)(0x767694), g_eax);
    /* 0034957A: mov dword ptr [0x7676b4], eax */ guest_write32((uint32_t)(0x7676b4), g_eax);
    /* 0034957F: movss dword ptr [0x767684], xmm0 */ fp_store_ss((uint32_t)(0x767684), &g_xmm0);
    /* 00349587: movss dword ptr [0x767688], xmm0 */ fp_store_ss((uint32_t)(0x767688), &g_xmm0);
    /* 0034958F: movss dword ptr [0x76768c], xmm0 */ fp_store_ss((uint32_t)(0x76768c), &g_xmm0);
    /* 00349597: mov edx, 0x778840 */ g_edx = ((uint32_t)0x778840);
    /* 0034959C: push edi */ left=g_edi;
    g_esp-=4u;
    guest_write32(g_esp,left);
    /* 0034959D: lea ecx, [ecx] */ g_ecx = (uint32_t)(g_ecx);
L_3495a0:;
    /* 003495A0: xor eax, eax */ g_eax = g_eax ^ g_eax;
    flags=flags32(g_eax,UINT32_MAX,32u,2u,&result);
    /* 003495A2: lea edi, [edx - 0x40] */ g_edi = (uint32_t)(g_edx - 0x40);
    /* 003495A5: mov ecx, 0x1a */ g_ecx = ((uint32_t)0x1a);
    /* 003495AA: rep stosd dword ptr es:[edi], eax */ while(g_ecx){guest_write32(g_edi,g_eax);g_edi+=g_df?(uint32_t)-4:4u;--g_ecx;}
    /* 003495AC: mov dword ptr [edx], 2 */ guest_write32((uint32_t)(g_edx), ((uint32_t)2));
    /* 003495B2: mov dword ptr [edx + 4], 0xffffffff */ guest_write32((uint32_t)(g_edx + 4), ((uint32_t)0xffffffff));
    /* 003495B9: add edx, 0x68 */ flags=flags32(g_edx,((uint32_t)0x68),32u,1u,&result);g_edx = result;
    /* 003495BC: cmp edx, 0x7789e0 */ flags=flags32(g_edx,((uint32_t)0x7789e0),32u,0u,&result);
    /* 003495C2: jl 0x3495a0 */ if ((((flags>>7)^(flags>>11))&1u)) { goto L_3495a0;
    }
    /* 003495C4: pop edi */ g_edi = guest_read32(g_esp);
    g_esp+=4u;
    /* 003495C5: ret  */ return;
}
/* Original 0x0036e920;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(0036E920, cdecl, 3, u32, game_t1479_vector_0036e920)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 0036E920: mov eax, dword ptr [esp + 4] */ g_eax = guest_read32((uint32_t)(g_esp + 4));
    /* 0036E924: movss xmm0, dword ptr [esp + 8] */ fp_load_ss(&g_xmm0, (uint32_t)(g_esp + 8));
    /* 0036E92A: movss xmm1, dword ptr [eax + 0x20] */ fp_load_ss(&g_xmm1, (uint32_t)(g_eax + 0x20));
    /* 0036E92F: addss xmm1, xmm0 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 0036E933: movss dword ptr [eax], xmm1 */ fp_store_ss((uint32_t)(g_eax), &g_xmm1);
    /* 0036E937: movss xmm1, dword ptr [eax + 0x28] */ fp_load_ss(&g_xmm1, (uint32_t)(g_eax + 0x28));
    /* 0036E93C: addss xmm1, xmm0 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 0036E940: movss xmm0, dword ptr [esp + 0xc] */ fp_load_ss(&g_xmm0, (uint32_t)(g_esp + 0xc));
    /* 0036E946: movss dword ptr [eax + 8], xmm1 */ fp_store_ss((uint32_t)(g_eax + 8), &g_xmm1);
    /* 0036E94B: movss xmm1, dword ptr [eax + 0x24] */ fp_load_ss(&g_xmm1, (uint32_t)(g_eax + 0x24));
    /* 0036E950: addss xmm1, xmm0 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 0036E954: movss dword ptr [eax + 4], xmm1 */ fp_store_ss((uint32_t)(g_eax + 4), &g_xmm1);
    /* 0036E959: movss xmm1, dword ptr [eax + 0x2c] */ fp_load_ss(&g_xmm1, (uint32_t)(g_eax + 0x2c));
    /* 0036E95E: addss xmm1, xmm0 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 0036E962: movss dword ptr [eax + 0xc], xmm1 */ fp_store_ss((uint32_t)(g_eax + 0xc), &g_xmm1);
    /* 0036E967: ret  */ return;
}
/* Original 0x0036ec40;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(0036EC40, cdecl, 3, u32, game_t1479_vector_0036ec40)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 0036EC40: mov eax, dword ptr [esp + 4] */ g_eax = guest_read32((uint32_t)(g_esp + 4));
    /* 0036EC44: movss xmm0, dword ptr [esp + 8] */ fp_load_ss(&g_xmm0, (uint32_t)(g_esp + 8));
    /* 0036EC4A: movss xmm1, dword ptr [eax + 0x10] */ fp_load_ss(&g_xmm1, (uint32_t)(g_eax + 0x10));
    /* 0036EC4F: movss xmm2, dword ptr [eax + 0x14] */ fp_load_ss(&g_xmm2, (uint32_t)(g_eax + 0x14));
    /* 0036EC54: addss xmm1, xmm0 */ g_xmm1.f[0]=fp_addss(g_xmm1.f[0],g_xmm0.f[0]);
    /* 0036EC58: movss dword ptr [eax], xmm1 */ fp_store_ss((uint32_t)(g_eax), &g_xmm1);
    /* 0036EC5C: movss xmm1, dword ptr [esp + 0xc] */ fp_load_ss(&g_xmm1, (uint32_t)(g_esp + 0xc));
    /* 0036EC62: addss xmm2, xmm1 */ g_xmm2.f[0]=fp_addss(g_xmm2.f[0],g_xmm1.f[0]);
    /* 0036EC66: movss dword ptr [eax + 4], xmm2 */ fp_store_ss((uint32_t)(g_eax + 4), &g_xmm2);
    /* 0036EC6B: movss xmm2, dword ptr [eax + 0x18] */ fp_load_ss(&g_xmm2, (uint32_t)(g_eax + 0x18));
    /* 0036EC70: addss xmm2, xmm0 */ g_xmm2.f[0]=fp_addss(g_xmm2.f[0],g_xmm0.f[0]);
    /* 0036EC74: movss xmm0, dword ptr [eax + 0x1c] */ fp_load_ss(&g_xmm0, (uint32_t)(g_eax + 0x1c));
    /* 0036EC79: addss xmm0, xmm1 */ g_xmm0.f[0]=fp_addss(g_xmm0.f[0],g_xmm1.f[0]);
    /* 0036EC7D: movss dword ptr [eax + 8], xmm2 */ fp_store_ss((uint32_t)(g_eax + 8), &g_xmm2);
    /* 0036EC82: movss dword ptr [eax + 0xc], xmm0 */ fp_store_ss((uint32_t)(g_eax + 0xc), &g_xmm0);
    /* 0036EC87: ret  */ return;
}
/* Original 0x00377560;
    ordered guest accesses and actual saved-slot reloads. */
GAME_REPLACE_EXACT(00377560, cdecl, 5, u32, game_t1479_vector_00377560)
{
    uint32_t flags=0, result=0, left=0, right=0;
    (void)flags;
    (void)result;
    (void)left;
    (void)right;
    /* 00377560: mov eax, dword ptr [esp + 4] */ g_eax = guest_read32((uint32_t)(g_esp + 4));
    /* 00377564: movss xmm1, dword ptr [esp + 0x10] */ fp_load_ss(&g_xmm1, (uint32_t)(g_esp + 0x10));
    /* 0037756A: cvtsi2ss xmm3, dword ptr [eax + 0x84] */ fp_cvtsi2ss(&g_xmm3,guest_read32((uint32_t)(g_eax + 0x84)));
    /* 00377572: movss xmm0, dword ptr [esp + 8] */ fp_load_ss(&g_xmm0, (uint32_t)(g_esp + 8));
    /* 00377578: movss xmm2, dword ptr [esp + 0xc] */ fp_load_ss(&g_xmm2, (uint32_t)(g_esp + 0xc));
    /* 0037757E: movaps xmm4, xmm1 */ fp_movaps_reg(&g_xmm4, &g_xmm1);
    /* 00377581: subss xmm4, xmm3 */ g_xmm4.f[0]=fp_subss(g_xmm4.f[0],g_xmm3.f[0]);
    /* 00377585: cvttss2si ecx, xmm4 */ g_ecx = (uint32_t)fp_cvttss2si(g_xmm4.f[0]);
    /* 00377589: movss dword ptr [eax + 0x28], xmm0 */ fp_store_ss((uint32_t)(g_eax + 0x28), &g_xmm0);
    /* 0037758E: movss xmm0, dword ptr [esp + 0x14] */ fp_load_ss(&g_xmm0, (uint32_t)(g_esp + 0x14));
    /* 00377594: mov dword ptr [eax], ecx */ guest_write32((uint32_t)(g_eax), g_ecx);
    /* 00377596: cvttss2si ecx, xmm1 */ g_ecx = (uint32_t)fp_cvttss2si(g_xmm1.f[0]);
    /* 0037759A: movss dword ptr [eax + 0x30], xmm1 */ fp_store_ss((uint32_t)(g_eax + 0x30), &g_xmm1);
    /* 0037759F: cvtsi2ss xmm1, dword ptr [eax + 0x20] */ fp_cvtsi2ss(&g_xmm1,guest_read32((uint32_t)(g_eax + 0x20)));
    /* 003775A4: cvttss2si edx, xmm2 */ g_edx = (uint32_t)fp_cvttss2si(g_xmm2.f[0]);
    /* 003775A8: movss dword ptr [eax + 0x34], xmm0 */ fp_store_ss((uint32_t)(g_eax + 0x34), &g_xmm0);
    /* 003775AD: mov dword ptr [eax + 4], edx */ guest_write32((uint32_t)(g_eax + 4), g_edx);
    /* 003775B0: subss xmm0, xmm1 */ g_xmm0.f[0]=fp_subss(g_xmm0.f[0],g_xmm1.f[0]);
    /* 003775B4: cvttss2si edx, xmm0 */ g_edx = (uint32_t)fp_cvttss2si(g_xmm0.f[0]);
    /* 003775B8: movss dword ptr [eax + 0x2c], xmm2 */ fp_store_ss((uint32_t)(g_eax + 0x2c), &g_xmm2);
    /* 003775BD: mov dword ptr [eax + 8], ecx */ guest_write32((uint32_t)(g_eax + 8), g_ecx);
    /* 003775C0: mov dword ptr [eax + 0xc], edx */ guest_write32((uint32_t)(g_eax + 0xc), g_edx);
    /* 003775C3: ret  */ return;
}
