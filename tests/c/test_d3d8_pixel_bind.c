/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define SOURCE 0x00D03000u
#define STREAM 0x00D00000u
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(STREAM,0x5000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE);store(D3D8_DEVICE_BASE,STREAM);
    store(D3D8_DEVICE_BASE+4u,STREAM+0x1000u);
    for(unsigned i=0u;i<60u;i++)store(SOURCE+4u*i,i*13u+7u);
}
static void valid(void)
{
    initialise();
    CHECK_EQ_U32(d3d8_set_pixel_shader(SOURCE),STREAM+336u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x784u),D3D8_DEVICE_BASE+0x924u);
    for(unsigned i=0u;i<57u;i++)CHECK_EQ_U32(load(0x3E3CC0u+4u*i),i*13u+7u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(SOURCE),STREAM+588u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(0u),STREAM+748u);
    environment_end();
}
static void protected_output(uint32_t page,bool null_shader)
{
    initialise();uint8_t state[0x30000],stream[4096];
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    memcpy(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream));
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(null_shader?0u:SOURCE));
    CHECK(fatal_seen);CHECK_EQ_U32(fatal_address,0x3D92E0u);
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(memcmp(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream))==0);
    environment_end();
}
static void refused(void)
{
    initialise();uint8_t state[0x30000];
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    CHECK(mprotect((void *)(uintptr_t)SOURCE,4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(SOURCE));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_v(SOURCE));CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)SOURCE,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)(SOURCE+0x1000u),4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(SOURCE+0xFC0u));CHECK(fatal_seen);
    CHECK(mprotect((void *)(uintptr_t)(SOURCE+0x1000u),4096u,PROT_READ|PROT_WRITE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(0x3E3CC0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(STREAM));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(UINT32_MAX-15u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader_v(0x3E3B18u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(D3D8_DEVICE_BASE+D3D8_DEV_PB_BASE,STREAM+4u);
    store(D3D8_DEVICE_BASE+D3D8_DEV_PB_END,STREAM+0x1000u);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(SOURCE));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(D3D8_DEVICE_BASE+D3D8_DEV_PB_BASE,0u);
    store(D3D8_DEVICE_BASE+D3D8_DEV_PB_END,0u);
    store(D3D8_DEVICE_BASE+4u,STREAM+68u);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(0u));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(D3D8_DEVICE_BASE,UINT32_MAX-64u);store(D3D8_DEVICE_BASE+4u,UINT32_MAX);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(SOURCE));CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    environment_end();
}
/* T525: a shader is one site (the check at 0x003D93F5, then the rows, blocks and input packet in one run: 336 bytes
 * from the null shader with the input words), the null shader two (the factor restore of 68 bytes, 0x003D7150, then the
 * rows and the other-stage word, 92 bytes, behind the check at 0x003D9374). Over a real ring with no GPU module (no
 * fence packet): the limit swept across the sites. The same against the original, fence packets, wraps and waits
 * included, is tests/test_d3d8_pixel_refill_oracle.py. */
static uint32_t recorded_begin,recorded_end;static unsigned recorded;
static void record(void *context,uint32_t begin,uint32_t end)
{
    (void)context;recorded_begin=begin;recorded_end=end;recorded++;
}
static uint32_t ringed(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(STREAM,0x5000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    CHECK(d3d8_pushbuffer_create());
    d3d8_pushbuffer_set_consumer(record,NULL);recorded=0u;
    for(unsigned i=0u;i<60u;i++)store(SOURCE+4u*i,i*13u+7u);
    return load(D3D8_DEVICE_BASE+0x24u);
}
static void place(uint32_t cursor,uint32_t limit)
{
    store(D3D8_DEVICE_BASE,cursor);store(D3D8_DEVICE_BASE+4u,limit);d3d8_pushbuffer_drain();
    recorded=0u;
}
static void refills_per_site(void)
{
    uint32_t base=ringed(),first=base+0x1000u;
    /* a shader from the null binding: one site of 336 bytes, so any limit above the cursor does not refill */
    place(first,first+1u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(SOURCE),first+336u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);
    environment_end();
    base=ringed();first=base+0x1000u;place(first,first);
    CHECK_EQ_U32(d3d8_set_pixel_shader(SOURCE),first+336u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);CHECK_EQ_U32(recorded,0u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),first+0x10000u-0x204u);
    environment_end();
    /* the null shader: two sites. A limit at the cursor refills before the first (and not before the second, the new
     * limit being far); at the cursor plus 68 before the second only, handing over exactly the first site */
    base=ringed();first=base+0x1000u;place(first,first);
    CHECK_EQ_U32(d3d8_set_pixel_shader(0u),first+160u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);CHECK_EQ_U32(recorded,0u);
    environment_end();
    base=ringed();first=base+0x1000u;place(first,first+68u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(0u),first+160u);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),1u);
    CHECK_EQ_U32(recorded,1u);CHECK_EQ_U32(recorded_begin,first);CHECK_EQ_U32(recorded_end,first+68u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+4u),first+68u+0x10000u-0x204u);
    CHECK_EQ_U32(load(first),0x00400A60u);CHECK_EQ_U32(load(first+68u),0x00181B68u);
    environment_end();
    base=ringed();first=base+0x1000u;place(first,first+69u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(0u),first+160u);CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);
    environment_end();
    /* a kickoff of 0x224 leaves the cursor at the new limit after the first site's refill, so both sites refill */
    base=ringed();first=base+0x1000u;store(D3D8_GLOBAL_KICKOFF_SIZE,0x224u);place(first,first);
    CHECK_EQ_U32(d3d8_set_pixel_shader(0u),first+160u);CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),2u);
    CHECK_EQ_U32(recorded,1u);CHECK_EQ_U32(recorded_begin,first);CHECK_EQ_U32(recorded_end,first+68u);
    environment_end();
    /* a wrap: the shader's one site is at the ring's base, the jump word at the old cursor, which a definition there
     * would be overwritten by before the original reads it: refused before any write */
    base=ringed();uint32_t wrap=base+0x100000u-0x100u;
    for(unsigned i=0u;i<60u;i++)store(wrap+4u*i,i*13u+7u);
    place(wrap,wrap);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(wrap));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),wrap);
    CHECK_EQ_U32(d3d8_pushbuffer_rollovers(),0u);CHECK_EQ_U32(load(wrap),7u);
    environment_end();
    /* the second site of the null shader is a span of its own: the first ends just below the D3D region and the
     * second starts at its first byte, so only the second overlaps it (no ring: nothing else bounds it) */
    initialise();map_fixed(0x3CF000u,0x1000u);
    store(D3D8_DEVICE_BASE,0x3D0000u-68u);store(D3D8_DEVICE_BASE+4u,0x3D1000u);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(0u));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),0x3D0000u-68u);
    environment_end();
    /* a shader's site that would run past the ring's end is refused (the page after the ring is mapped, so it is the
     * extent check and not the mapping); one that ends exactly there is written */
    base=ringed();map_fixed(base+0x100000u,0x1000u);
    const uint32_t end=base+0x100000u;
    place(end-336u+4u,end+0x800u);
    RUN_EXPECTING_FATAL((void)d3d8_set_pixel_shader(SOURCE));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"aliases")!=NULL);CHECK_EQ_U32(load(D3D8_DEVICE_BASE),end-336u+4u);
    place(end-336u,end+0x800u);
    CHECK_EQ_U32(d3d8_set_pixel_shader(SOURCE),end);
    environment_end();
    /* with the definition elsewhere the same wrap is made */
    base=ringed();wrap=base+0x100000u-0x100u;place(wrap,wrap);
    CHECK_EQ_U32(d3d8_set_pixel_shader(SOURCE),base+336u);
    CHECK_EQ_U32(load(wrap),(base&0x0FFFFFFFu)+1u);
    environment_end();
}
int main(void)
{
    valid();protected_output(0x3E3000u,false);protected_output(0x3E4000u,false);
    protected_output(STREAM,false);protected_output(0x3E3000u,true);refused();refills_per_site();
    printf("%d checks, %d failures\n",checks,failures);return failures!=0;
}
