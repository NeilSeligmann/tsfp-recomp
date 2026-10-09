/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_viewport_matrix.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "kernel_call.h"
#include <stdbool.h>
#include <string.h>

static void check_platform(uint32_t entry)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t mxcsr;uint16_t control;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1":"=m"(mxcsr),"=m"(control));
    if((mxcsr&0xE040u)!=0u || (mxcsr&0x1F80u)!=0x1F80u ||
       (control&0xF00u)!=0x300u || (control&0x3Fu)!=0x3Fu)
        d3d8_hle_fatal(entry,"matrix arithmetic requires default rounding and extended x87 precision");
#else
    d3d8_hle_fatal(entry,"exact matrix arithmetic requires x86 SSE/x87");
#endif
}
static void read_span(uint32_t entry,uint32_t address,void *out,size_t bytes)
{
    if(!kernel_guest_read_bytes(address,out,bytes))
        d3d8_hle_fatal(entry,"matrix input span is unreadable or overflows");
}
static void output_span(uint32_t entry,uint32_t address)
{
    /* Guest mappings must remain quiescent through this internal operation. */
    if((address&15u)!=0u || (address&0xFFFu)>0xFC0u || kernel_guest_at(address,64u)==NULL)
        d3d8_hle_fatal(entry,"matrix output is unaligned or unmapped");
}
static void reject_nan(uint32_t entry,const uint32_t *words,unsigned count)
{
    for(unsigned i=0u;i<count;i++)if((words[i]&0x7FFFFFFFu)>0x7F800000u)
        d3d8_hle_fatal(entry,"NaN input payload behavior differs in the original-byte oracle");
}
static void multiply(const uint32_t left[16],const uint32_t right[16],uint32_t output[16])
{
#if defined(__x86_64__) || defined(__i386__)
    for(unsigned row=0u;row<4u;row++) {
        __asm__ volatile(
            "movups (%1), %%xmm2\n\tshufps $0, %%xmm2, %%xmm2\n\tmulps (%2), %%xmm2\n\t"
            "movups (%1), %%xmm1\n\tshufps $85, %%xmm1, %%xmm1\n\tmulps 16(%2), %%xmm1\n\t"
            "movups (%1), %%xmm0\n\tshufps $170, %%xmm0, %%xmm0\n\tmulps 32(%2), %%xmm0\n\t"
            "addps %%xmm1, %%xmm2\n\t"
            "movups (%1), %%xmm1\n\tshufps $255, %%xmm1, %%xmm1\n\tmulps 48(%2), %%xmm1\n\t"
            "addps %%xmm0, %%xmm2\n\taddps %%xmm1, %%xmm2\n\tmovups %%xmm2, (%0)"
            : : "r"(output+row*4u),"r"(left+row*4u),"r"(right) : "xmm0","xmm1","xmm2","memory");
    }
#else
    (void)left;(void)right;(void)output;
    d3d8_hle_fatal(0x003D9B30u,"exact matrix arithmetic requires x86 SSE/x87");
#endif
}
uint32_t d3d8_multiply_matrix(uint32_t destination,uint32_t left,uint32_t right)
{
    check_platform(0x003D9B30u);
    uint32_t a[16] __attribute__((aligned(16))),b[16] __attribute__((aligned(16))),out[16];
    if((left&15u)!=0u || (right&15u)!=0u)
        d3d8_hle_fatal(0x003D9B30u,"original MOVAPS matrix inputs require alignment");
    output_span(0x003D9B30u,destination);
    read_span(0x003D9B30u,left,a,sizeof(a));read_span(0x003D9B30u,right,b,sizeof(b));
    reject_nan(0x003D9B30u,a,16u);reject_nan(0x003D9B30u,b,16u);
    multiply(a,b,out);
    if(!kernel_guest_write_bytes(destination,out,sizeof(out)))
        d3d8_hle_fatal(0x003D9B30u,"matrix output is unwritable");
    return left;
}
static uint32_t rebuild(bool apply, bool override_scales, uint32_t scale_x, uint32_t scale_y)
{
#if defined(__x86_64__) || defined(__i386__)
    check_platform(0x003D6E50u);
    uint8_t device[0xF00];uint32_t constants[4];uint32_t temporary[24] __attribute__((aligned(16)))={0};uint32_t left[16] __attribute__((aligned(16))),out[16];
    if(d3d8_guest_load32(0x003E3F58u)!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(0x003D6E50u,"viewport matrix requires fixed device");
    read_span(0x003D6E50u,D3D8_DEVICE_BASE,device,sizeof(device));
    /* T533: a caller that has not stored the scales yet plans with the words it is about to store. */
    if(override_scales){memcpy(device+0x95Cu,&scale_x,4u);memcpy(device+0x960u,&scale_y,4u);}
    const uint32_t addresses[]={0x475CCCu,0x475CD4u,0x4760C4u,0x475C78u};
    for(unsigned i=0;i<4;i++)read_span(0x003D6E50u,addresses[i],constants+i,4u);
    const uint32_t mode=d3d8_guest_load32(0x003E3EFCu);
    const uint32_t dirty=d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    output_span(0x003D6E50u,D3D8_DEVICE_BASE+0x980u);
    read_span(0x003D6E50u,D3D8_DEVICE_BASE+0xCA0u,left,64u);
    reject_nan(0x003D6E50u,left,16u);reject_nan(0x003D6E50u,constants,4u);
    const unsigned float_offsets[]={0x95Cu,0x960u,0x944u,0x948u,0xEF0u,0xEF4u};
    for(unsigned i=0u;i<6u;i++){uint32_t word;memcpy(&word,device+float_offsets[i],4u);reject_nan(0x003D6E50u,&word,1u);}
    if(!apply) {
        /* The read-only twin: every refusal above, plus the identical-byte write probes below. */
        uint32_t current[16];
        read_span(0x003D6E50u,D3D8_DEVICE_BASE+0x980u,current,sizeof(current));
        if(!kernel_guest_write_u32(D3D8_GLOBAL_DIRTY_MASK,dirty) ||
           !kernel_guest_write_bytes(D3D8_DEVICE_BASE+0x980u,current,sizeof(current)))
            d3d8_hle_fatal(0x003D6E50u,"viewport matrix output is unwritable");
        return 0u;
    }
    __asm__ volatile(
        "fildl 0xee8(%[d])\n\tcmpl $0,0xee8(%[d])\n\tjge 1f\n\tfadds 0(%[c])\n1:\n\t"
        "fmuls 0x95c(%[d])\n\tfmuls 4(%[c])\n\tfsts 0x14(%[t])\n\t"
        "fildl 0xeec(%[d])\n\tcmpl $0,0xeec(%[d])\n\tjge 2f\n\tfadds 0(%[c])\n2:\n\t"
        "fmuls 0x960(%[d])\n\tfmuls 8(%[c])\n\tfsts 0xc(%[t])\n\tfchs\n\tfstps 0x18(%[t])\n\t"
        "flds 0xef4(%[d])\n\tfsubs 0xef0(%[d])\n\tfmuls 0x948(%[d])\n\tfstps 0x10(%[t])\n\t"
        "flds 0x948(%[d])\n\tfmuls 0xef0(%[d])\n\tfstps 0x1c(%[t])\n\t"
        "cmpl $2,%[mode]\n\tjne 3f\n\t"
        "flds 0x944(%[d])\n\tfmuls 0x948(%[d])\n\tfld %%st(0)\n\tfmulp %%st,%%st(2)\n\t"
        "flds 0xc(%[t])\n\tfmul %%st(1),%%st\n\t"
        "fld %%st(1)\n\tfmuls 0x10(%[t])\n\tfstps 0x10(%[t])\n\t"
        "fld %%st(1)\n\tfmuls 0x14(%[t])\n\tfstps 0x14(%[t])\n\t"
        "fld %%st(1)\n\tfmuls 0x18(%[t])\n\tfstps 0x18(%[t])\n\t"
        "fld %%st(1)\n\tfmuls 0x1c(%[t])\n\tfstps 0x1c(%[t])\n\tjmp 4f\n3:\n\t"
        "flds 12(%[c])\n\tflds 0xc(%[t])\n4:\n\t"
        "fxch %%st(2)\n\tfstps 0x20(%[t])\n\tfxch %%st(1)\n\tfstps 0x34(%[t])\n\tfstps 0x5c(%[t])"
        : : [d]"r"(device),[c]"r"(constants),[t]"r"(temporary),[mode]"r"(mode)
        : "cc","memory","st","st(1)","st(2)","st(3)");
    temporary[0x48/4]=temporary[0x10/4];temporary[0x50/4]=temporary[0x14/4];
    temporary[0x54/4]=temporary[0x18/4];temporary[0x58/4]=temporary[0x1c/4];
    multiply(left,temporary+8,out);
    /* Probe the aligned one-page dirty word with identical bytes before cache
     * publication. With quiescent mappings this establishes both write domains
     * without changing guest state; protected pages fail without a prefix. */
    if(!kernel_guest_write_u32(D3D8_GLOBAL_DIRTY_MASK,dirty))
        d3d8_hle_fatal(0x003D6E50u,"viewport dirty word is unwritable");
    if(!kernel_guest_write_bytes(D3D8_DEVICE_BASE+0x980u,out,64u))
        d3d8_hle_fatal(0x003D6E50u,"viewport matrix output is unwritable");
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK,dirty|0x200u);
    return D3D8_DEVICE_BASE+0xCA0u;
#else
    (void)apply;(void)override_scales;(void)scale_x;(void)scale_y;
    d3d8_hle_fatal(0x003D6E50u,"exact viewport matrix requires x86 SSE/x87");
#endif
}
uint32_t d3d8_rebuild_viewport_matrix(void)
{
    return rebuild(true,false,0u,0u);
}
void d3d8_rebuild_viewport_matrix_check(void)
{
    (void)rebuild(false,false,0u,0u);
}
void d3d8_rebuild_viewport_matrix_check_scaled(uint32_t scale_x,uint32_t scale_y)
{
    (void)rebuild(false,true,scale_x,scale_y);
}
