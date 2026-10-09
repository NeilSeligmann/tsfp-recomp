/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_pixel_bind.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"
#include <string.h>
#define DEVICE_GLOBAL 0x003E3F58u
#define INTERNAL_WRAPPER (D3D8_DEVICE_BASE+0x924u)
#define FACTOR 0x003E3F10u
static bool overlaps(uint32_t a,uint32_t n,uint32_t b,uint32_t m)
{
    return n && m && (uint64_t)a<(uint64_t)b+m && (uint64_t)b<(uint64_t)a+n;
}
static void read_span(uint32_t entry,uint32_t address,void *out,size_t bytes)
{
    if (!kernel_guest_read_bytes(address,out,bytes))
        d3d8_hle_fatal(entry,"pixel binder input %#x/%zu is unreadable",address,bytes);
}
static void probe(uint32_t entry,uint32_t address,size_t bytes)
{
    uint8_t old[336];
    if (bytes>sizeof(old) || !kernel_guest_read_bytes(address,old,bytes) ||
        !kernel_guest_write_bytes(address,old,bytes))
        d3d8_hle_fatal(entry,"pixel binder output %#x/%zu is not readable/writable",address,bytes);
}
void d3d8_pixel_bind_prepare(uint32_t entry,uint32_t argument,bool definition_api,
                             d3d8_pixel_bind_plan *plan)
{
    memset(plan,0,sizeof(*plan));
    uint32_t device,state[0x2500u/4u],dirty;
    read_span(entry,DEVICE_GLOBAL,&device,4u);
    if (device!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(entry,"pixel binder requires the recovered fixed device");
    read_span(entry,device,state,sizeof(state));
    read_span(entry,D3D8_GLOBAL_DIRTY_MASK,&dirty,4u);
    plan->previous=state[0x784u/4u];
    if (argument) {
        plan->wrapper=definition_api ? INTERNAL_WRAPPER : argument;
        if (definition_api) plan->definition=argument;
        else {
            if (argument>UINT32_MAX-11u ||
                (argument!=INTERNAL_WRAPPER && overlaps(argument,12u,0x3D0000u,0x30000u)))
                d3d8_hle_fatal(entry,"pixel wrapper wraps or aliases recovered D3D state");
            read_span(entry,argument+8u,&plan->definition,4u);
        }
        if ((uint64_t)plan->definition+240u>UINT64_C(0x100000000) ||
            overlaps(plan->definition,240u,0x3D0000u,0x30000u) ||
            overlaps(plan->definition,240u,plan->wrapper,12u))
            d3d8_hle_fatal(entry,"pixel definition wraps or aliases shader state/wrapper");
        read_span(entry,plan->definition,plan->words,sizeof(plan->words));
    } else read_span(entry,FACTOR,&plan->factor,4u);
    const uint32_t inputs=plan->words[8]|plan->words[9];
    const bool rows_needed=!argument || plan->previous==0u;
    if (rows_needed) {
        const uint32_t first=argument ? 1u : 0u;
        for (unsigned row=0;row<3;row++)
            read_span(entry,0x3E3B18u+(row+first)*0x80u,plan->rows+row*6u,24u);
    }
    /* The sites the binder writes, each behind its own check of the limit: a shader is one site (0x003D93F5, before
     * the shadow copy and everything it writes, so the rows when binding from the null shader, the blocks and the
     * input packet are all one run), the null shader is two (0x003D7150's factor restore of 68 bytes, then
     * 0x003D9374 before the rows and the other-stage word, 92 bytes). A site that finds the cursor at the limit
     * refills there (T525), planned over a writer that carries what the first refill moves (T526). */
    uint32_t site_bytes[2];
    unsigned sites=0u;
    if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+(rows_needed ? 84u : 0u);
    else { site_bytes[sites++]=68u; site_bytes[sites++]=92u; }
    const uint32_t ring_base=state[D3D8_DEV_PB_BASE/4u];
    const uint32_t ring_end=state[D3D8_DEV_PB_END/4u];
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    uint32_t spans[3u*2u][2],span_count=0u;
    for (unsigned site=0u;site<sites;site++) {
        const uint32_t refills=sim.refills;
        const uint32_t start=d3d8_pushbuffer_sim_site(&sim,entry,site_bytes[site]);
        plan->site[site]=start;
        spans[span_count][0]=start; spans[span_count++][1]=site_bytes[site];
        if (sim.refills!=refills) {
            /* What the refill writes besides the commands: the ring jump word and its fence packet. */
            spans[span_count][0]=sim.last_refill.at; spans[span_count++][1]=sim.last_refill.wraps ? 4u : 0u;
            spans[span_count][0]=sim.last_refill.fence_begin; spans[span_count++][1]=sim.last_refill.fence_bytes;
        }
        if (ring_end && ((uint64_t)start+site_bytes[site]>ring_end || start<ring_base || ring_end<=ring_base))
            d3d8_hle_fatal(entry,"pixel binder command bounds or aliases are unsupported");
    }
    for (uint32_t span=0u;span<span_count;span++)
        if (overlaps(spans[span][0],spans[span][1],0x3D0000u,0x30000u) ||
            (argument && (overlaps(spans[span][0],spans[span][1],plan->definition,240u) ||
                          overlaps(spans[span][0],spans[span][1],plan->wrapper,12u))))
            d3d8_hle_fatal(entry,"pixel binder command bounds or aliases are unsupported");
    /* All potentially failing checks happen before wrapper/cache/packet publication. */
    if (argument && definition_api) probe(entry,INTERNAL_WRAPPER,12u);
    probe(entry,device+0x784u,argument ? 16u : 4u);
    probe(entry,D3D8_GLOBAL_DIRTY_MASK,4u);
    if (argument && !(state[2]&0x10u)) {
        probe(entry,0x3E3CC0u,228u); probe(entry,0x3E3EE0u,4u);
    }
    if (!argument) probe(entry,FACTOR,4u);
    for (uint32_t span=0u;span<span_count;span++) probe(entry,spans[span][0],spans[span][1]);
    probe(entry,device,4u);
}
