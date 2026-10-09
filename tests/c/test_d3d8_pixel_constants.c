/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define SOURCE 0x00D03000u
#define DEFINITION 0x00D02000u
#define STREAM 0x00D00000u
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(STREAM,0x5000u);map_fixed(0x549000u,0x1000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE);store(D3D8_DEVICE_BASE,STREAM);
    store(D3D8_DEVICE_BASE+4u,STREAM+0x2000u);
    store(D3D8_DEVICE_BASE+0x784u,D3D8_DEVICE_BASE+0x924u);
    store(D3D8_DEVICE_BASE+0x92Cu,DEFINITION);
    for(unsigned i=0u;i<64u;i++)store(SOURCE+4u*i,0x3F000000u);
}
static void valid(void)
{
    initialise();CHECK_EQ_U32(d3d8_set_pixel_shader_constants(0u,SOURCE,2u),0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+304u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x8E4u),0x80808080u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x8E8u),0x80808080u);
    store(0x5496E0u+15u*4u,0xFFFFFFFFu);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(15u,SOURCE,1u),0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM+304u); /* no matching nibble */
    environment_end();
}
static void zero_count(void)
{
    initialise();uint8_t state[0x30000];memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    CHECK(mprotect((void *)(uintptr_t)SOURCE,4096u,PROT_NONE)==0);
    CHECK(mprotect((void *)(uintptr_t)DEFINITION,4096u,PROT_NONE)==0);
    CHECK(mprotect((void *)(uintptr_t)0x549000u,4096u,PROT_NONE)==0);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(UINT32_MAX,UINT32_MAX,0u),0u);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(mprotect((void *)(uintptr_t)0x3E3000u,4096u,PROT_READ)==0);
    CHECK(mprotect((void *)(uintptr_t)0x3E4000u,4096u,PROT_READ)==0);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(UINT32_MAX,UINT32_MAX,0u),0u);
    CHECK(mprotect((void *)(uintptr_t)0x3E3000u,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)0x3E4000u,4096u,PROT_READ|PROT_WRITE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,SOURCE,1u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(D3D8_DEVICE_BASE+0x784u,DEFINITION);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,0u,0u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(mprotect((void *)(uintptr_t)SOURCE,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)DEFINITION,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)0x549000u,4096u,PROT_READ|PROT_WRITE)==0);
    store(D3D8_DEVICE_BASE+0x784u,D3D8_DEVICE_BASE+0x924u);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    CHECK(mprotect((void *)(uintptr_t)DEFINITION,4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,SOURCE,2u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(mprotect((void *)(uintptr_t)DEFINITION,4096u,PROT_READ|PROT_WRITE)==0);
    environment_end();
}
static void protected_output(uint32_t page)
{
    initialise();store(D3D8_DEVICE_BASE,STREAM+0xFE0u);
    uint8_t state[0x30000],stream[8192];
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    memcpy(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream));
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,SOURCE,2u));
    CHECK(fatal_seen);CHECK_EQ_U32(fatal_address,0x3D9520u);
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(memcmp(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream))==0);
    environment_end();
}
static void invalid(void)
{
    initialise();uint8_t state[0x30000];memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(16u,SOURCE,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(15u,SOURCE,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,UINT32_MAX-15u,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,0x3E3CE8u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,STREAM,1u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(D3D8_DEVICE_BASE+4u,STREAM+160u);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,SOURCE,2u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    environment_end();
}
/* T525: the limit is checked once per VECTOR (0x003D95B5) and the refill falls between vectors. Over a real ring and
 * no GPU module (so no fence packet) with every nibble selecting (usage and map words zero), a vector is 144 bytes
 * (160 for register 0): the limit swept across the vector boundaries. The refill against the original, fence packets
 * and waits included, is tests/test_d3d8_pixel_refill_oracle.py. */
static uint32_t recorded_begin,recorded_end;static unsigned recorded;
static void record(void *context,uint32_t begin,uint32_t end)
{
    (void)context;recorded_begin=begin;recorded_end=end;recorded++;
}
static uint32_t ringed(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(STREAM,0x5000u);map_fixed(0x549000u,0x1000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    CHECK(d3d8_pushbuffer_create());
    d3d8_pushbuffer_set_consumer(record,NULL);recorded=0u;
    store(D3D8_DEVICE_BASE+0x784u,D3D8_DEVICE_BASE+0x924u);
    store(D3D8_DEVICE_BASE+0x92Cu,DEFINITION);
    for(unsigned i=0u;i<64u;i++)store(SOURCE+4u*i,0x3F000000u);
    return load(D3D8_DEVICE_BASE+0x24u);
}
static void place(uint32_t cursor,uint32_t limit)
{
    store(D3D8_DEVICE_BASE,cursor);store(D3D8_DEVICE_BASE+4u,limit);d3d8_pushbuffer_drain();
    recorded=0u;
}
static void refills_between_vectors(void)
{
    uint32_t base=ringed();uint32_t first=base+0x1000u;
    /* index 1, three vectors of 144 bytes: the limit AT the first boundary refills before vector 1 and the second run
     * follows the first without a gap (no fence packet), one hand-off of exactly vector 0 */
    place(first,first+144u);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u,SOURCE,3u),0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),first+3u*144u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);
    CHECK_EQ_U32(recorded,1u);CHECK_EQ_U32(recorded_begin,first);CHECK_EQ_U32(recorded_end,first+144u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),first+144u+0x10000u-0x204u);
    for(unsigned vector=0u;vector<3u;vector++)CHECK_EQ_U32(load(first+vector*144u),0x00040A60u);
    environment_end();
    /* one byte above the boundary: vector 1 is not refilled, vector 2 is (its cursor is at the limit's 288) */
    base=ringed();first=base+0x1000u;place(first,first+145u);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u,SOURCE,3u),0u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);
    CHECK_EQ_U32(recorded_end,first+288u);
    environment_end();
    /* above the last check: no refill */
    base=ringed();first=base+0x1000u;place(first,first+3u*144u);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u,SOURCE,3u),0u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),first+432u);
    environment_end();
    /* the limit AT the first byte: the first vector's own check refills, nothing handed over */
    base=ringed();first=base+0x1000u;place(first,first);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u,SOURCE,2u),0u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);CHECK_EQ_U32(recorded,0u);
    environment_end();
    /* register 0 adds the raw RGB: its vector is 160 bytes, so the boundary is 160 */
    base=ringed();first=base+0x1000u;place(first,first+160u);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(0u,SOURCE,2u),0u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);CHECK_EQ_U32(recorded_end,first+160u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),first+160u+144u);
    environment_end();
    /* a wrap: vector 0 ends 0x100 from the ring's end, the refill writes the ring jump word there and vector 1 starts at
     * the ring base */
    base=ringed();uint32_t wrap=base+0x100000u-0x100u;place(wrap,wrap+144u);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u,SOURCE,2u),0u);
    CHECK_EQ_U32(load(wrap+144u),(base&0x0FFFFFFFu)+1u);
    CHECK_EQ_U32(load(base),0x00040A60u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),base+144u);
    environment_end();
    /* the jump word is a span of its own: a source on it is refused before any write (vector 1 is at the base, so
     * nothing else overlaps the source: vectors 0 and 1 read at +0 and +16, the word is at wrap+144) */
    base=ringed();wrap=base+0x100000u-0x100u;place(wrap,wrap+144u);
    uint32_t stream[8];for(unsigned i=0u;i<8u;i++)stream[i]=load(wrap+144u+4u*i-16u);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(1u,wrap+144u-16u,2u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),wrap);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);CHECK_EQ_U32(load(wrap+144u),0u);
    for(unsigned i=0u;i<8u;i++)CHECK_EQ_U32(load(wrap+144u+4u*i-16u),stream[i]);
    environment_end();
    /* a vector that would run past the ring's end (the original lets it write beyond) is refused before any write,
     * with the page after the ring mapped so that it is the extent check and not the mapping that refuses */
    base=ringed();map_fixed(base+0x100000u,0x1000u);const uint32_t end=base+0x100000u;
    place(end-100u,end+0x800u);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(1u,SOURCE,1u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"exceeds the ring")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),end-100u);
    CHECK_EQ_U32(load(end-100u),0u);
    place(end-144u,end+0x800u);
    CHECK_EQ_U32(d3d8_set_pixel_shader_constants(1u,SOURCE,1u),0u); /* exactly to the end is fine */
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE),end);
    environment_end();
    /* no ring at all: the refill is the original's own fatal case, refused before any write */
    initialise();store(D3D8_DEVICE_BASE+4u,STREAM+160u);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_constants(0u,SOURCE,2u));CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address,0x3D6B20u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),STREAM);
    environment_end();
}
int main(void)
{
    valid();zero_count();protected_output(0x3E3000u);protected_output(0x3E4000u);
    protected_output(STREAM+0x1000u);invalid();refills_between_vectors();
    printf("%d checks, %d failures\n",checks,failures);return failures!=0;
}
