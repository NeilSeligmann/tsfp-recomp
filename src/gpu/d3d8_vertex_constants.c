/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_vertex_constants.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_state.h"
#include "kernel_call.h"
#include <string.h>
#define ENTRY 0x003D58B0u
#define ENTRY_PLAIN 0x003D57D0u
#define MAX_WORDS (D3D8_CONSTANT_REGISTERS*4u)
#define MAX_PACKET_WORDS (2u+MAX_WORDS+D3D8_CONSTANT_REGISTERS/4u)
static bool overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn)
{
    return an!=0u && bn!=0u && (uint64_t)a<(uint64_t)b+bn && (uint64_t)b<(uint64_t)a+an;
}
static void read_span(uint32_t entry,uint32_t address,void *out,size_t bytes)
{
    if(bytes!=0u && !kernel_guest_read_bytes(address,out,bytes))
        d3d8_hle_fatal(entry,"vertex constant input span is unreadable or overflows");
}
static void probe(uint32_t entry,uint32_t address,size_t bytes)
{
    uint8_t old[MAX_PACKET_WORDS*4u];
    if(bytes!=0u && (!kernel_guest_read_bytes(address,old,bytes) ||
                    !kernel_guest_write_bytes(address,old,bytes)))
        d3d8_hle_fatal(entry,"vertex constant output span is unreadable/unwritable");
}
static bool collides(uint32_t address,uint32_t bytes)
{
    return overlap(address,bytes,D3D8_CONSTANT_SHADOW,MAX_WORDS*4u) ||
           overlap(address,bytes,D3D8_DEVICE_BASE,0x2500u) ||
           overlap(address,bytes,0x3E3F58u,4u);
}
/* `shadowed` is 0x003D58B0 (writes the CPU shadow unless device flag 0x10 is set). Not shadowed is 0x003D57D0 (T734):
 * the same emitter body (same entry roll-over, same chunks, same refill site, same return value) with no shadow
 * write and no flags test, so a source inside the shadow is only read, as the original reads it. */
static uint32_t upload_vertex_constants(uint32_t entry,bool shadowed,uint32_t index,uint32_t data,uint32_t count)
{
    if(index>=D3D8_CONSTANT_REGISTERS || count>(D3D8_CONSTANT_REGISTERS-index)*4u)
        d3d8_hle_fatal(entry,"vertex constants exceed recovered192-register policy (original unchecked)");
    uint32_t control[3];read_span(entry,D3D8_DEVICE_BASE,control,sizeof(control));
    const uint32_t source_bytes=count*4u;
    const uint32_t chunks=count==0u?1u:(count+15u)/16u;
    const uint32_t packet_bytes=8u+source_bytes+chunks*4u;
    const uint32_t shadow=D3D8_CONSTANT_SHADOW+index*16u;
    /* T391: the original's preamble (0x003D57DE, `jae 0x003D5890`) rolls the ring over when the cursor has
     * reached the limit, then writes at the new cursor. T487: it checks the limit again only after a FULL chunk of
     * 16 dwords that another FULL chunk follows (0x003D5877, `cmp edi, [limit]`, never before a last partial chunk)
     * and there calls the refill (0x003D5880 to 0x003D588B) with the cursor published, so a packet runs past the
     * limit into the 0x204 byte slack or is split in runs by a refill, whose fence packet (and, when the ring
     * wraps, jump word) lands between the chunks. T526: any number of refills, the entry roll-over's included,
     * each planned from what the ones before it left. All of it is planned over a simulated writer before any
     * write, so every check below sees where each run goes and a refusal leaves guest memory untouched. */
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_split plan;
    const uint32_t partial=count&15u;
    d3d8_pushbuffer_sim_split_site(&sim,entry,8u,4u+16u*4u,count/16u,
                                   count==0u?4u:(partial!=0u?4u+partial*4u:0u),&plan);
    const uint32_t cursor=plan.begin;
    uint32_t planned_bytes=0u;
    for(uint32_t run=0u;run<plan.runs;run++)planned_bytes+=plan.run[run].bytes;
    if(planned_bytes!=packet_bytes)
        d3d8_hle_fatal(entry,"vertex constants planned %u bytes for a %u byte packet",
                       (unsigned)planned_bytes,(unsigned)packet_bytes);
    if((source_bytes!=0u && data>UINT32_MAX-source_bytes) ||
       (shadowed && overlap(data,source_bytes,D3D8_CONSTANT_SHADOW,MAX_WORDS*4u)) ||
       overlap(data,source_bytes,D3D8_DEVICE_BASE,0x2500u))
        d3d8_hle_fatal(entry,"vertex constant source/cache/command aliases are unsupported");
    /* Every span the emitter writes: the runs, and the jump word each wrapping refill writes at the end of its run.
     * (The fence packets are not listed: the source is at least two full chunks long when a packet splits, so a
     * source touching a fence packet also touches a run, and the refill's own writes land in the order the
     * original makes them.) */
    uint32_t spans[2u*D3D8_PUSHBUFFER_MAX_RUNS][2];
    unsigned span_count=0u;
    for(uint32_t run=0u;run<plan.runs;run++){
        const d3d8_pushbuffer_run *planned=&plan.run[run];
        spans[span_count][0]=planned->begin;spans[span_count++][1]=planned->bytes;
        spans[span_count][0]=planned->begin+planned->bytes;spans[span_count++][1]=planned->wraps?4u:0u;
    }
    for(unsigned span=0u;span<span_count;span++)
        if(overlap(data,source_bytes,spans[span][0],spans[span][1]) ||
           collides(spans[span][0],spans[span][1]))
            d3d8_hle_fatal(entry,"vertex constant source/cache/command aliases are unsupported");
    uint32_t source[MAX_WORDS],packet[MAX_PACKET_WORDS];
    read_span(entry,data,source,source_bytes);
    /* Same-byte permission probes do not publish guest content. Exclusive
     * quiescent mappings keep all permissions stable through publication. */
    for(unsigned span=0u;span<span_count;span++)probe(entry,spans[span][0],spans[span][1]);
    probe(entry,D3D8_DEVICE_BASE,4u);
    if(plan.entered && d3d8_pushbuffer_begin()!=cursor)
        d3d8_hle_fatal(entry,"the vertex constants' roll-over did not land where it was previewed");
    const bool writes_shadow=shadowed && (control[2]&0x10u)==0u;
    if(writes_shadow)probe(entry,shadow,source_bytes);
    packet[0]=0x41EA4u;packet[1]=index;
    uint32_t at=2u,copied=0u,last=0u;
    do{
        const uint32_t remaining=count-copied;
        const uint32_t chunk=remaining<16u?remaining:16u;
        last=(chunk<<18u)|0xB80u;packet[at++]=last;
        if(chunk!=0u)memcpy(packet+at,source+copied,(size_t)chunk*4u);
        at+=chunk;copied+=chunk;
    }while(copied!=count);
    if(writes_shadow && source_bytes!=0u &&
       !kernel_guest_write_bytes(shadow,source,source_bytes))
        d3d8_hle_fatal(entry,"vertex constant mappings changed during publication");
    uint32_t written=0u;
    for(uint32_t run=0u;run<plan.runs;run++){
        const d3d8_pushbuffer_run *planned=&plan.run[run];
        if(!kernel_guest_write_bytes(planned->begin,(const uint8_t *)packet+written,planned->bytes))
            d3d8_hle_fatal(entry,"vertex constant mappings changed during publication");
        written+=planned->bytes;
        if(run+1u==plan.runs)break;
        /* The run is in the ring before the refill hands it to the GPU. */
        if(d3d8_pushbuffer_refill_at(planned->begin+planned->bytes)!=plan.run[run+1u].begin)
            d3d8_hle_fatal(entry,"the vertex constants' refill did not land where it was previewed");
    }
    const uint32_t next=plan.run[plan.runs-1u].begin+plan.run[plan.runs-1u].bytes;
    if(!kernel_guest_write_bytes(D3D8_DEVICE_BASE,&next,4u))
        d3d8_hle_fatal(entry,"vertex constant mappings changed during publication");
    return count!=0u && (count&15u)==0u?0u:last;
}
uint32_t d3d8_upload_vertex_constants(uint32_t index,uint32_t data,uint32_t count)
{
    return upload_vertex_constants(ENTRY,true,index,data,count);
}
uint32_t d3d8_upload_vertex_constants_unshadowed(uint32_t index,uint32_t data,uint32_t count)
{
    return upload_vertex_constants(ENTRY_PLAIN,false,index,data,count);
}
static uint32_t upload_fixed_vertex_constants(uint32_t entry, bool shadowed,
                                               uint32_t registers, uint32_t index, uint32_t data)
{
    const uint32_t source_bytes = registers * 16u;
    const uint32_t packet_bytes = 12u + source_bytes;
    if (index > D3D8_CONSTANT_REGISTERS - registers)
        d3d8_hle_fatal(entry, "fixed vertex constants exceed recovered192-register policy (original unchecked)");
    /* T990: 5670/56D0 use the same checked-refill structure with28 bytes.
     * T579: the original (0x003D5720) has no entry preamble. It tests `cursor + 76 >= limit` first and, at or past it,
     * calls the refill 0x003D6B20 and tests again from the new cursor and limit (0x003D57BD jumps back to the top),
     * so one call refills any number of times, each from the state the ones before it left, and only then loads the
     * source (the eight MMX loads at 0x003D5739), writes the packet and the shadow and publishes the cursor. It is
     * planned over the carried simulation before any write; the source is read again after the refills, as the
     * original reads it, so a source inside what a refill writes (a jump word, a fence packet, the semaphore dword,
     * a history entry) is read the way the original reads it. */
    d3d8_pushbuffer_sim sim=d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_split plan;
    d3d8_pushbuffer_sim_checked_site(&sim,entry,packet_bytes,&plan);
    const uint32_t cursor=plan.begin;
    const uint32_t shadow = D3D8_CONSTANT_SHADOW + index * 16u;
    if ((uint64_t)data + source_bytes > UINT64_C(0x100000000) ||
        (shadowed && overlap(data,source_bytes,D3D8_CONSTANT_SHADOW,MAX_WORDS*4u)) ||
        overlap(data,source_bytes,D3D8_DEVICE_BASE,0x2500u) ||
        overlap(data,source_bytes,cursor,packet_bytes))
        d3d8_hle_fatal(entry,"fixed vertex constant source/cache/command aliases are unsupported");
    for (uint32_t span=0u;span<sim.span_count;span++)
        if (collides(sim.span[span].begin,sim.span[span].bytes))
            d3d8_hle_fatal(entry,"fixed vertex constant source/cache/command aliases are unsupported");
    uint32_t packet[19] = {0x41EA4u,index,(registers << 20u) | 0xB80u};
    if (!kernel_guest_read_bytes(data,packet+3,source_bytes))
        d3d8_hle_fatal(entry,"fixed vertex constant source is unreadable");
    /* Same-byte permission probes do not publish guest content. */
    for (uint32_t span=0u;span<sim.span_count;span++)probe(entry,sim.span[span].begin,sim.span[span].bytes);
    if (shadowed) probe(entry,shadow,source_bytes);
    probe(entry,D3D8_DEVICE_BASE,4u);
    for (uint32_t run=0u;run+1u<plan.runs;run++) {
        /* The cursor is published and the ring rolls over; nothing is written between two refills. */
        if (d3d8_pushbuffer_refill_at(plan.run[run].begin)!=plan.run[run+1u].begin)
            d3d8_hle_fatal(entry,"fixed vertex constant refill differs from its preview");
    }
    /* The original loads the source after its last refill. */
    if (plan.split && !kernel_guest_read_bytes(data,packet+3,source_bytes))
        d3d8_hle_fatal(entry,"fixed vertex constant mappings changed during publication");
    /* 3D5670 and 3D5720 always update shadow, even for flag0x10.
     * 3D56D0 never reads the flags or writes the CPU shadow. */
    if ((shadowed && !kernel_guest_write_bytes(shadow,packet+3,source_bytes)) ||
        !kernel_guest_write_bytes(cursor,packet,packet_bytes))
        d3d8_hle_fatal(entry,"fixed vertex constant mappings changed during publication");
    d3d8_pushbuffer_end(cursor+packet_bytes);
    return cursor+packet_bytes;
}

uint32_t d3d8_upload_four_vertex_constants(uint32_t index, uint32_t data)
{
    return upload_fixed_vertex_constants(0x003D5720u, true, 4u, index, data);
}

uint32_t d3d8_upload_one_vertex_constant(uint32_t index, uint32_t data)
{
    return upload_fixed_vertex_constants(0x003D5670u, true, 1u, index, data);
}

uint32_t d3d8_upload_one_vertex_constant_fast(uint32_t index, uint32_t data)
{
    return upload_fixed_vertex_constants(0x003D56D0u, false, 1u, index, data);
}

static uint32_t fixed_handler(void *context)
{
    uint32_t index,data;
    if (!kernel_frame_reg_arg(context,0u,&index) || !kernel_frame_reg_arg(context,1u,&data))
        d3d8_hle_fatal(0x003D5720u,"fixed vertex constant register arguments are unreadable");
    return d3d8_upload_four_vertex_constants(index,data);
}

static uint32_t one_handler(void *context)
{
    uint32_t index, data;
    if (!kernel_frame_reg_arg(context,0u,&index) || !kernel_frame_reg_arg(context,1u,&data))
        d3d8_hle_fatal(0x003D5670u,"fixed vertex constant register arguments are unreadable");
    return d3d8_upload_one_vertex_constant(index,data);
}

static uint32_t one_fast_handler(void *context)
{
    uint32_t index, data;
    if (!kernel_frame_reg_arg(context,0u,&index) || !kernel_frame_reg_arg(context,1u,&data))
        d3d8_hle_fatal(0x003D56D0u,"fixed vertex constant register arguments are unreadable");
    return d3d8_upload_one_vertex_constant_fast(index,data);
}

static uint32_t plain_handler(void *context)
{
    uint32_t index,data,count;
    const kernel_call_frame *frame=(const kernel_call_frame *)context;
    if(!kernel_frame_reg_arg(frame,0u,&index) || !kernel_frame_reg_arg(frame,1u,&data) ||
       !kernel_frame_arg(frame,0u,&count))
        d3d8_hle_fatal(ENTRY_PLAIN,"vertex constant register/stack arguments are unreadable");
    return d3d8_upload_vertex_constants_unshadowed(index,data,count);
}

static uint32_t handler(void *context)
{
    uint32_t index,data,count;
    const kernel_call_frame *frame=(const kernel_call_frame *)context;
    if(!kernel_frame_reg_arg(frame,0u,&index) || !kernel_frame_reg_arg(frame,1u,&data) ||
       !kernel_frame_arg(frame,0u,&count))
        d3d8_hle_fatal(ENTRY,"vertex constant register/stack arguments are unreadable");
    return d3d8_upload_vertex_constants(index,data,count);
}
size_t d3d8_vertex_constants_register(void)
{
    return (size_t)d3d8_hle_register(0x003D5670u,one_handler) +
           (size_t)d3d8_hle_register(0x003D56D0u,one_fast_handler) +
           (size_t)d3d8_hle_register(ENTRY,handler) +
           (size_t)d3d8_hle_register(0x003D5720u,fixed_handler) +
           (size_t)d3d8_hle_register(ENTRY_PLAIN,plain_handler);
}
