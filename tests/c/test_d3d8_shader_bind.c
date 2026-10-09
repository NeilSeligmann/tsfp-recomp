/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define SOURCE 0x00D03000u
#define STREAM 0x00D00000u
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV); map_fixed(STREAM,0x5000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE); store(D3D8_DEVICE_BASE,STREAM);
    store(D3D8_DEVICE_BASE+4u,STREAM+0x1000u);
    store(D3D8_DEVICE_BASE+0x794u,0x3E2D80u);
    store(0x3E2D84u,0x10u); store(0x3E2C6Cu,0x10u);
    for (unsigned i=0u;i<64u;i++) store(SOURCE+4u*i,i*17u);
}
static void valid(void)
{
    initialise();
    CHECK_EQ_U32(d3d8_set_vertex_shader(SOURCE,23u),STREAM+8u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x794u),0x3E2C68u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK),0x70u);
    for (unsigned i=0u;i<64u;i++) CHECK_EQ_U32(load(0x3E2C7Cu+4u*i),i*17u);
    CHECK_EQ_U32(d3d8_set_vertex_shader(0u,7u),STREAM+16u);
    store(0x3E2D84u,0u);store(D3D8_DEVICE_BASE+0x794u,0x3E2D80u);
    CHECK_EQ_U32(d3d8_set_vertex_shader(SOURCE,8u),STREAM+88u);
    environment_end();
}
static void protected_output(uint32_t page, bool changed)
{
    initialise();
    uint8_t state[0x30000],stream[4096];
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    memcpy(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream));
    if (changed) { store(0x3E2D84u,0u); memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state)); }
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE,23u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address,0x3D5630u);
    CHECK(mprotect((void *)(uintptr_t)page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(memcmp(stream,kernel_guest_at(STREAM,sizeof(stream)),sizeof(stream))==0);
    environment_end();
}
static void invalid_inputs(void)
{
    initialise();
    uint8_t state[0x30000]; memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    CHECK(mprotect((void *)(uintptr_t)SOURCE,4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE,0u)); CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(mprotect((void *)(uintptr_t)SOURCE,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(mprotect((void *)(uintptr_t)(SOURCE+0x1000u),4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE+0xFC0u,0u)); CHECK(fatal_seen);
    store(D3D8_DEVICE_BASE+0x794u,SOURCE+0x1000u);
    uint8_t changed_state[0x30000];
    memcpy(changed_state,kernel_guest_at(0x3D0000u,sizeof(changed_state)),sizeof(changed_state));
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE,0u)); CHECK(fatal_seen);
    CHECK(memcmp(changed_state,kernel_guest_at(0x3D0000u,sizeof(changed_state)),sizeof(changed_state))==0);
    store(D3D8_DEVICE_BASE+0x794u,0x3E2D80u);
    CHECK(mprotect((void *)(uintptr_t)(SOURCE+0x1000u),4096u,PROT_READ|PROT_WRITE)==0);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(0x3E2C7Cu,0u)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader_handle(UINT32_MAX,0u)); CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader_handle(0x3E3AB5u,0u)); CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(0x3E2D84u,0u);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    uint16_t control, altered;
    __asm__ volatile("fnstcw %0":"=m"(control));
    altered=(uint16_t)((control&~0xC00u)|0x400u);
    __asm__ volatile("fldcw %0"::"m"(altered));
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE,0u));
    __asm__ volatile("fldcw %0"::"m"(control));
    CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    store(D3D8_DEVICE_BASE+4u,STREAM);
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE,0u)); CHECK(fatal_seen);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    environment_end();
}
/* The binder has two reservation preambles (T368): before the mode-change packets when the shader
 * class changed (44 bytes for the fixed-function class, 0x003D7860 and the 12-byte write have none
 * of their own) and before the 8-byte handle write. A roll-over falls at the first one whose cursor
 * is at the limit, and with device flag 4 it refuses BEFORE either packet is written. */
#define RING 0x00E00000u
static uint32_t refill_state(uint32_t old_flags, uint32_t new_flags, uint32_t back, uint32_t flags)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(STREAM,0x5000u); map_fixed(RING,0x40000u);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(D3D8_DEVICE_BASE+0x24u,RING); store(D3D8_DEVICE_BASE+0x28u,RING+0x40000u);
    store(0x3E6400u,0x8000u); store(D3D8_DEVICE_BASE+8u,flags);
    const uint32_t limit=RING+0x4000u;
    store(D3D8_DEVICE_BASE,limit-back); store(D3D8_DEVICE_BASE+4u,limit);
    store(D3D8_DEVICE_BASE+0x794u,0x3E2D80u);
    store(0x3E2D84u,old_flags); store(0x3E2C6Cu,new_flags);
    for (unsigned i=0u;i<64u;i++) store(SOURCE+4u*i,i*17u);
    store(SOURCE+4u,new_flags);
    return limit;
}
static void command_span_inside_the_state_refuses(void)
{
    /* The 8-byte handle write is a site of its own, so a cursor whose span lies in the recovered D3D
     * state refuses before anything is written. MUTATION: planning the site with no bytes lets the
     * overlap test pass and the handle is written over device state. */
    initialise();
    store(D3D8_DEVICE_BASE,0x3E5000u); store(D3D8_DEVICE_BASE+4u,0x3E6000u);
    uint8_t state[0x30000]; memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(0u,7u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address,0x3D5630u);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    environment_end();
}
static void refill_sites(void)
{
    static const struct { uint32_t back, roll_offset; } cases[]={
        {0u,0u},{4u,44u},{40u,44u},{44u,44u},{48u,UINT32_MAX},{52u,UINT32_MAX}};
    for (unsigned i=0u;i<sizeof(cases)/sizeof(cases[0]);i++) {
        /* A changed class (0x10 to 0): site 1 is 44 bytes, site 2 starts 44 bytes on. */
        const uint32_t limit=refill_state(0x10u,0u,cases[i].back,0u);
        const uint32_t cursor=limit-cases[i].back;
        const uint32_t result=d3d8_set_vertex_shader(SOURCE,23u);
        if (cases[i].roll_offset==UINT32_MAX) {
            CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT),limit);
        } else {
            /* MUTATION: one reservation for both sites leaves the old limit or the wrong one. */
            CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT),cursor+cases[i].roll_offset+0x8000u-0x204u);
        }
        CHECK_EQ_U32(result,cursor+44u+8u);
        CHECK_EQ_U32(load(cursor+44u),0x41EA0u);
        CHECK_EQ_U32(load(cursor+48u),23u);
        environment_end();
    }
    /* An unchanged class has only the 8-byte site. */
    uint32_t limit=refill_state(0u,0u,0u,0u);
    CHECK_EQ_U32(d3d8_set_vertex_shader(0u,7u),limit+8u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT),limit+0x8000u-0x204u);
    environment_end();
    limit=refill_state(0u,0u,4u,0u);
    CHECK_EQ_U32(d3d8_set_vertex_shader(0u,7u),limit-4u+8u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_LIMIT),limit);
    environment_end();
    /* Flag 4: a refill at the SECOND site still refuses before the first site is written. */
    limit=refill_state(0x10u,0u,4u,4u);
    uint8_t state[0x30000],ring[256];
    memcpy(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state));
    memcpy(ring,kernel_guest_at(limit-4u,sizeof(ring)),sizeof(ring));
    RUN_EXPECTING_FATAL((void)d3d8_set_vertex_shader(SOURCE,23u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address,0x3D69E0u);
    CHECK(memcmp(state,kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state))==0);
    CHECK(memcmp(ring,kernel_guest_at(limit-4u,sizeof(ring)),sizeof(ring))==0);
    environment_end();
}
int main(void)
{
    refill_sites(); command_span_inside_the_state_refuses();
    valid(); protected_output(0x3E2000u,false); protected_output(0x3E3000u,false);
    protected_output(0x3E4000u,false); protected_output(STREAM,false);
    protected_output(0x3E5000u,true); invalid_inputs();
    printf("%d checks, %d failures\n",checks,failures); return failures!=0;
}
