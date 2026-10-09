/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_pixel_constants.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"
#include <stdbool.h>
#define ENTRY 0x003D9520u
#define DEVICE_GLOBAL 0x003E3F58u
#define USAGE 0x005496E0u
#define MAX_PACKET_BYTES 2320u
static bool overlap(uint32_t a,uint32_t n,uint32_t b,uint32_t m)
{
    return n && m && (uint64_t)a<(uint64_t)b+m && (uint64_t)b<(uint64_t)a+n;
}
static void read_span(uint32_t address,void *out,size_t bytes)
{
    if (!kernel_guest_read_bytes(address,out,bytes))
        d3d8_hle_fatal(ENTRY,"pixel constant input %#x/%zu is unreadable",address,bytes);
}
static void probe(uint32_t address,size_t bytes)
{
    uint8_t old[MAX_PACKET_BYTES];
    if (bytes && (bytes>sizeof(old) || !kernel_guest_read_bytes(address,old,bytes) ||
                  !kernel_guest_write_bytes(address,old,bytes)))
        d3d8_hle_fatal(ENTRY,"pixel constant output %#x/%zu is not readable/writable",address,bytes);
}
void d3d8_pixel_constants_prepare(uint32_t index,uint32_t data,uint32_t count,
                                  d3d8_pixel_constants_plan *plan)
{
    uint32_t device,wrapper,definition;
    read_span(DEVICE_GLOBAL,&device,4u);
    if (device!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(ENTRY,"pixel constants require the recovered fixed device");
    read_span(device+0x784u,&wrapper,4u);
    if (wrapper>UINT32_MAX-11u)
        d3d8_hle_fatal(ENTRY,"pixel constant wrapper pointer wraps");
    read_span(wrapper+8u,&definition,4u);
    /* Original count-zero path stops here: no data, map, usage, cursor or outputs. */
    if (!count) return;
    if (index>=D3D8_PIXEL_CONSTANT_SLOTS || count>D3D8_PIXEL_CONSTANT_SLOTS-index)
        d3d8_hle_fatal(ENTRY,"pixel constants exceed the local16-slot policy (original unchecked)");
    if ((uint64_t)data+count*16u>UINT64_C(0x100000000) || definition>UINT32_MAX-239u)
        d3d8_hle_fatal(ENTRY,"pixel constant source/map span wraps");
    const uint32_t map_address=definition+0xE4u,usage_address=USAGE+index*4u;
    if (overlap(data,count*16u,0x3D0000u,0x30000u) ||
        overlap(map_address,12u,0x3D0000u,0x30000u) ||
        (wrapper!=device+0x924u && overlap(wrapper+8u,4u,0x3D0000u,0x30000u)))
        d3d8_hle_fatal(ENTRY,"pixel constant inputs alias recovered D3D state");
    read_span(data,plan->source,count*16u);
    read_span(usage_address,plan->usage,count*4u);
    read_span(map_address,plan->maps,12u);
    uint32_t ring[2];
    read_span(device+D3D8_DEV_PB_BASE,ring,sizeof(ring));
    /* 0x003D95B3 to 0x003D95BF: every vector checks the limit with the cursor the previous one published and
     * refills there (0x003D6B20), then writes its commands at the cursor the refill leaves. One site per vector, over
     * a writer that carries the state each refill moves, so the vector after a refill is planned from it. */
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    uint32_t selected=0u,spans[3u*D3D8_PIXEL_CONSTANT_SLOTS][2],span_count=0u;
    for (uint32_t vector=0u;vector<count;vector++) {
        uint32_t bytes=0u;
        for (uint32_t factor=0u;factor<3u;factor++) {
            const uint32_t map=plan->usage[vector]^plan->maps[factor];
            const uint32_t stages=factor==2u ? 2u : 8u;
            for (uint32_t stage=0u;stage<stages;stage++)
                if (((map>>(stage*4u))&15u)==0u) {
                    bytes+=8u; selected|=1u<<(factor*8u+stage);
                }
        }
        if (index+vector==0u) bytes+=16u;
        const uint32_t refills=sim.refills;
        const uint32_t start=d3d8_pushbuffer_sim_site(&sim,ENTRY,bytes);
        plan->site[vector]=start;
        spans[span_count][0]=start; spans[span_count++][1]=bytes;
        if (sim.refills!=refills) {
            /* What the refill itself writes: the jump word at the end of the previous vector when it wraps, and
             * its fence packet at the start of the new segment. */
            spans[span_count][0]=sim.last_refill.at; spans[span_count++][1]=sim.last_refill.wraps ? 4u : 0u;
            spans[span_count][0]=sim.last_refill.fence_begin; spans[span_count++][1]=sim.last_refill.fence_bytes;
        }
        if ((ring[0] || ring[1]) && (start<ring[0] || ring[1]<=ring[0] || (uint64_t)start+bytes>ring[1]))
            d3d8_hle_fatal(ENTRY,"pixel constant command extent wraps or exceeds the ring");
    }
    for (uint32_t span=0u;span<span_count;span++)
        if (overlap(spans[span][0],spans[span][1],0x3D0000u,0x30000u) ||
            overlap(spans[span][0],spans[span][1],data,count*16u) ||
            overlap(spans[span][0],spans[span][1],usage_address,count*4u) ||
            overlap(spans[span][0],spans[span][1],map_address,12u) ||
            overlap(spans[span][0],spans[span][1],wrapper+8u,4u))
            d3d8_hle_fatal(ENTRY,"pixel constant command/input/state aliases are unsupported");
    probe(device+0x8E4u+index*4u,count*4u);
    for (uint32_t factor=0u;factor<3u;factor++) {
        const uint32_t stages=factor==2u ? 2u : 8u;
        const uint32_t shadow=factor==2u ? 0x3E3D6Cu : 0x3E3CE8u+factor*0x20u;
        for (uint32_t stage=0u;stage<stages;stage++)
            if (selected&(1u<<(factor*8u+stage))) probe(shadow+stage*4u,4u);
    }
    for (uint32_t span=0u;span<span_count;span++) probe(spans[span][0],spans[span][1]);
    probe(device,4u);
}
