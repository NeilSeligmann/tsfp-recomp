/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_indexed.h"
#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_resource.h"
#include "kernel_call.h"
#include <string.h>
#define ENTRY 0x003D5050u
static bool overlaps(uint32_t a,uint32_t bytes,uint32_t b,uint32_t span)
{
    return (uint64_t)a+bytes>b && (uint64_t)b+span>a;
}
/* Device flag 0x800 is set from after the deferred work (0x003D5076) to the end of the draw, so a refill inside it
 * inserts no fence packet and leaves flag 0x1000, which the draw answers with one fence at its end (0x003D5298). */
#define FLAG_IN_DRAW 0x800u
#define FLAG_REFILLED 0x1000u
#define FLAG_KICKOFF (FLAG_IN_DRAW | FLAG_REFILLED)
/* The end of a group of commands: the original publishes the cursor and makes the sized reservation 0x003D6B30(0x204),
 * which rolls the ring over when the group left too little. T546: the dry run (`plan`) plans it over the simulation
 * and notes the run written so far, the real one performs it. Returns where the next group starts. */
static uint64_t next_group(uint64_t position,uint32_t *run_begin,d3d8_pushbuffer_sim *plan)
{
    if(plan==NULL) {
        d3d8_pushbuffer_end((uint32_t)position);
        return d3d8_pushbuffer_reserve(0x204u);
    }
    if(position>UINT32_MAX)d3d8_hle_fatal(ENTRY,"indexed packet address overflows");
    (void)d3d8_pushbuffer_sim_write(plan,ENTRY,(uint32_t)position-*run_begin);
    (void)d3d8_pushbuffer_sim_reserve(plan,0x204u);
    *run_begin=plan->cursor;
    return plan->cursor;
}
/* Walk the original header grouping from `cursor` (just past the 8-byte draw header). `plan` is the dry run, which
 * records every run and reservation over the simulation and writes nothing, NULL the real emission.
 * Scalar copies preserve the original MMX/REP memory bytes for disjoint spans. */
static uint32_t packets(uint32_t cursor,uint32_t count,uint32_t data,d3d8_pushbuffer_sim *plan)
{
    const bool emit=plan==NULL;
    uint32_t run_begin=emit?0u:cursor-8u;
    uint64_t position=cursor;
    uint32_t remaining=count,source=data;
    const uint32_t alignment=((0u-cursor)>>2u)&7u;
    bool fast=remaining>=alignment*2u+62u;
    uint32_t pairs;
    if(fast && alignment!=0u) {
        pairs=alignment-1u;
        if(emit)d3d8_guest_store32((uint32_t)position,0x40001800u+(pairs<<18u));
        position+=4u;
        for(uint32_t i=0u;i<pairs;i++) {
            if(emit)d3d8_guest_store32((uint32_t)position,d3d8_guest_load32(source));
            position+=4u;source+=4u;
        }
        remaining-=pairs*2u;
    }
    if(fast) {
        const uint32_t remainder=remaining%1022u;
        pairs=remainder>=62u?(((remainder>>1u)+1u)&~15u)-1u:511u;
        do {
            if(emit)d3d8_guest_store32((uint32_t)position,0x40001800u+(pairs<<18u));
            position+=4u;
            for(uint32_t i=0u;i<pairs;i++) {
                if(emit)d3d8_guest_store32((uint32_t)position,d3d8_guest_load32(source));
                position+=4u;source+=4u;
            }
            remaining-=pairs*2u;
            position=next_group(position,&run_begin,plan);
            pairs=511u;
        }while(remaining>=1022u);
    }
    pairs=remaining>>1u;
    if(emit)d3d8_guest_store32((uint32_t)position,0x40001800u+(pairs<<18u));
    position+=4u;
    for(uint32_t i=0u;i<pairs;i++) {
        if(emit)d3d8_guest_store32((uint32_t)position,d3d8_guest_load32(source));
        position+=4u;source+=4u;
    }
    if((remaining&1u)!=0u) {
        uint16_t last=0u;
        if(emit){
            if(!kernel_guest_read_bytes(source,&last,2u))
                d3d8_hle_fatal(ENTRY,"index array changed readability during emission");
            d3d8_guest_store32((uint32_t)position,0x00041808u);
            d3d8_guest_store32((uint32_t)position+4u,last);
        }
        position+=8u;
    }
    position+=8u; /* SET_BEGIN_END(0) */
    if(position>UINT32_MAX)d3d8_hle_fatal(ENTRY,"indexed packet address overflows");
    if(!emit)(void)d3d8_pushbuffer_sim_write(plan,ENTRY,(uint32_t)position-run_begin);
    return (uint32_t)position;
}
static void check_stream_overlap(uint32_t begin,uint32_t bytes,uint32_t base_vertex)
{
    const uint32_t dirty=d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    if((dirty&0x40000000u)!=0u || ((dirty&0x40u)==0u && d3d8_device_load32(0x20u)==base_vertex))return;
    const uint32_t declaration=d3d8_device_load32(0x794u);
    if((uint64_t)declaration+0x1020u>UINT64_C(0x100000000) ||
       overlaps(begin,bytes,declaration,0x1020u))
        d3d8_hle_fatal(ENTRY,"indexed command/declaration overlap is not recovered");
    const uint32_t mapping=0x005496A0u+(d3d8_guest_load32(declaration+4u)&0x10u);
    for(uint32_t attribute=0u;attribute<16u;attribute++) {
        const uint32_t descriptor=declaration+d3d8_guest_load8(mapping+attribute)*16u;
        if(d3d8_guest_load32(descriptor+0x1Cu)==2u)continue;
        const uint32_t stream=d3d8_guest_load32(descriptor+0x14u);
        const uint32_t buffer=d3d8_guest_load32(0x003E2BB0u+stream*12u);
        if(buffer!=0u && overlaps(begin,bytes,buffer,8u))
            d3d8_hle_fatal(ENTRY,"indexed command/stream-header overlap is not recovered");
    }
}
uint32_t d3d8_draw_indexed_vertices(uint32_t primitive,uint32_t count,uint32_t index_data)
{
    if(d3d8_guest_load32(0x003E3F58u)!=D3D8_DEVICE_BASE)
        d3d8_hle_fatal(ENTRY,"indexed reservation device differs from fixed device");
    if(count>UINT32_MAX/2u || (count!=0u && kernel_guest_at(index_data,count*2u)==NULL))
        d3d8_hle_fatal(ENTRY,"index array span is unmapped or overflows");
    /* Quiescent guest mappings are required throughout validation/emission.
     * Probe every input byte before deferred state can be changed. */
    for(uint32_t offset=0u;offset<count*2u;) {
        uint8_t probe[256];
        uint32_t bytes=count*2u-offset;
        if(bytes>sizeof(probe))bytes=sizeof(probe);
        if(!kernel_guest_read_bytes(index_data+offset,probe,bytes))
            d3d8_hle_fatal(ENTRY,"index array is unreadable");
        offset+=bytes;
    }
    const uint32_t base_vertex=d3d8_device_load32(0x1Cu);
    /* T546: ONE simulation for everything the call writes, in the original's order: the deferred cascade and stream
     * work (0x003D5068: each emitter has its own preamble), the flag 0x800 set before the sized reservation of 0x209
     * dwords (0x003D5076), the draw's groups with a reservation between them, and the fence a refill under the flag
     * leaves to be inserted at the end (0x003D5298). A refill anywhere moves what follows it. */
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    d3d8_draw_plan_deferred(&sim,base_vertex);
    d3d8_pushbuffer_sim_set_flags(&sim,d3d8_pushbuffer_sim_flags(&sim)|FLAG_IN_DRAW);
    (void)d3d8_pushbuffer_sim_reserve(&sim,0x209u);
    const uint32_t start=sim.cursor;
    if((uint64_t)start+8u>UINT32_MAX)d3d8_hle_fatal(ENTRY,"deferred packet address overflows");
    const uint32_t end=packets(start+8u,count,index_data,&sim); /* the planned end, refills included */
    if(count!=0u && overlaps(index_data,count*2u,0x003D0000u,0x30000u))
        d3d8_hle_fatal(ENTRY,"indexed input/output overlap is not recovered");
    for(uint32_t span=0u;span<sim.span_count;span++) {
        const uint32_t begin=sim.span[span].begin,bytes=sim.span[span].bytes;
        if(overlaps(begin,bytes,0x003D0000u,0x30000u) ||
           overlaps(begin,bytes,0x005496A0u,32u) ||
           overlaps(begin,bytes,0x00470000u,0x10000u) ||
           overlaps(begin,bytes,0x004A1000u,0x1000u) ||
           (count!=0u && overlaps(begin,bytes,index_data,count*2u)))
            d3d8_hle_fatal(ENTRY,"indexed input/output overlap is not recovered");
        check_stream_overlap(begin,bytes,base_vertex);
    }
    /* The fence at the end is written after the last index is read, so it is planned (it may refill) but not checked
     * against the inputs. */
    const uint32_t finished_flags=d3d8_pushbuffer_sim_flags(&sim);
    const bool kickoff=(finished_flags&FLAG_REFILLED)!=0u;
    d3d8_pushbuffer_sim_set_flags(&sim,finished_flags&~FLAG_KICKOFF);
    if(kickoff)(void)d3d8_pushbuffer_sim_site(&sim,ENTRY,D3D8_PUSHBUFFER_FENCE_PACKET_BYTES);
    d3d8_draw_flush_streams(base_vertex);
    /* 0x003D5070: the flags are read after the deferred work. */
    d3d8_device_store32(D3D8_DEV_FLAGS,d3d8_device_load32(D3D8_DEV_FLAGS)|FLAG_IN_DRAW);
    if(d3d8_pushbuffer_reserve(0x209u)!=start)
        d3d8_hle_fatal(ENTRY,"indexed plan disagrees with the reservation");
    d3d8_guest_store32(start,0x000417FCu);d3d8_guest_store32(start+4u,primitive);
    const uint32_t actual_end=packets(start+8u,count,index_data,NULL);
    if(actual_end!=end)d3d8_hle_fatal(ENTRY,"indexed plan disagrees with the emission");
    d3d8_guest_store32(actual_end-8u,0x000417FCu);d3d8_guest_store32(actual_end-4u,0u);
    d3d8_pushbuffer_end(actual_end);
    const uint32_t finished=d3d8_device_load32(D3D8_DEV_FLAGS);
    d3d8_device_store32(D3D8_DEV_FLAGS,finished&~FLAG_KICKOFF);
    if((finished&FLAG_REFILLED)!=0u)return d3d8_gpu_fence_insert(1u);
    return finished;
}
static uint32_t handler(void *context)
{
    uint32_t args[3];
    for(unsigned i=0u;i<3u;i++)if(!kernel_frame_arg((const kernel_call_frame *)context,i,&args[i]))
        d3d8_hle_fatal(ENTRY,"indexed draw argument unreadable");
    return d3d8_draw_indexed_vertices(args[0],args[1],args[2]);
}
size_t d3d8_indexed_register(void){return(size_t)d3d8_hle_register(ENTRY,handler);}
