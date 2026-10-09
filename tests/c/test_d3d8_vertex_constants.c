/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_vertex_constants.h"
#include "d3d8_state.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define STREAM 0xD00000u
#define SOURCE 0xD03000u
static uint8_t state[D3D_REGION_BYTES],stream[4096],source[3072];
static void snapshot(void)
{
    CHECK(kernel_guest_read_bytes(D3D_REGION_BASE,state,sizeof(state)));
    CHECK(kernel_guest_read_bytes(STREAM,stream,sizeof(stream)));
    CHECK(kernel_guest_read_bytes(SOURCE,source,sizeof(source)));
}
static void unchanged(void)
{
    CHECK(memcmp(state,kernel_guest_at(D3D_REGION_BASE,sizeof(state)),sizeof(state))==0);
    CHECK(memcmp(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream))==0);
    CHECK(memcmp(source,kernel_guest_at(SOURCE,sizeof(source)),sizeof(source))==0);
}
static uint32_t recorded,recorded_begin,recorded_end,recorded_last,recorded_complete;
/* The consumer sees the first run COMPLETE: its last dword is the source dword the chunk ends with. */
static void record(void *context,uint32_t begin,uint32_t end)
{
    (void)context;recorded++;recorded_begin=begin;recorded_end=end;recorded_last=load(end-4u);
    recorded_complete=load(end-4u)==0x50607080u+15u?1u:0u;
}
/* T990 one-vector body: fixed28-byte packet, shadow policy independent of flags. */
static void test_one_constants(void)
{
    for (unsigned fast=0u;fast<2u;fast++)for(unsigned flag=0u;flag<2u;flag++) {
        store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+29u);
        store(D3D8_DEVICE_BASE+8u,flag?0x4213u:0x4203u);
        for(unsigned i=0u;i<768u;i++)store(D3D8_CONSTANT_SHADOW+i*4u,0xAAAAAAAAu);
        for(unsigned i=0u;i<16u;i++)store(STREAM+i*4u,0xBBBBBBBBu);
        const uint32_t next=fast?d3d8_upload_one_vertex_constant_fast(191u,SOURCE):
                                  d3d8_upload_one_vertex_constant(191u,SOURCE);
        CHECK_EQ_U32(next,STREAM+28u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),next);
        CHECK_EQ_U32(load(STREAM),0x41EA4u);CHECK_EQ_U32(load(STREAM+4u),191u);
        CHECK_EQ_U32(load(STREAM+8u),0x100B80u);
        for(unsigned i=0u;i<4u;i++) {
            CHECK_EQ_U32(load(STREAM+12u+i*4u),load(SOURCE+i*4u));
            CHECK_EQ_U32(load(D3D8_CONSTANT_SHADOW+191u*16u+i*4u),fast?0xAAAAAAAAu:load(SOURCE+i*4u));
        }
        CHECK_EQ_U32(load(STREAM+28u),0xBBBBBBBBu);
        CHECK_EQ_U32(load(D3D8_CONSTANT_SHADOW+190u*16u),0xAAAAAAAAu);
    }
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    CHECK_EQ_U32(d3d8_upload_one_vertex_constant_fast(0u,D3D8_CONSTANT_SHADOW),STREAM+28u);
    for(unsigned i=0u;i<4u;i++)CHECK_EQ_U32(load(STREAM+12u+i*4u),load(D3D8_CONSTANT_SHADOW+i*4u));
    /* Original equalityrefills, not only overflow: no ring makes this a named stop. */
    for(unsigned delta=0u;delta<2u;delta++) {
        store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+28u-delta);snapshot();
        RUN_EXPECTING_FATAL((void)d3d8_upload_one_vertex_constant_fast(0u,SOURCE));CHECK(fatal_seen);unchanged();
    }
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_upload_one_vertex_constant(192u,SOURCE));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_one_vertex_constant_fast(0u,0xFFFFFFF8u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_one_vertex_constant(0u,D3D8_CONSTANT_SHADOW));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_one_vertex_constant_fast(0u,STREAM));CHECK(fatal_seen);unchanged();
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(STREAM,0x4000u);
    for(unsigned i=0u;i<768u;i++)store(SOURCE+i*4u,0x10203040u+i);
    test_one_constants();
    const unsigned counts[]={0u,1u,8u,15u,16u,17u,32u,33u,768u};
    for(unsigned f=0u;f<2u;f++)for(unsigned c=0u;c<9u;c++){
        const uint32_t n=counts[c];
        store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
        store(D3D8_DEVICE_BASE+8u,f==0u?0x4203u:0x4213u);
        for(unsigned i=0u;i<768u;i++)store(D3D8_CONSTANT_SHADOW+i*4u,0xAAAAAAAAu);
        for(unsigned i=0u;i<1024u;i++)store(STREAM+i*4u,0xBBBBBBBBu);
        const uint32_t expected=n!=0u && (n&15u)==0u?0u:((n&15u)<<18u)|0xB80u;
        CHECK_EQ_U32(d3d8_upload_vertex_constants(0u,SOURCE,n),expected);
        const uint32_t chunks=n==0u?1u:(n+15u)/16u;
        const uint32_t bytes=8u+4u*n+4u*chunks;
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+bytes);
        CHECK_EQ_U32(load(STREAM),0x41EA4u);CHECK_EQ_U32(load(STREAM+4u),0u);
        uint32_t at=STREAM+8u,copied=0u;
        do{
            const uint32_t chunk=n-copied<16u?n-copied:16u;
            CHECK_EQ_U32(load(at),(chunk<<18u)|0xB80u);at+=4u;
            for(unsigned i=0u;i<chunk;i++){
                CHECK_EQ_U32(load(at),load(SOURCE+(copied+i)*4u));at+=4u;
            }
            copied+=chunk;
        }while(copied!=n);
        CHECK_EQ_U32(load(STREAM+bytes),0xBBBBBBBBu);
        for(unsigned i=0u;i<768u;i++)
            CHECK_EQ_U32(load(D3D8_CONSTANT_SHADOW+i*4u),f==0u && i<n?load(SOURCE+i*4u):0xAAAAAAAAu);
    }
    /* T734: 0x003D57D0 writes the same packet and returns the same value but never touches the shadow, with the
     * device flag 0x10 clear or set, and it may read its source from the shadow (the original only reads it). */
    for(unsigned f=0u;f<2u;f++)for(unsigned c=0u;c<9u;c++){
        const uint32_t n=counts[c];
        store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
        store(D3D8_DEVICE_BASE+8u,f==0u?0x4203u:0x4213u);
        for(unsigned i=0u;i<768u;i++)store(D3D8_CONSTANT_SHADOW+i*4u,0xAAAAAAAAu);
        for(unsigned i=0u;i<1024u;i++)store(STREAM+i*4u,0xBBBBBBBBu);
        const uint32_t expected=n!=0u && (n&15u)==0u?0u:((n&15u)<<18u)|0xB80u;
        CHECK_EQ_U32(d3d8_upload_vertex_constants_unshadowed(0u,SOURCE,n),expected);
        const uint32_t bytes=8u+4u*n+4u*(n==0u?1u:(n+15u)/16u);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+bytes);
        CHECK_EQ_U32(load(STREAM),0x41EA4u);CHECK_EQ_U32(load(STREAM+4u),0u);
        CHECK_EQ_U32(load(STREAM+8u),((n<16u?n:16u)<<18u)|0xB80u);
        if(n!=0u)CHECK_EQ_U32(load(STREAM+12u),load(SOURCE));
        for(unsigned i=0u;i<768u;i++)CHECK_EQ_U32(load(D3D8_CONSTANT_SHADOW+i*4u),0xAAAAAAAAu);
    }
    for(unsigned i=0u;i<768u;i++)store(D3D8_CONSTANT_SHADOW+i*4u,0x70000000u+i);
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    CHECK_EQ_U32(d3d8_upload_vertex_constants_unshadowed(0u,D3D8_CONSTANT_SHADOW+16u*5u,8u),0x200B80u);
    CHECK_EQ_U32(load(STREAM+12u),0x70000000u+20u);CHECK_EQ_U32(load(STREAM+40u),0x70000000u+27u);
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    store(D3D8_DEVICE_BASE+8u,0x4203u);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,D3D8_CONSTANT_SHADOW+16u*5u,8u));CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address,0x3D58B0u);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants_unshadowed(192u,SOURCE,0u));CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address,0x3D57D0u);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants_unshadowed(0u,STREAM,8u));CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address,0x3D57D0u);unchanged();
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    store(D3D8_DEVICE_BASE+8u,0x4203u);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(192u,SOURCE,0u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(UINT32_MAX,SOURCE,8u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(191u,SOURCE,5u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,0xFFFFFFF0u,8u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,STREAM,8u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,D3D8_CONSTANT_SHADOW,8u));CHECK(fatal_seen);unchanged();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,D3D8_DEVICE_BASE+0x2400u,8u));CHECK(fatal_seen);unchanged();
    const uint32_t badcursor[]={D3D8_CONSTANT_SHADOW,D3D8_DEVICE_BASE+0x2000u,0x3E3F38u,0xFFFFFFF0u};
    for(unsigned i=0u;i<4u;i++){
        store(D3D8_DEVICE_BASE,badcursor[i]);store(D3D8_DEVICE_BASE+4u,UINT32_MAX);snapshot();
        RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,SOURCE,8u));CHECK(fatal_seen);unchanged();
    }
    /* T391: a packet that runs past the limit into the slack is written, as the original does: its only checks
     * against the limit are the entry (cursor >= limit) and between two FULL chunks, never before a last partial
     * chunk. 44 bytes against a limit 43 bytes out. */
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+43u);
    CHECK_EQ_U32(d3d8_upload_vertex_constants(0u,SOURCE,8u),0x200B80u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+44u);CHECK_EQ_U32(load(STREAM),0x41EA4u);
    CHECK_EQ_U32(load(STREAM+40u),load(SOURCE+28u));
    /* T487: between two full chunks the original checks the cursor against the limit and refills. 33 dwords is two
     * full chunks and one dword, the cursor after the first is STREAM + 8 + 68 and a second full chunk follows. With
     * no ring (device+0x24 is 0 here) that refill is the original's own fatal case, refused before any write, at the
     * limit and one byte below it, and the packet is written one byte above (the last, one dword chunk is not
     * re-checked). The refill itself is tested below over a real ring and against the original in
     * tests/test_d3d8_vertex_constants_refill_oracle.py. */
    for(unsigned i=0u;i<3u;i++){
        const uint32_t limits[3]={STREAM+76u,STREAM+75u,STREAM+77u};
        store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,limits[i]);snapshot();
        if(i<2u){
            RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,SOURCE,33u));CHECK(fatal_seen);
            CHECK(strstr(fatal_text,"CreateDevice")!=NULL);unchanged();
        }
        else{CHECK_EQ_U32(d3d8_upload_vertex_constants(0u,SOURCE,33u),0x40B80u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+152u);}
    }
    /* A last partial chunk is never re-checked, so 17 dwords against a limit already passed by the first chunk is
     * still written. */
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+1u);
    CHECK_EQ_U32(d3d8_upload_vertex_constants(0u,SOURCE,17u),0x40B80u|0u);
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    store(D3D8_DEVICE_BASE+4u,STREAM);snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,SOURCE,8u));CHECK(fatal_seen);unchanged();
    store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);snapshot();
    const uint32_t pages[]={STREAM,0x3E2000u,0x3E3000u};
    for(unsigned i=0u;i<3u;i++){
        CHECK(mprotect((void *)(uintptr_t)pages[i],0x1000u,PROT_READ)==0);
        RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,SOURCE,8u));CHECK(fatal_seen);
        CHECK(mprotect((void *)(uintptr_t)pages[i],0x1000u,PROT_READ|PROT_WRITE)==0);unchanged();
    }
    CHECK(mprotect((void *)(uintptr_t)SOURCE,0x1000u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,SOURCE,8u));CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)SOURCE,0x1000u,PROT_READ|PROT_WRITE)==0);unchanged();
    /* Pure-device bit skips shadow permissions and leaves its bytes unchanged. */
    store(D3D8_DEVICE_BASE+8u,0x4213u);
    const uint32_t old=load(D3D8_CONSTANT_SHADOW);
    CHECK(mprotect((void *)(uintptr_t)0x3E2000u,0x1000u,PROT_READ)==0);
    CHECK_EQ_U32(d3d8_upload_vertex_constants(0u,SOURCE,8u),0x200B80u);
    CHECK_EQ_U32(load(D3D8_CONSTANT_SHADOW),old);
    CHECK(mprotect((void *)(uintptr_t)0x3E2000u,0x1000u,PROT_READ|PROT_WRITE)==0);
    store(D3D8_DEVICE_BASE,STREAM);
    CHECK_EQ_U32(d3d8_upload_vertex_constants(191u,UINT32_MAX,0u),0xB80u);
    const d3d8_surface_entry row[2]={{0x3D58B0u,"vertex constants",1u},{0x3D57D0u,"vertex constants",1u}};
    CHECK(d3d8_hle_init(row,2u));CHECK_EQ_U32(d3d8_vertex_constants_register(),2u);
    const uint32_t count=8u;kernel_call_frame frame;
    memset(&frame,0,sizeof(frame));CHECK(kernel_frame_build(&frame,call_scratch,0x100u,&count,1u));
    RUN_EXPECTING_FATAL((void)d3d8_hle_call(0x3D58B0u,&frame));CHECK(fatal_seen);
    kernel_frame_set_registers(&frame,58u,SOURCE);
    CHECK_EQ_U32(d3d8_hle_call(0x3D58B0u,&frame),0x200B80u);
    /* T734: the plain 0x003D57D0 is routed to its own handler (index 58 is the shadow slot 58, left alone). */
    store(D3D8_DEVICE_BASE,STREAM);store(D3D8_DEVICE_BASE+8u,0x4203u);
    store(D3D8_CONSTANT_SHADOW+58u*16u,0x5A5A5A5Au);
    CHECK_EQ_U32(d3d8_hle_call(0x3D57D0u,&frame),0x200B80u);
    CHECK_EQ_U32(load(STREAM+4u),58u);CHECK_EQ_U32(load(STREAM+12u),load(SOURCE));
    CHECK_EQ_U32(load(D3D8_CONSTANT_SHADOW+58u*16u),0x5A5A5A5Au);
    d3d8_hle_shutdown();environment_end();
    /* T391: at the limit the original's entry rolls the ring over (0x003D57DE, `jae 0x003D5890`) and writes at the
     * new cursor. The refill arithmetic is tested in test_d3d8_pushbuffer and against the original in
     * tests/test_d3d8_refill_oracle.py, here the emitter must USE it: the packet lands at the cursor the roll-over
     * leaves, the limit moved, and a roll-over that is fatal (the ring does not exist) refuses before any write. */
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(SOURCE,0x1000u);
    for(unsigned i=0u;i<64u;i++)store(SOURCE+i*4u,0x50607080u+i);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    CHECK(d3d8_pushbuffer_create());
    const uint32_t ring=load(D3D8_DEVICE_BASE+0x24u);
    store(D3D8_DEVICE_BASE+8u,0x4203u);
    store(D3D8_DEVICE_BASE,ring+0x1000u);store(D3D8_DEVICE_BASE+4u,ring+0x1000u);
    CHECK_EQ_U32(d3d8_upload_vertex_constants(4u,SOURCE,8u),0x200B80u);
    CHECK(d3d8_pushbuffer_rollovers()==1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),ring+0x1000u+0x10000u-0x204u);
    CHECK_EQ_U32(load(ring+0x1000u),0x41EA4u);CHECK_EQ_U32(load(ring+0x1004u),4u);
    CHECK_EQ_U32(load(ring+0x1008u),0x200B80u);CHECK_EQ_U32(load(ring+0x100Cu),load(SOURCE));
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),ring+0x1000u+44u);
    store(D3D8_DEVICE_BASE,ring+0x1000u);store(D3D8_DEVICE_BASE+4u,ring+0x1000u);
    store(D3D8_DEVICE_BASE+0x24u,0u); /* no ring: the roll-over is fatal */
    store(ring+0x1000u,0xDEADBEEFu);
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(4u,SOURCE,8u));CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address,0x003D6B20u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),ring+0x1000u);CHECK_EQ_U32(load(ring+0x1000u),0xDEADBEEFu);
    environment_end();
    /* T487: the refill between two chunks, over a real ring and no GPU module (no fence packet, so the second run
     * starts at the cursor the refill is called with). 33 dwords at ring + 0x1000 with the limit at the first
     * boundary (8 + 68 bytes in): the first run is in the ring when the consumer is handed it, the limit moves to
     * the split cursor + kickoff - 0x204 and the rest of the packet follows. */
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(SOURCE,0x1000u);
    for(unsigned i=0u;i<768u;i++)store(SOURCE+i*4u,0x50607080u+i);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    CHECK(d3d8_pushbuffer_create());
    uint32_t base=0u,first=0u;
    store(D3D8_DEVICE_BASE+8u,0x4203u);
    for(unsigned pass=0u;pass<4u;pass++){
        /* the limit at the boundary, one below, one above (no refill), and below the first chunk */
        const uint32_t offsets[4]={76u,75u,77u,8u};
        d3d8_pushbuffer_reset();CHECK(d3d8_pushbuffer_create());
        base=load(D3D8_DEVICE_BASE+0x24u);first=base+0x1000u;
        const uint32_t limit=first+offsets[pass];
        store(D3D8_DEVICE_BASE,first);store(D3D8_DEVICE_BASE+4u,limit);
        d3d8_pushbuffer_drain();
        for(unsigned i=0u;i<64u;i++)store(first+i*4u,0xCCCCCCCCu);
        recorded=0u;d3d8_pushbuffer_set_consumer(record,NULL);
        CHECK_EQ_U32(d3d8_upload_vertex_constants(4u,SOURCE,33u),0x40B80u);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE),first+152u);
        for(unsigned i=0u;i<33u;i++){
            /* the chunk headers at dwords 2 and 19 (a split puts nothing between them without the GPU module) */
            const uint32_t at=first+8u+i*4u+(i/16u+1u)*4u;
            CHECK_EQ_U32(load(at),0x50607080u+i);
        }
        CHECK_EQ_U32(load(first),0x41EA4u);CHECK_EQ_U32(load(first+4u),4u);
        CHECK_EQ_U32(load(first+8u),0x400B80u);CHECK_EQ_U32(load(first+76u),0x400B80u);
        CHECK_EQ_U32(load(first+144u),0x40B80u);
        if(pass==2u){
            CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);CHECK_EQ_U32(recorded,0u);
            CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),limit);
            continue;
        }
        /* pass 3: the limit is below the first chunk's end, so the check after chunk 1 refills as well */
        CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);CHECK_EQ_U32(recorded,1u);
        CHECK_EQ_U32(recorded_begin,first);CHECK_EQ_U32(recorded_end,first+76u);
        CHECK_EQ_U32(recorded_last,load(first+76u-4u));
        CHECK_EQ_U32(recorded_complete,1u);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),first+76u+0x10000u-0x204u);
    }
    /* The spans a split writes are checked against what the port reads before it writes: a source that is the second
     * run, a source that is the jump word a wrapping refill writes at the end of the first run, and a second run that
     * lands on the constant shadow. Each is refused before any write. */
    store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    d3d8_pushbuffer_reset();CHECK(d3d8_pushbuffer_create());
    base=load(D3D8_DEVICE_BASE+0x24u);first=base+0x1000u;
    store(D3D8_DEVICE_BASE,first);store(D3D8_DEVICE_BASE+4u,first+76u);
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,first+76u,33u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),first);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);
    const uint32_t wrap_cursor=base+0x100000u-0x2000u;
    store(D3D8_DEVICE_BASE,wrap_cursor);store(D3D8_DEVICE_BASE+4u,wrap_cursor+76u);
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,wrap_cursor+76u,33u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),wrap_cursor);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);
    /* the same wrap with the source elsewhere is written: the jump word lands at the end of the first run */
    CHECK_EQ_U32(d3d8_upload_vertex_constants(0u,SOURCE,33u),0x40B80u);
    CHECK_EQ_U32(load(wrap_cursor+76u),(base&0x0FFFFFFFu)+1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),base+152u-76u);
    CHECK_EQ_U32(load(base),0x400B80u);CHECK_EQ_U32(load(base+4u),load(SOURCE+16u*4u));
    store(D3D8_DEVICE_BASE,D3D8_CONSTANT_SHADOW-76u);store(D3D8_DEVICE_BASE+4u,D3D8_CONSTANT_SHADOW);
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_constants(0u,SOURCE,33u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),D3D8_CONSTANT_SHADOW-76u);
    /* T526: more than one refill inside the packet. 748 dwords (46 full chunks and a tail) against a 0x400 byte
     * kickoff: the limit is 100 bytes in, so the check after chunk 2 (8 + 2 * 68 = 144) refills, and then every
     * 0x1FC bytes (the next limit is the cursor + 0x400 - 0x204), which a chunk boundary passes 8 chunks (544 bytes) on:
     * after chunks 2, 10, 18, 26, 34 and 42, six refills, never after chunk 45 (the last full one is not checked). No
     * GPU module is attached, so no fence packet and the runs are contiguous. */
    d3d8_pushbuffer_set_consumer(NULL,NULL);
    store(D3D8_GLOBAL_KICKOFF_SIZE,0x400u);
    d3d8_pushbuffer_reset();CHECK(d3d8_pushbuffer_create());
    base=load(D3D8_DEVICE_BASE+0x24u);first=base+0x1000u;
    for(unsigned pass=0u;pass<2u;pass++){
        /* pass 1: the cursor is at the limit, so the entry roll-over comes first (limit 0x1FC on) and the splits
         * follow after chunks 8, 16, 24, 32 and 40 */
        store(D3D8_DEVICE_BASE,first);store(D3D8_DEVICE_BASE+4u,pass==0u?first+100u:first);
        d3d8_pushbuffer_drain();
        for(unsigned i=0u;i<900u;i++)store(first+i*4u,0xDDDDDDDDu);
        const uint64_t rollovers_before=d3d8_pushbuffer_rollovers();
        CHECK_EQ_U32(d3d8_upload_vertex_constants(5u,SOURCE,748u),(12u<<18)|0xB80u);
        CHECK_EQ_U32((uint32_t)(d3d8_pushbuffer_rollovers()-rollovers_before),6u);
        CHECK_EQ_U32(load(first),0x41EA4u);CHECK_EQ_U32(load(first+4u),5u);
        uint32_t at=first+8u;
        for(unsigned chunk=0u;chunk<47u;chunk++){
            const unsigned size=chunk<46u?16u:12u;
            CHECK_EQ_U32(load(at),(size<<18)|0xB80u);
            for(unsigned i=0u;i<size;i++)CHECK_EQ_U32(load(at+4u+i*4u),0x50607080u+chunk*16u+i);
            at+=4u+size*4u;
        }
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE),at);CHECK_EQ_U32(at,first+3188u);
        CHECK_EQ_U32(load(at),0xDDDDDDDDu);
        CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),first+(pass==0u?144u+5u*544u+0x1FCu:552u+4u*544u+0x1FCu));
    }
    environment_end();
    printf("%d checks, %d failures\n",checks,failures);return failures==0?0:1;
}
