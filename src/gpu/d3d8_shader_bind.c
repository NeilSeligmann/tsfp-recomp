/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_shader_bind.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"
#include <float.h>
#include <stdbool.h>
#include <string.h>
#define DEVICE_GLOBAL 0x003E3F58u
#define DEVICE_BYTES 0x2500u
#define COPY_CACHE 0x003E2C7Cu
static bool overlaps(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{
    return n && m && (uint64_t)a < (uint64_t)b + m && (uint64_t)b < (uint64_t)a + n;
}
static void read_span(uint32_t entry,uint32_t address,void *data,size_t bytes)
{
    if (!kernel_guest_read_bytes(address,data,bytes))
        d3d8_hle_fatal(entry,"shader binder input %#x/%zu is unreadable",address,bytes);
}
static void probe(uint32_t entry,uint32_t address,size_t bytes)
{
    uint8_t old[256];
    if (bytes>sizeof(old) || !kernel_guest_read_bytes(address,old,bytes) ||
        !kernel_guest_write_bytes(address,old,bytes))
        d3d8_hle_fatal(entry,"shader binder output %#x/%zu is not readable/writable",address,bytes);
}
static void float_profile(uint32_t entry)
{
#if defined(__i386__) || defined(__x86_64__)
    uint16_t control; uint32_t mxcsr;
    __asm__ volatile("fnstcw %0\n\tstmxcsr %1":"=m"(control),"=m"(mxcsr));
    if (LDBL_MANT_DIG!=64 || (control&0xF00u)!=0x300u || (control&0x3Fu)!=0x3Fu ||
        (mxcsr&0xFFC0u)!=0x1F80u)
        d3d8_hle_fatal(entry,"shader binder changed mode requires masked x87 extended RN and SSE RN without FTZ/DAZ");
#else
    d3d8_hle_fatal(entry,"shader binder changed mode requires x87-width host arithmetic");
#endif
}
/* Plan one reservation site and return where its commands start (after any roll-over). */
static uint32_t plan_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes)
{
    return d3d8_pushbuffer_sim_site(sim,entry,bytes);
}
void d3d8_shader_bind_prepare(uint32_t entry,uint32_t tagged,uint32_t source,uint32_t words[64])
{
    uint32_t device, state[DEVICE_BYTES/4u], dirty, flags=0u, previous=0u;
    read_span(entry,DEVICE_GLOBAL,&device,4u);
    if (device!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(entry,"shader binder requires the recovered fixed device");
    read_span(entry,device,state,sizeof(state));
    if (source) {
        if ((uint64_t)source+256u>UINT64_C(0x100000000) ||
            overlaps(source,256u,0x003D0000u,0x30000u))
            d3d8_hle_fatal(entry,"shader binder source aliases recovered D3D state");
        read_span(entry,source,words,256u);
    }
    uint32_t old_flags_address=0u,new_flags_address=0u;
    if (tagged) {
        const uint32_t header=tagged-1u, old=state[0x794u/4u];
        if (header>UINT32_MAX-7u || old>UINT32_MAX-7u)
            d3d8_hle_fatal(entry,"shader binder declaration address wraps");
        new_flags_address=header+4u; old_flags_address=old+4u;
        if (source && (overlaps(old_flags_address,4u,COPY_CACHE,256u) ||
                       overlaps(new_flags_address,4u,COPY_CACHE,256u)))
            d3d8_hle_fatal(entry,"shader binder declaration aliases copied cache");
        if (overlaps(old_flags_address,4u,D3D8_GLOBAL_DIRTY_MASK,4u) ||
            overlaps(new_flags_address,4u,D3D8_GLOBAL_DIRTY_MASK,4u) ||
            overlaps(old_flags_address,4u,DEVICE_GLOBAL,4u) ||
            overlaps(new_flags_address,4u,DEVICE_GLOBAL,4u))
            d3d8_hle_fatal(entry,"shader declaration aliases publication globals");
        read_span(entry,new_flags_address,&flags,4u);
        read_span(entry,old_flags_address,&previous,4u);
        read_span(entry,D3D8_GLOBAL_DIRTY_MASK,&dirty,4u);
    }
    const bool changed=tagged && ((flags^previous)&0x13u);
    const bool programmable=(flags&0x12u)!=0u;
    uint32_t mode_bytes=0u;
    if (changed) {
        float_profile(entry);
        uint32_t mode,half_pixel;
        read_span(entry,0x003E3EFCu,&mode,4u);
        read_span(entry,0x003E3F20u,&half_pixel,4u);
        (void)mode; (void)half_pixel;
        mode_bytes=programmable ? ((state[D3D8_DEV_FLAGS/4u]&0x200u) ? 24u : 64u) : 44u;
    }
    /* 0x003D5A50 has two reservation preambles: one before the mode-change packets (0x003D7860 and
     * the 12-byte 0x81E94 write, which have none of their own) when the shader class changed, and
     * one before the 8-byte handle write. Each may roll the ring over, so the command spans are
     * the planned sites, not one run from the entry cursor. */
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    sim.cursor=state[0]; sim.limit=state[1];
    uint32_t span_start[2]={0u,0u}, span_bytes[2]={0u,0u};
    unsigned spans=0u;
    if (changed) {
        span_start[spans]=plan_site(&sim,entry,mode_bytes);
        span_bytes[spans++]=mode_bytes;
    }
    span_start[spans]=plan_site(&sim,entry,8u);
    span_bytes[spans++]=8u;
    for (unsigned span=0u;span<spans;span++) {
        const uint32_t cursor=span_start[span], bytes=span_bytes[span];
        if (overlaps(cursor,bytes,0x003D0000u,0x30000u) ||
            overlaps(cursor,bytes,source,source ? 256u : 0u) ||
            overlaps(cursor,bytes,old_flags_address,tagged ? 4u : 0u) ||
            overlaps(cursor,bytes,new_flags_address,tagged ? 4u : 0u))
            d3d8_hle_fatal(entry,"shader binder source/declaration/command/state aliases are unsupported");
    }
    if ((tagged && (overlaps(old_flags_address,4u,device,DEVICE_BYTES) ||
                    overlaps(new_flags_address,4u,device,DEVICE_BYTES))) ||
        (source && (overlaps(source,256u,old_flags_address,tagged ? 4u : 0u) ||
                    overlaps(source,256u,new_flags_address,tagged ? 4u : 0u))))
        d3d8_hle_fatal(entry,"shader binder source/declaration/command/state aliases are unsupported");
    /* Every fallible permission check precedes copy/cache/dirty/stream writes.
     * Identical-byte probes do not promise safety against concurrent remapping. */
    if (source) probe(entry,COPY_CACHE,256u);
    for (unsigned span=0u;span<spans;span++) probe(entry,span_start[span],span_bytes[span]);
    probe(entry,device,4u);
    probe(entry,device+0x79Cu,4u);
    if (tagged) {
        probe(entry,device+0x794u,8u);
        probe(entry,D3D8_GLOBAL_DIRTY_MASK,4u);
    }
    if (changed && !(state[0x1C18u/4u] & (programmable ? 2u : 1u)))
        probe(entry,device+(programmable ? 0x1C10u : 0x1C08u),8u);
}
