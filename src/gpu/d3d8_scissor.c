/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_scissor.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"
#include <string.h>
#define ENTRY 0x003D4470u
/* Same conservative device exclusion as d3d8_lock.c. CreateDevice clears
 * 0x24A0 bytes; round up the exclusion to 0x2500, including trailing globals. */
#define DEVICE_EXCLUSION_BYTES 0x2500u
static bool overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}
/* Whether a span the routine writes (`begin`, `bytes`) covers an input it reads or the device. */
static bool aliased(uint32_t rectangles, uint32_t begin, uint32_t bytes)
{
    return overlap(rectangles,16u,begin,bytes) || overlap(begin,bytes,D3D8_DEVICE_BASE,DEVICE_EXCLUSION_BYTES) ||
           overlap(begin,bytes,0x475CD4u,4u) || overlap(begin,bytes,0x3E3F58u,4u);
}
static void read_span(uint32_t address, void *data, size_t size)
{
    if (!kernel_guest_read_bytes(address,data,size))
        d3d8_hle_fatal(ENTRY,"scissor input is unreadable or overflows");
}
static void probe(uint32_t address, size_t size)
{
    uint8_t old[36];
    if (!kernel_guest_read_bytes(address,old,size) ||
        !kernel_guest_write_bytes(address,old,size))
        d3d8_hle_fatal(ENTRY,"scissor output is unreadable/unwritable");
}
static uint32_t scaled(uint32_t value, uint32_t scale, uint32_t bias)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t spill,result;
    __asm__ volatile("fildl %2\n\tfmuls %3\n\tfadds %4\n\tfstps %1\n\t"
                     "cvttss2si %1,%0"
                     : "=r"(result),"=m"(spill)
                     : "m"(value),"m"(scale),"m"(bias) : "st");
    return result;
#else
    (void)value;(void)scale;(void)bias;
    d3d8_hle_fatal(ENTRY,"scissor arithmetic requires x86 x87/SSE");
#endif
}
/* T533: the same arithmetic as the viewport rectangle path of 0x003D4470 and 0x003D7B80 run it: `fild`, the unsigned
 * adjustment (`test; jge; fadd [0x475CCC]` when the dword is negative), the scale, the bias, a float store and
 * `cvttss2si`. */
uint32_t d3d8_scaled_integer(uint32_t value, uint32_t scale, uint32_t bias, uint32_t unsigned_adjust)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t spill,result;
    const uint32_t negative = (value & 0x80000000u) != 0u;
    if (negative)
        __asm__ volatile("fildl %2\n\tfadds %5\n\tfmuls %3\n\tfadds %4\n\tfstps %1\n\t"
                         "cvttss2si %1,%0"
                         : "=r"(result),"=m"(spill)
                         : "m"(value),"m"(scale),"m"(bias),"m"(unsigned_adjust) : "st");
    else
        return scaled(value,scale,bias);
    return result;
#else
    (void)value;(void)scale;(void)bias;(void)unsigned_adjust;
    d3d8_hle_fatal(ENTRY,"scissor arithmetic requires x86 x87/SSE");
#endif
}
uint32_t d3d8_set_scissors(uint32_t count, uint32_t exclusive, uint32_t rectangles)
{
    if (count!=1u || exclusive!=0u)
        d3d8_hle_fatal(ENTRY,"only one inclusive scissor rectangle is recovered");
#if defined(__x86_64__) || defined(__i386__)
    uint32_t mxcsr; uint16_t control;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1":"=m"(mxcsr),"=m"(control));
    if ((mxcsr&0xE040u)!=0u || (mxcsr&0x1F80u)!=0x1F80u ||
        (control&0xF00u)!=0x300u || (control&0x3Fu)!=0x3Fu)
        d3d8_hle_fatal(ENTRY,"scissors require masked default host rounding/extended precision");
#else
    d3d8_hle_fatal(ENTRY,"scissor arithmetic requires x86 x87/SSE");
#endif
    uint32_t device; read_span(0x3E3F58u,&device,4u);
    if (device!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(ENTRY,"scissors require the recovered device");
    uint32_t fields[4],rect[4],bias;
    read_span(device+0x954u,fields,sizeof(fields));
    read_span(rectangles,rect,sizeof(rect));
    read_span(0x475CD4u,&bias,4u);
    /* 0x003D4490: ONE limit check at the entry, before the rectangle is read, then the whole 36 bytes at the cursor it
     * leaves (the limit is never tested against the end of the packet, so it may run into the slack). T546: a refill is
     * planned here, so the packet lands after its fence packet, and nothing is written before the plan is checked. */
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    const uint32_t start=d3d8_pushbuffer_sim_site(&sim,ENTRY,36u);
    if (rectangles>UINT32_MAX-16u || overlap(rectangles,16u,device,DEVICE_EXCLUSION_BYTES) ||
        aliased(rectangles,start,36u))
        d3d8_hle_fatal(ENTRY,"scissor input/output aliases are not recovered");
    /* The original reads the rectangle AFTER the refill, so what the refill writes (the jump word at the old cursor and
     * the fence packet) must stay clear of every input as well as of the packet. */
    if (sim.refills!=0u) {
        const d3d8_pushbuffer_refill_spans *spans=&sim.last_refill;
        if ((spans->wraps && aliased(rectangles,spans->at,4u)) ||
            (spans->fence_bytes!=0u && aliased(rectangles,spans->fence_begin,spans->fence_bytes)))
            d3d8_hle_fatal(ENTRY,"scissor input/output aliases are not recovered");
    }
    /* Identical-byte permission probes precede ALL guest publication. Mappings
     * and permissions must remain quiescent until this bounded operation ends. */
    probe(start,36u); probe(device,4u); probe(device+0x1B80u,16u);
    probe(device+0x1C00u,8u);
    if (sim.refills!=0u) {
        if (sim.last_refill.wraps) probe(sim.last_refill.at,4u);
        if (sim.last_refill.fence_bytes!=0u) probe(sim.last_refill.fence_begin,sim.last_refill.fence_bytes);
    }
    const uint32_t left=scaled(rect[0],fields[2],bias);
    const uint32_t top=scaled(rect[1],fields[3],bias);
    const uint32_t right=scaled(rect[2],fields[2],bias);
    const uint32_t bottom=scaled(rect[3],fields[3],bias);
    const uint32_t packet[9]={0x80200u,((right-left)<<16u)|left,
        ((bottom-top)<<16u)|top,0x402B4u,0u,0x402C0u,fields[0]<<16u,
        0x402E0u,fields[1]<<16u};
    const uint32_t cache[2]={1u,0u};
    /* The real roll-over (when the cursor is at the limit) writes the jump word and the fence packet the plan named. */
    if (d3d8_pushbuffer_begin()!=start)
        d3d8_hle_fatal(ENTRY,"scissor plan disagrees with the roll-over");
    if (!kernel_guest_write_bytes(start,packet,sizeof(packet)) ||
        !kernel_guest_write_bytes(device+0x1B80u,rect,sizeof(rect)) ||
        !kernel_guest_write_bytes(device+0x1C00u,cache,sizeof(cache)))
        d3d8_hle_fatal(ENTRY,"scissor mappings changed during publication");
    d3d8_pushbuffer_end(start+36u);
    return 16u;
}
/* --- T533: SetScissors(0, 0, 0), the call SetViewport(NULL) makes (0x003D4033) -------------------------------------
 * Count 0 takes the viewport rectangle (device+0xEE0 to +0xEEC) instead of a caller's: left, top, width and height are
 * each `trunc(float(value * scale + 0.5))` (width and height are NOT turned into right and bottom), then the same 36
 * byte packet follows as for one rectangle, and the cache words at +0x1C00 and +0x1C04 take the count and the
 * exclusive flag (0 and 0), no rectangle copied. One limit check at the entry. The scale words (+0x95C, +0x960) and the
 * clip size (+0x954, +0x958) are INPUTS here because SetViewport(NULL) runs after 0x003D7B80 stored them, and the plan
 * has to run before that store. */
static void viewport_scissor_inputs(uint32_t viewport[4], uint32_t *bias, uint32_t *adjust)
{
    uint32_t device; read_span(0x3E3F58u,&device,4u);
    if (device!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(ENTRY,"scissors require the recovered device");
    read_span(device+0xEE0u,viewport,16u);
    read_span(0x475CD4u,bias,4u);
    read_span(0x475CCCu,adjust,4u);
}
static bool viewport_scissor_aliased(uint32_t begin, uint32_t bytes)
{
    return overlap(begin,bytes,D3D8_DEVICE_BASE,DEVICE_EXCLUSION_BYTES) || overlap(begin,bytes,0x475CD4u,4u) ||
           overlap(begin,bytes,0x475CCCu,4u) || overlap(begin,bytes,0x3E3F58u,4u);
}
void d3d8_check_default_fp(uint32_t entry)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t mxcsr; uint16_t control;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1":"=m"(mxcsr),"=m"(control));
    if ((mxcsr&0xE040u)!=0u || (mxcsr&0x1F80u)!=0x1F80u ||
        (control&0xF00u)!=0x300u || (control&0x3Fu)!=0x3Fu)
        d3d8_hle_fatal(entry,"arithmetic requires masked default host rounding and extended precision");
#else
    d3d8_hle_fatal(entry,"arithmetic requires x86 x87/SSE");
#endif
}
void d3d8_plan_viewport_scissor(d3d8_pushbuffer_sim *sim)
{
    d3d8_check_default_fp(ENTRY);
    uint32_t viewport[4],bias,adjust;
    viewport_scissor_inputs(viewport,&bias,&adjust);
    const uint32_t start=d3d8_pushbuffer_sim_site(sim,ENTRY,36u);
    if (viewport_scissor_aliased(start,36u))
        d3d8_hle_fatal(ENTRY,"scissor input/output aliases are not recovered");
    if (sim->refills!=0u) {
        const d3d8_pushbuffer_refill_spans *spans=&sim->last_refill;
        if ((spans->wraps && viewport_scissor_aliased(spans->at,4u)) ||
            (spans->fence_bytes!=0u && viewport_scissor_aliased(spans->fence_begin,spans->fence_bytes)))
            d3d8_hle_fatal(ENTRY,"scissor input/output aliases are not recovered");
    }
    probe(start,36u); probe(D3D8_DEVICE_BASE,4u); probe(D3D8_DEVICE_BASE+0x1C00u,8u);
    if (sim->refills!=0u) {
        if (sim->last_refill.wraps) probe(sim->last_refill.at,4u);
        if (sim->last_refill.fence_bytes!=0u) probe(sim->last_refill.fence_begin,sim->last_refill.fence_bytes);
    }
}
void d3d8_set_viewport_scissor(const d3d8_viewport_scissor *in)
{
    d3d8_check_default_fp(ENTRY);
    uint32_t viewport[4],bias,adjust;
    viewport_scissor_inputs(viewport,&bias,&adjust);
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    const uint32_t start = d3d8_pushbuffer_sim_site(&sim, ENTRY, 36u);
    const uint32_t left=d3d8_scaled_integer(viewport[0],in->scale_x,bias,adjust);
    const uint32_t top=d3d8_scaled_integer(viewport[1],in->scale_y,bias,adjust);
    const uint32_t width=d3d8_scaled_integer(viewport[2],in->scale_x,bias,adjust);
    const uint32_t height=d3d8_scaled_integer(viewport[3],in->scale_y,bias,adjust);
    const uint32_t packet[9]={0x80200u,(width<<16u)|left,(height<<16u)|top,0x402B4u,0u,0x402C0u,
        in->width<<16u,0x402E0u,in->height<<16u};
    const uint32_t cache[2]={0u,0u};
    if (d3d8_pushbuffer_begin() != start)
        d3d8_hle_fatal(ENTRY,"scissor plan disagrees with the roll-over");
    if (!kernel_guest_write_bytes(start,packet,sizeof(packet)) ||
        !kernel_guest_write_bytes(D3D8_DEVICE_BASE+0x1C00u,cache,sizeof(cache)))
        d3d8_hle_fatal(ENTRY,"scissor mappings changed during publication");
    d3d8_pushbuffer_end(start + 36u);
}
static uint32_t handler(void *context)
{
    uint32_t a[3];
    for(unsigned i=0u;i<3u;i++)
        if(!kernel_frame_arg((const kernel_call_frame *)context,i,&a[i]))
            d3d8_hle_fatal(ENTRY,"scissor argument %u is unreadable",i);
    return d3d8_set_scissors(a[0],a[1],a[2]);
}
size_t d3d8_scissor_register(void)
{
    return (size_t)d3d8_hle_register(ENTRY,handler);
}
