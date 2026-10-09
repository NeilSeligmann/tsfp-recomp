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


/* 0x0010ea50: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0010EA50, cdecl, 2, u32, game_object_set_field_0x60_mark_dirty_0x400)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (zf) goto label_0010EA6F;
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x8u)));
    
    left = g_ecx;
     right = 0x400u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x60u), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
label_0010EA6F:;
    
    return;
    
}


/* 0x0010ea90: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0010EA90, cdecl, 2, u32, game_object_set_field_0x5c_mark_dirty_0x400)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (zf) goto label_0010EAAF;
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x8u)));
    
    left = g_ecx;
     right = 0x400u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x5Cu), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
label_0010EAAF:;
    
    return;
    
}


/* 0x0010eb10: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0010EB10, cdecl, 3, u32, game_object_set_fields_0x68_0x6c)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (zf) goto label_0010EB2E;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x8u)));
    
    guest_write32((uint32_t)(g_eax + 0x68u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0xCu)));
    
    guest_write32((uint32_t)(g_eax + 0x6Cu), g_xmm0.u[0]);
    
label_0010EB2E:;
    
    return;
    
}


/* 0x0010eb90: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0010EB90, cdecl, 2, u32, game_object_set_field_0x74)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (zf) goto label_0010EBA3;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x8u)));
    
    guest_write32((uint32_t)(g_eax + 0x74u), g_xmm0.u[0]);
    
label_0010EBA3:;
    
    return;
    
}


/* 0x0011f510: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0011F510, cdecl, 1, u32, game_object_set_field_0xa8_default_mark_dirty_0x400)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(0x47CFF4u)));
    
    left = g_ecx;
     right = 0x400u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
    guest_write32((uint32_t)(g_eax + 0xA8u), g_xmm0.u[0]);
    
    return;
    
}


/* 0x0011f570: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0011F570, cdecl, 1, u32, game_object_set_field_0xb0_default_mark_dirty_0x800)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(0x47CFF4u)));
    
    left = g_ecx;
     right = 0x800u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
    guest_write32((uint32_t)(g_eax + 0xB0u), g_xmm0.u[0]);
    
    return;
    
}


/* 0x00126ac0: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00126AC0, cdecl, 12, u32, game_ring_73ed18_push_entry_stride_0x38)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_ecx = guest_read32((uint32_t)(0x73ED10u));
    
    g_eax = g_ecx;
    
    g_eax = g_eax * 0x38u;
    
    left = g_eax;
     right = 0x73ED18u;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    left = g_ecx;
     right = 1u;
     value = (left + right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     
    left = g_ecx;
     right = 0xF0u;
     value = (left - right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    guest_write32((uint32_t)(0x73ED10u), g_ecx);
    
    if (!zf) goto label_00126AE9;
    
    guest_write32((uint32_t)(0x73ED10u), 0x0u);
    
label_00126AE9:;
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x4u));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(0x742198u)));
    
    g_edx = guest_read32((uint32_t)(g_esp + 0x8u));
    
    guest_write32((uint32_t)(g_eax + 0x8u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x2Cu)));
    
    guest_write32((uint32_t)(g_eax + 0xCu), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0xCu)));
    
    value = g_esi;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    guest_write32((uint32_t)(g_eax), g_edx);
    
    guest_write32((uint32_t)(g_eax + 0x10u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x14u)));
    
    guest_write32((uint32_t)(g_eax + 0x4u), g_ecx);
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x34u));
    
    guest_write32((uint32_t)(g_eax + 0x14u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x18u)));
    
    g_edx = g_ecx;
    
    g_esi = g_ecx;
    
    left = g_esi;
     right = 0x10u;
     value = (left << right) & 0xFFFFFFFFu;
     g_esi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_edx;
     right = 0xFF00u;
     value = (left & right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_edx;
     right = g_esi;
     value = (left | right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x18u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x1Cu)));
    
    guest_write32((uint32_t)(g_eax + 0x20u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x20u)));
    
    g_esi = g_ecx;
    
    left = g_esi;
     right = 0xFF0000u;
     value = (left & right) & 0xFFFFFFFFu;
     g_esi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x24u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x24u)));
    
    left = g_ecx;
     right = 0x10u;
     value = (left >> right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_esi;
     right = g_ecx;
     value = (left | right) & 0xFFFFFFFFu;
     g_esi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x28u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x28u)));
    
    left = g_esi;
     right = 0x8u;
     value = (left >> right) & 0xFFFFFFFFu;
     g_esi = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_edx;
     right = 0x8u;
     value = (left << right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    left = g_edx;
     right = g_esi;
     value = (left | right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax + 0x2Cu), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x2Cu)));
    
    guest_write32((uint32_t)(g_eax + 0x30u), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(g_eax + 0x34u), g_edx);
    
    g_esi = guest_read32(g_esp);
     g_esp += 4u;
    
    return;
    
}


/* 0x0011fdc0: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0011FDC0, cdecl, 3, u32, game_effect_set_ratio_0x98_when_in_unit_range_else_clear_flags_0x20002)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x8u)));
    
    g_xmm1.q[0] ^= g_xmm1.q[0];
     g_xmm1.q[1] ^= g_xmm1.q[1];
    
    value = vec_compare(g_xmm0.f[0], g_xmm1.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (cf || zf) goto label_0011FE08;
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(0x475C78u)));
    
    value = vec_compare(g_xmm2.f[0], g_xmm0.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (cf) goto label_0011FE08;
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_edx = guest_read32((uint32_t)(g_ecx + 0x4u));
    
    g_eax = (uint32_t)(g_ecx + 0x4u);
    
    left = g_edx;
     right = 0x2u;
     value = (left | right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax), g_edx);
    
    g_edx = guest_read32((uint32_t)(g_esp + 0xCu));
    
    guest_write32((uint32_t)(g_ecx + 0x88u), 0x0u);
    
    guest_write32((uint32_t)(g_ecx + 0x98u), g_xmm0.u[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x8Cu), g_edx);
    
    goto label_0011FE2C;
    
label_0011FE08:;
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_edx = guest_read32((uint32_t)(g_ecx + 0x4u));
    
    g_eax = (uint32_t)(g_ecx + 0x4u);
    
    left = g_edx;
     right = 0xFFFDFFFDu;
     value = (left & right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax), g_edx);
    
    guest_write32((uint32_t)(g_ecx + 0x98u), g_xmm1.u[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x8Cu), 0x0u);
    
label_0011FE2C:;
    
    g_ecx = guest_read32((uint32_t)(g_eax));
    
    left = ((g_ecx >> 0) & 255u);
     right = 0x2u;
     value = (left & right) & 0xFFu;
     zf = value == 0u;
     sf = (value >> 7) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (zf) goto label_0011FE3B;
    
    left = g_ecx;
     right = 0x20000u;
     value = (left | right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    guest_write32((uint32_t)(g_eax), g_ecx);
    
label_0011FE3B:;
    
    return;
    
}


/* 0x00128e90: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00128E90, cdecl, 2, u32, game_list_remove_entries_stride_0x34_field_0x2c_gt_arg)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    value = g_ebx;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    g_ebx = guest_read32((uint32_t)(g_esp + 0x8u));
    
    g_eax = guest_read32((uint32_t)(g_ebx));
    
    left = g_eax;
     right = 1u;
     value = (left - right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     
    if (sf) goto label_00128EE2;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0xCu)));
    
    g_ecx = g_eax;
    
    g_ecx = g_ecx * 0x34u;
    
    value = g_ebp;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    value = g_esi;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    g_edx = (uint32_t)(g_ecx + g_ebx + 0x3Cu);
    
    value = g_edi;
     g_esp -= 4u;
     guest_write32(g_esp, value);
    
    g_esp = (uint32_t)(g_esp);
    
label_00128EB0:;
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(g_edx + 0xFFFFFFF4u)));
    
    value = vec_compare(g_xmm1.f[0], g_xmm0.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (cf || zf) goto label_00128ED7;
    
    g_ecx = guest_read32((uint32_t)(g_ebx));
    
    left = g_ecx;
     right = g_eax;
     value = (left - right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    left = g_ecx;
     right = 1u;
     value = (left - right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     
    g_ecx = g_ecx * 0x34u;
    
    g_ebp = g_ecx;
    
    left = g_ecx;
     right = 0x2u;
     value = (left >> right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    g_esi = g_edx;
    
    g_edi = (uint32_t)(g_edx + 0xFFFFFFCCu);
    
    while (g_ecx != 0u) { value = guest_read32(g_esi);
     guest_write32(g_edi, (uint32_t)value);
     g_esi += g_df ? (uint32_t)-4 : 4u;
     g_edi += g_df ? (uint32_t)-4 : 4u;
     g_ecx--;
     }
    g_ecx = g_ebp;
    
    left = g_ecx;
     right = 0x3u;
     value = (left & right) & 0xFFFFFFFFu;
     g_ecx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    while (g_ecx != 0u) { value = guest_read8(g_esi);
     guest_write8(g_edi, (uint8_t)value);
     g_esi += g_df ? (uint32_t)-1 : 1u;
     g_edi += g_df ? (uint32_t)-1 : 1u;
     g_ecx--;
     }
    left = guest_read32((uint32_t)(g_ebx));
     right = 1u;
     value = (left - right) & 0xFFFFFFFFu;
     guest_write32((uint32_t)(g_ebx), value);
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     
label_00128ED7:;
    
    left = g_eax;
     right = 1u;
     value = (left - right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     
    left = g_edx;
     right = 0x34u;
     value = (left - right) & 0xFFFFFFFFu;
     g_edx = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = left < right;
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (sf == of) goto label_00128EB0;
    
    g_edi = guest_read32(g_esp);
     g_esp += 4u;
    
    g_esi = guest_read32(g_esp);
     g_esp += 4u;
    
    g_ebp = guest_read32(g_esp);
     g_esp += 4u;
    
label_00128EE2:;
    
    g_ebx = guest_read32(g_esp);
     g_esp += 4u;
    
    return;
    
}


/* 0x0012b8e0: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(0012B8E0, cdecl, 1, u32, game_ring_742510_set_prev_entry_field_0x28_clamped)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(0x7450D0u));
    
    left = g_eax;
     right = 1u;
     value = (left - right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = (((left ^ right) & (left ^ value)) >> 31) & 1u;
     
    if (!sf) goto label_0012B8ED;
    
    left = g_eax;
     right = 0xC8u;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
label_0012B8ED:;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_esp + 0x4u)));
    
    g_eax = g_eax * 0x38u;
    
    g_xmm0.f[0] = vec_mul(g_xmm0.f[0], vec_float(guest_read32((uint32_t)(0x475D24u))));
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(0x475D1Cu)));
    
    left = g_eax;
     right = 0x742510u;
     value = (left + right) & 0xFFFFFFFFu;
     g_eax = value;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = ((~(left ^ right) & (left ^ value)) >> 31) & 1u;
     cf = value < left;
    
    value = vec_compare(g_xmm0.f[0], g_xmm2.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (!cf && !zf) goto label_0012B923;
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(0x4785D8u)));
    
    value = vec_compare(g_xmm1.f[0], g_xmm0.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (cf || zf) goto label_0012B923;
    
    guest_write32((uint32_t)(g_eax + 0x28u), g_xmm1.u[0]);
    
    return;
    
label_0012B923:;
    
    value = vec_compare(g_xmm0.f[0], g_xmm2.f[0], 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (cf || zf) goto label_0012B92E;
    
    guest_write32((uint32_t)(g_eax + 0x28u), g_xmm2.u[0]);
    
    return;
    
label_0012B92E:;
    
    guest_write32((uint32_t)(g_eax + 0x28u), g_xmm0.u[0]);
    
    return;
    
}


/* 0x00166060: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00166060, cdecl, 3, u32, game_entry_74c458_point_velocity_cross_0x134_with_offset_from_0x140_plus_vec_0x14c)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_eax = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_ecx = guest_read32((uint32_t)(0x74C458u));
    
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
    
    vec_load_scalar(&g_xmm3, guest_read32((uint32_t)(g_eax + g_ecx + 0x138u)));
    
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
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(g_ecx + 0x4u)));
    
    g_xmm1.f[0] = vec_sub(g_xmm1.f[0], vec_float(guest_read32((uint32_t)(g_eax + 0x144u))));
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(g_ecx + 0x8u)));
    
    g_xmm2.f[0] = vec_sub(g_xmm2.f[0], vec_float(guest_read32((uint32_t)(g_eax + 0x148u))));
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_ecx)));
    
    g_xmm0.f[0] = vec_sub(g_xmm0.f[0], vec_float(guest_read32((uint32_t)(g_eax + 0x140u))));
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0xCu));
    
    g_xmm3.f[0] = vec_mul(g_xmm3.f[0], g_xmm2.f[0]);
    
    g_xmm4 = g_xmm1;
    
    g_xmm4.f[0] = vec_mul(g_xmm4.f[0], vec_float(guest_read32((uint32_t)(g_eax + 0x13Cu))));
    
    g_xmm3.f[0] = vec_sub(g_xmm3.f[0], g_xmm4.f[0]);
    
    guest_write32((uint32_t)(g_ecx), g_xmm3.u[0]);
    
    vec_load_scalar(&g_xmm4, guest_read32((uint32_t)(g_eax + 0x134u)));
    
    g_xmm3 = g_xmm0;
    
    g_xmm3.f[0] = vec_mul(g_xmm3.f[0], vec_float(guest_read32((uint32_t)(g_eax + 0x13Cu))));
    
    g_xmm4.f[0] = vec_mul(g_xmm4.f[0], g_xmm2.f[0]);
    
    g_xmm3.f[0] = vec_sub(g_xmm3.f[0], g_xmm4.f[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x4u), g_xmm3.u[0]);
    
    vec_load_scalar(&g_xmm2, guest_read32((uint32_t)(g_eax + 0x134u)));
    
    g_xmm2.f[0] = vec_mul(g_xmm2.f[0], g_xmm1.f[0]);
    
    vec_load_scalar(&g_xmm1, guest_read32((uint32_t)(g_eax + 0x138u)));
    
    g_xmm1.f[0] = vec_mul(g_xmm1.f[0], g_xmm0.f[0]);
    
    g_xmm2.f[0] = vec_sub(g_xmm2.f[0], g_xmm1.f[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x8u), g_xmm2.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_eax + 0x14Cu)));
    
    g_xmm0.f[0] = vec_add(g_xmm0.f[0], vec_float(guest_read32((uint32_t)(g_ecx))));
    
    guest_write32((uint32_t)(g_ecx), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_eax + 0x150u)));
    
    g_xmm0.f[0] = vec_add(g_xmm0.f[0], g_xmm3.f[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x4u), g_xmm0.u[0]);
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_eax + 0x154u)));
    
    g_xmm0.f[0] = vec_add(g_xmm0.f[0], g_xmm2.f[0]);
    
    guest_write32((uint32_t)(g_ecx + 0x8u), g_xmm0.u[0]);
    
    return;
    
}


/* 0x00166770: original-order reads/stores, exact GPRs and XMM lanes. */
GAME_REPLACE_EXACT(00166770, cdecl, 2, u32, game_sorted_list_insert_by_field_0x1c_next_0x34)
{
    uint32_t value __attribute__((unused)), left __attribute__((unused)), right __attribute__((unused));
    
    uint32_t cf __attribute__((unused)) = 0, zf __attribute__((unused)) = 0, sf __attribute__((unused)) = 0, of __attribute__((unused)) = 0, pf __attribute__((unused)) = 0;
    
    g_ecx = guest_read32((uint32_t)(g_esp + 0x4u));
    
    g_eax = guest_read32((uint32_t)(g_ecx));
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    g_edx = guest_read32((uint32_t)(g_esp + 0x8u));
    
    if (zf) goto label_001667B3;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_eax + 0x1Cu)));
    
    value = vec_compare(g_xmm0.f[0], vec_float(guest_read32((uint32_t)(g_edx + 0x1Cu))), 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (!cf && !zf) goto label_001667B3;
    
    g_esp = (uint32_t)(g_esp);
    
label_00166790:;
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x34u));
    
    left = g_ecx;
     right = g_ecx;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (zf) goto label_001667A9;
    
    vec_load_scalar(&g_xmm0, guest_read32((uint32_t)(g_ecx + 0x1Cu)));
    
    value = vec_compare(g_xmm0.f[0], vec_float(guest_read32((uint32_t)(g_edx + 0x1Cu))), 0);
     cf = value & 1u;
     pf = (value >> 2) & 1u;
     zf = (value >> 6) & 1u;
     sf = of = 0;
    
    if (!cf && !zf) goto label_001667A9;
    
    g_eax = g_ecx;
    
    left = g_eax;
     right = g_eax;
     value = (left & right) & 0xFFFFFFFFu;
     zf = value == 0u;
     sf = (value >> 31) & 1u;
     pf = vec_parity(value);
     of = 0;
     cf = 0;
    
    if (!zf) goto label_00166790;
    
    return;
    
label_001667A9:;
    
    g_ecx = guest_read32((uint32_t)(g_eax + 0x34u));
    
    guest_write32((uint32_t)(g_edx + 0x34u), g_ecx);
    
    guest_write32((uint32_t)(g_eax + 0x34u), g_edx);
    
    return;
    
label_001667B3:;
    
    guest_write32((uint32_t)(g_ecx), g_edx);
    
    guest_write32((uint32_t)(g_edx + 0x34u), g_eax);
    
    return;
    
}

