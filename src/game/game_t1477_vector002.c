/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1477 vector census. Original-order register/memory contracts, raw XMM lanes.
 * Names retain their existing overlay confidence. See docs/t1477-vector-census.md.
 */
#include <string.h>
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_ebp, g_edi;
    
extern __thread int g_df;
    
typedef union T1477Xmm { float f[4];
     uint32_t u[4];
     uint64_t q[2];
     } T1477Xmm;
    
__thread T1477Xmm g_xmm0 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm1 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm2 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm3 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm4 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm5 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm6 __attribute__((weak, aligned(8)));
    
__thread T1477Xmm g_xmm7 __attribute__((weak, aligned(8)));
    

#define VHELP static __attribute__((unused))
VHELP void vec_load_scalar(T1477Xmm *dst, uint32_t bits)
{
    dst->u[0] = bits;
     dst->u[1] = 0u;
     dst->u[2] = 0u;
     dst->u[3] = 0u;
    
}
VHELP uint16_t guest_read16(uint32_t address) { uint16_t value;
     memcpy(&value, game_host_ptr(address), sizeof value);
     return value;
     }
VHELP float vec_float(uint32_t bits) { float value;
     memcpy(&value, &bits, 4);
     return value;
     }
VHELP uint32_t vec_parity(uint32_t value) { return (uint32_t)!__builtin_parity(value & 255u);
     }
VHELP float vec_add(float dest, float source) { __asm__ volatile("addss %1, %0" : "+x"(dest) : "xm"(source));
     return dest;
     }
VHELP float vec_sub(float dest, float source) { __asm__ volatile("subss %1, %0" : "+x"(dest) : "xm"(source));
     return dest;
     }
VHELP float vec_mul(float dest, float source) { __asm__ volatile("mulss %1, %0" : "+x"(dest) : "xm"(source));
     return dest;
     }
VHELP float vec_div(float dest, float source) { __asm__ volatile("divss %1, %0" : "+x"(dest) : "xm"(source));
     return dest;
     }
VHELP float vec_convert(int32_t value) { float result;
     __asm__ volatile("cvtsi2ss %1, %0" : "=x"(result) : "rm"(value));
     return result;
     }
VHELP uint32_t vec_compare(float dest, float source, int unordered)
{
    unsigned char below, equal, parity;
    
    if (unordered) {
        __asm__ volatile("ucomiss %4, %2" : "=@ccb"(below), "=@ccz"(equal), "+x"(dest), "=@ccp"(parity) : "xm"(source));
    
    } else {
        __asm__ volatile("comiss %4, %2" : "=@ccb"(below), "=@ccz"(equal), "+x"(dest), "=@ccp"(parity) : "xm"(source));
    
    }
    return (uint32_t)below | ((uint32_t)equal << 6) | ((uint32_t)parity << 2);
    
}


/* 0x0011f540: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0011F540, cdecl, 1, u32, game_object_set_field_0xac_default_mark_dirty_0x1000)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(0x4782C4u)));
    
    left = g_ecx;
     right = 0x1000u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
    guest_write32((uint32_t)(g_eax + 0xACu), g_xmm0.u[0]);
    
    return;
    
}


/* 0x0011fe80: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0011FE80, cdecl, 2, u32, game_effect_arm_flag_0x2_mode_0x88_1_target_0x8c_0x3f_with_arg_0x94)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x8u)));
    
    left = g_ecx;
     right = 0x2u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x94u), g_xmm0.u[0]);
    
    g_xmm0.q[0] ^= g_xmm0.q[0];
     g_xmm0.q[1] ^= g_xmm0.q[1];
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
    guest_write32((uint32_t)(g_eax + 0x88u), 0x1u);
    
    guest_write32((uint32_t)(g_eax + 0x90u), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(g_eax + 0x8Cu), 0x3Fu);
    
    return;
    
}


/* 0x00129ec0: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00129EC0, cdecl, 1, u32, game_object_release_children_0x1f8_state_and_table_742360_entries)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    value = g_ebx;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    value = g_ebp;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    g_ebp = guest_read32((uint32_t)(g_esp + 0xCu));
    
    g_ecx = guest_read32((uint32_t)(g_ebp + 0x1Cu));
    
    g_eax = guest_read32((uint32_t)(g_ebp + 0x1F8u));
    
    value = g_edi;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    left = g_ebx;
     right = g_ebx;
     value = (left ^ right) & 0xFFFFFFFFu;
     g_ebx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_edi;
     right = g_edi;
     value = (left ^ right) & 0xFFFFFFFFu;
     g_edi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_ecx;
     right = g_ebx;
     value = (left - right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    if (zf || sf != of) goto label_00129F43;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(0x47D9D4u)));
    
    g_xmm1.q[0] ^= g_xmm1.q[0];
     g_xmm1.q[1] ^= g_xmm1.q[1];
    
    value = g_esi;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    g_esi = g_eax;
    
label_00129EE6:;
    
    g_eax = guest_read32((uint32_t)(g_esi));
    
    left = guest_read16((uint32_t)(g_eax + 0x28u));
     right = 0x40Cu;
     value = (left & right) & 0xFFFFu;
     zf = value == 0u;
     sf = (value >> 15) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (!zf) goto label_00129EF6;
    
    guest_write32((uint32_t)(g_eax), g_xmm0.u[0]);
    
    goto label_00129EFA;
    
label_00129EF6:;
    
    guest_write32((uint32_t)(g_eax), g_xmm1.u[0]);
    
label_00129EFA:;
    
    left = g_eax;
     right = 0xEu;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    g_edx = 0x2u;
    
label_00129F02:;
    
    g_ecx = (g_ecx & 0xFFFFFF00u) | ((guest_read8((uint32_t)(g_eax))) & 255u);
    
    left = ((g_ecx >> 0) & 255u);
     right = ((g_ebx >> 0) & 255u);
     value = (left - right) & 0xFFu;
     zf = value == 0u;
     sf = (value >> 7) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 7) & 1u;
     cf = left < right;
    
    if (sf != of) goto label_00129F17;
    
    g_ecx = (uint32_t)(int32_t)(int8_t)(((g_ecx >> 0) & 255u));
    
    g_ecx = g_ecx * 0x4Cu;
    
    guest_write32((uint32_t)(g_ecx + 0x7ABFA4u), g_ebx);
    
    guest_write8((uint32_t)(g_eax), 0xFFu);
    
label_00129F17:;
    
    left = g_eax;
     right = 1u;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     
    left = g_edx;
     right = 1u;
     value = (left - right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     
    if (!zf) goto label_00129F02;
    
    g_eax = 0x742360u;
    
label_00129F20:;
    
    left = guest_read32((uint32_t)(g_eax));
     right = g_ebx;
     value = (left - right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    if (zf) goto label_00129F2D;
    
    g_edx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    left = g_edx;
     right = guest_read32((uint32_t)(g_esi));
     value = (left - right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    if (!zf) goto label_00129F2D;
    
    guest_write32((uint32_t)(g_eax), g_ebx);
    
label_00129F2D:;
    
    left = g_eax;
     right = 0x6Cu;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    left = g_eax;
     right = 0x742510u;
     value = (left - right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    if (sf != of) goto label_00129F20;
    
    g_eax = guest_read32((uint32_t)(g_ebp + 0x1Cu));
    
    left = g_edi;
     right = 1u;
     value = (left + right) & 0xFFFFFFFFu;
     g_edi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     
    left = g_esi;
     right = 0x4u;
     value = (left + right) & 0xFFFFFFFFu;
     g_esi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    left = g_edi;
     right = g_eax;
     value = (left - right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    if (sf != of) goto label_00129EE6;
    
    g_esi = guest_read32(g_esp);
     g_esp += 4u;
    
label_00129F43:;
    
    g_edi = guest_read32(g_esp);
     g_esp += 4u;
    
    g_ebp = guest_read32(g_esp);
     g_esp += 4u;
    
    g_ebx = guest_read32(g_esp);
     g_esp += 4u;
    
    return;
    
}


/* 0x001427d0: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(001427D0, cdecl, 1, u32, game_slot_7a6160_set_flag_0x1750_reset_0x1754)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_eax = guest_read32((uint32_t)(g_eax + 0x4u));
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (sf != of) goto label_00142815;
    
    g_xmm0.q[0] ^= g_xmm0.q[0];
     g_xmm0.q[1] ^= g_xmm0.q[1];
    
    g_eax = g_eax * 0x177Cu;
    
    left = g_eax;
     right = 0x7A6160u;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x1750u));
    
    left = g_ecx;
     right = g_ecx;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (!zf) goto label_00142803;
    
    guest_write32((uint32_t)(g_eax + 0x175Cu), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(g_eax + 0x1760u), g_xmm0.u[0]);
    
label_00142803:;
    
    guest_write32((uint32_t)(g_eax + 0x1750u), 0x1u);
    
    guest_write32((uint32_t)(g_eax + 0x1754u), g_xmm0.u[0]);
    
label_00142815:;
    
    return;
    
}


/* 0x00155040: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00155040, cdecl, 0, u32, game_clear_globals_74984c_to_749884)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    left = g_eax;
     right = g_eax;
     value = (left ^ right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(0x74984Cu), g_eax);
    
    guest_write32((uint32_t)(0x749850u), g_eax);
    
    g_xmm0.q[0] ^= g_xmm0.q[0];
     g_xmm0.q[1] ^= g_xmm0.q[1];
    
    guest_write32((uint32_t)(0x749854u), g_eax);
    
    guest_write32((uint32_t)(0x749858u), g_eax);
    
    guest_write32((uint32_t)(0x74985Cu), g_eax);
    
    guest_write32((uint32_t)(0x749860u), g_eax);
    
    guest_write32((uint32_t)(0x749864u), g_eax);
    
    guest_write32((uint32_t)(0x749868u), g_eax);
    
    guest_write32((uint32_t)(0x74986Cu), g_eax);
    
    guest_write32((uint32_t)(0x749870u), g_eax);
    
    guest_write32((uint32_t)(0x749874u), g_eax);
    
    guest_write32((uint32_t)(0x749878u), g_eax);
    
    guest_write32((uint32_t)(0x74987Cu), g_eax);
    
    guest_write32((uint32_t)(0x749880u), g_eax);
    
    guest_write32((uint32_t)(0x7A3890u), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(0x7A388Cu), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(0x749884u), g_eax);
    
    return;
    
}


/* 0x00155410: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00155410, cdecl, 1, u32, game_set_global_4e7968_reset_74a990)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_xmm0.q[0] ^= g_xmm0.q[0];
     g_xmm0.q[1] ^= g_xmm0.q[1];
    
    guest_write32((uint32_t)(0x4E7968u), g_eax);
    
    guest_write32((uint32_t)(0x74A990u), g_xmm0.u[0]);
    
    return;
    
}

/* 0x001553b0: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(001553B0, cdecl, 1, u32, game_global_74a990_raise_to_clamped_arg)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x4u)));
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(0x475C78u)));
    
    value = vec_compare(g_xmm0.f[0], g_xmm1.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (cf || zf) goto label_001553C6;
    
    g_xmm0 = g_xmm1;
    
label_001553C6:;
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(0x74A990u)));
    
    value = vec_compare(g_xmm1.f[0], g_xmm0.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (!cf && !zf) goto label_001553DB;
    
    guest_write32((uint32_t)(0x74A990u), g_xmm0.u[0]);
    
label_001553DB:;
    
    return;
    
}


/* 0x00166140: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00166140, cdecl, 4, u32, game_entry_74c458_cross_0x14c_with_offset_to_closest_point_on_line_arg)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_edx = guest_read32((uint32_t)(g_esp + 0xCu));
    
    g_ecx = guest_read32((uint32_t)(0x74C458u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_edx + 0x8u)));
    
    left = g_eax;
     right = 0xFFFFu;
     value = (left & right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_eax;
     right = 0x9u;
     value = (left << right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    vec_load_scalar(&g_xmm3, guest_read32((uint32_t)(g_eax + g_ecx + 0x148u)));
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(g_eax + g_ecx + 0x140u)));
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(g_eax + g_ecx + 0x144u)));
    
    vec_load_scalar(&g_xmm4, guest_read32((uint32_t)(g_eax + g_ecx + 0x154u)));
    
    left = g_eax;
     right = g_ecx;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x8u));
    
    g_xmm3.f[0] = vec_sub(g_xmm3.f[0], vec_float(guest_read32((uint32_t)(g_ecx + 0x8u))));
    
    g_xmm2.f[0] = vec_sub(g_xmm2.f[0], vec_float(guest_read32((uint32_t)(g_ecx))));
    
    g_xmm1.f[0] = vec_sub(g_xmm1.f[0], vec_float(guest_read32((uint32_t)(g_ecx + 0x4u))));
    
    g_xmm1.f[0] = vec_mul(g_xmm1.f[0], vec_float(guest_read32((uint32_t)(g_edx + 0x4u))));
    
    g_xmm0.f[0] = vec_mul(g_xmm0.f[0], g_xmm3.f[0]);
    
    vec_load_scalar(&g_xmm3, guest_read32((uint32_t)(g_edx)));
    
    g_xmm3.f[0] = vec_mul(g_xmm3.f[0], g_xmm2.f[0]);
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(g_edx)));
    
    g_xmm0.f[0] = vec_add(g_xmm0.f[0], g_xmm3.f[0]);
    
    vec_load_scalar(&g_xmm3, guest_read32((uint32_t)(g_edx + 0x8u)));
    
    g_xmm0.f[0] = vec_add(g_xmm0.f[0], g_xmm1.f[0]);
    
    g_xmm3.f[0] = vec_mul(g_xmm3.f[0], g_xmm0.f[0]);
    
    g_xmm3.f[0] = vec_add(g_xmm3.f[0], vec_float(guest_read32((uint32_t)(g_ecx + 0x8u))));
    
    g_xmm2.f[0] = vec_mul(g_xmm2.f[0], g_xmm0.f[0]);
    
    g_xmm2.f[0] = vec_add(g_xmm2.f[0], vec_float(guest_read32((uint32_t)(g_ecx))));
    
    g_xmm1 = g_xmm0;
    
    g_xmm1.f[0] = vec_mul(g_xmm1.f[0], vec_float(guest_read32((uint32_t)(g_edx + 0x4u))));
    
    g_xmm1.f[0] = vec_add(g_xmm1.f[0], vec_float(guest_read32((uint32_t)(g_ecx + 0x4u))));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_eax + 0x140u)));
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x10u));
    
    g_xmm0.f[0] = vec_sub(g_xmm0.f[0], g_xmm2.f[0]);
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(g_eax + 0x144u)));
    
    g_xmm2.f[0] = vec_sub(g_xmm2.f[0], g_xmm1.f[0]);
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(g_eax + 0x148u)));
    
    g_xmm1.f[0] = vec_sub(g_xmm1.f[0], g_xmm3.f[0]);
    
    vec_load_scalar(&g_xmm3, guest_read32((uint32_t)(g_eax + 0x150u)));
    
    g_xmm3.f[0] = vec_mul(g_xmm3.f[0], g_xmm1.f[0]);
    
    g_xmm4.f[0] = vec_mul(g_xmm4.f[0], g_xmm2.f[0]);
    
    g_xmm3.f[0] = vec_sub(g_xmm3.f[0], g_xmm4.f[0]);
    
    guest_write32((uint32_t)(g_ecx), g_xmm3.u[0]);
    
    vec_load_scalar(&g_xmm3, guest_read32((uint32_t)(g_eax + 0x154u)));
    
    vec_load_scalar(&g_xmm4, guest_read32((uint32_t)(g_eax + 0x14Cu)));
    
    g_xmm4.f[0] = vec_mul(g_xmm4.f[0], g_xmm1.f[0]);
    
    g_xmm3.f[0] = vec_mul(g_xmm3.f[0], g_xmm0.f[0]);
    
    g_xmm3.f[0] = vec_sub(g_xmm3.f[0], g_xmm4.f[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x4u), g_xmm3.u[0]);
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(g_eax + 0x14Cu)));
    
    g_xmm1.f[0] = vec_mul(g_xmm1.f[0], g_xmm2.f[0]);
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(g_eax + 0x150u)));
    
    g_xmm2.f[0] = vec_mul(g_xmm2.f[0], g_xmm0.f[0]);
    
    g_xmm1.f[0] = vec_sub(g_xmm1.f[0], g_xmm2.f[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x8u), g_xmm1.u[0]);
    
    return;
    
}


