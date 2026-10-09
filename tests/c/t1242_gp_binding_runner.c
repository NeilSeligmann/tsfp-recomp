/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include <sys/mman.h>
#include "dsound_device.h"
#include "dsound_effects_binding.h"
#include "dsound_effects_dsp.h"
#include "dsound_effects_metadata.h"
#include "dsound_hle.h"
static unsigned fail_allocation,allocation_calls;
extern uint32_t __real_guest_heap_alloc(uint32_t heap,uint32_t bytes);
uint32_t __wrap_guest_heap_alloc(uint32_t owner,uint32_t bytes)
{
    if(fail_allocation!=0u&&++allocation_calls==fail_allocation)return 0u;
    return __real_guest_heap_alloc(owner,bytes);
}
static unsigned destroyed;
extern void __real_dsound_effects_dsp_destroy(dsound_effects_dsp *dsp);
void __wrap_dsound_effects_dsp_destroy(dsound_effects_dsp *dsp)
{if(dsp)destroyed++;__real_dsound_effects_dsp_destroy(dsp);}
static void file_into(const char *path,uint32_t address,size_t bytes)
{
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);
    CHECK(fread(kernel_guest_at(address,bytes),1u,bytes,f)==bytes);fclose(f);
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;setvbuf(stdout,NULL,_IONBF,0);
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_reset();
    dsound_effects_binding_reset();dsound_effects_binding_set_fatal(catching_fatal);
    map_fixed(0x412000u,0x1000u);map_fixed(0x4A1000u,0x1000u);map_fixed(0x7F7000u,0x6000u);
    file_into(argv[1],0x4124D0u,0x5CCu);file_into(argv[2],0x7F78C0u,18608u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    uint32_t out=SCRATCH_DATA,loc=out+32u,source=out+64u;
    CHECK_EQ_U32(dsound_device_create(0u,out,0u),0u);uint32_t iface=load(out);
    store(loc,3u);store(loc+4u,4u);store(out,0xAAAAAAAAu);
    dsound_effects_binding_set_enabled(true);CHECK(dsound_effects_binding_set_gp_enabled(true));
    size_t heaps=guest_mem_heap_count();
    uint8_t *firmware=kernel_guest_at(0x4124D0u,0x5CCu);firmware[0]^=1u;
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"bootstrap failed")!=NULL);
    CHECK_EQ_U32(load(out),0xAAAAAAAAu);CHECK_EQ_U32(guest_mem_heap_count(),heaps);
    firmware[0]^=1u;
    for(unsigned failure=1u;failure<=6u;failure++) {
        unsigned old=destroyed;fail_allocation=failure;allocation_calls=0u;
        CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0x8007000Eu);
        CHECK_EQ_U32(load(out),0xAAAAAAAAu);CHECK_EQ_U32(guest_mem_heap_count(),heaps);
        CHECK_EQ_U32(destroyed,old+1u);
    }
    fail_allocation=0u;
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0u);
    dsound_effects_binding_gp_view view;
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));
    CHECK_EQ_U32(view.descriptor,load(out));CHECK_EQ_U32(guest_mem_heap_count(),heaps+1u);
    CHECK_EQ_U32(view.pending_start,0x8000u);CHECK_EQ_U32(view.pending_bytes,0u);
    CHECK(!dsound_effects_binding_set_gp_enabled(false));
    CHECK_EQ_U32(dsound_effects_binding_register(),2u);
    uint32_t live=load(view.descriptor+8u+5u*32u+8u);
    uint32_t map_offset=live-view.state;
    uint32_t initial=load(live+0x20u);CHECK_EQ_U32(initial,0x3E8FA0u);
    store(0x4124A8u,1u);
    CHECK_EQ_U32(dsound_effects_binding_apply(0xBADu,UINT32_MAX,UINT32_MAX,0xBADu,UINT32_MAX,0u),0x80004005u);
    CHECK_EQ_U32(load(live+0x20u),initial);store(0x4124A8u,0u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,9u,UINT32_MAX,0xBADu,UINT32_MAX,0u),0x88780032u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x21u,0xDEADBEEFu,0u,0u),0u);
    CHECK_EQ_U32(load(live+0x20u),initial);
    store(source,0x400000u);
    kernel_call_frame frame={0};frame.stack_ptr=SCRATCH_DATA+0x800u;frame.stack_limit=frame.stack_ptr+28u;
    const uint32_t args[7]={0x38669Fu,iface,5u,0x20u,source,4u,0u};
    for(unsigned i=0;i<7u;i++)store(frame.stack_ptr+i*4u,args[i]);
    store(frame.stack_ptr,0xBADu);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407A02u,&frame));CHECK(fatal_seen);
    CHECK_EQ_U32(load(live+0x20u),initial);store(frame.stack_ptr,0x38669Fu);
    store(frame.stack_ptr+12u,0x24u);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x407A02u,&frame));CHECK(fatal_seen);
    CHECK_EQ_U32(load(live+0x20u),initial);store(frame.stack_ptr+12u,0x20u);
    CHECK_EQ_U32(dsound_hle_call(0x407A02u,&frame),0u);
    CHECK_EQ_U32(load(live+0x20u),0x400000u);
    CHECK_EQ_U32(load(view.staging+map_offset+0x20u),0x400000u);
    uint32_t input[1024]={0},output[1024];input[27u*32u]=0x1000u;
    for(unsigned frame=0u;frame<8u;frame++) {
        CHECK(dsound_effects_binding_gp_frame(iface,input,output));
        printf("signal %u %x\n",frame,output[27u*32u]);
    }
    store(source,0x123456u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x20u,source,4u,1u),0u);
    CHECK_EQ_U32(load(live+0x20u),0x400000u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));
    CHECK_EQ_U32(view.pending_start,0x27A8u+map_offset+0x20u);CHECK_EQ_U32(view.pending_bytes,4u);
    CHECK(dsound_effects_binding_gp_commit(iface));CHECK_EQ_U32(load(live+0x20u),0x123456u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));
    CHECK_EQ_U32(view.pending_start,0x8000u);CHECK_EQ_U32(view.pending_bytes,0u);
    /* RAM stage accepts byte tails; real firmware consumes only floor(count/4). */
    store(source,0x654321u);*((uint8_t *)kernel_guest_at(source+4u,1u))=0xCCu;
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x20u,source,5u,1u),0u);
    CHECK(dsound_effects_binding_gp_commit(iface));CHECK_EQ_U32(load(live+0x20u),0x654321u);
    CHECK_EQ_U32(*(uint8_t *)kernel_guest_at(view.staging+map_offset+0x24u,1u),0xCCu);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,0x21u,source,4u,0u));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"aligned four-byte")!=NULL);
    CHECK_EQ_U32(load(view.staging+map_offset+0x21u),0x654321u);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,0x20u,source,3u,0u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,155u,source,4u,1u));CHECK(fatal_seen);
    /* A real source-page fault preserves completed staging words and publishes
     * neither pending metadata nor the later live copy. */
    map_fixed(0x63000000u,0x2000u);store(0x63000FFCu,0x345678u);
    CHECK(mprotect((void *)(uintptr_t)0x63001000u,0x1000u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,0x20u,0x63000FFCu,8u,1u));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"source read fault")!=NULL);
    CHECK_EQ_U32(load(view.staging+map_offset+0x20u),0x345678u);
    CHECK_EQ_U32(load(live+0x20u),0x654321u);
    CHECK(mprotect((void *)(uintptr_t)0x63001000u,0x1000u,PROT_READ|PROT_WRITE)==0);
    store(source,0xFF123456u);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,0x20u,source,4u,0u));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"aligned24-bit")!=NULL);
    CHECK_EQ_U32(load(live+0x20u),0x654321u);
    CHECK_EQ_U32(load(view.staging+map_offset+0x20u),0xFF123456u);
    /* The first staging copy and second live source reread are distinct. */
    store(live+0x20u,0x123456u);store(live+0x24u,0x234567u);store(live+0x28u,0x345678u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x24u,live+0x20u,12u,0u),0u);
    for(unsigned i=0u;i<3u;i++)CHECK_EQ_U32(load(live+0x24u+i*4u),0x123456u);
    CHECK_EQ_U32(load(view.staging+map_offset+0x24u),0x123456u);
    CHECK_EQ_U32(load(view.staging+map_offset+0x28u),0x234567u);
    CHECK_EQ_U32(load(view.staging+map_offset+0x2Cu),0x345678u);
    /* Do not execute a program with deliberately overwritten opaque control fields. */
    dsound_effects_binding_reset();CHECK_EQ_U32(guest_mem_heap_count(),heaps);
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));live=load(view.descriptor+8u+5u*32u+8u);
    map_offset=live-view.state;
    uint32_t stage=view.staging+map_offset+0x20u;
    store(stage,0x123456u);store(stage+4u,0x234567u);store(stage+8u,0x345678u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x24u,stage,12u,0u),0u);
    for(unsigned i=0u;i<3u;i++)CHECK_EQ_U32(load(live+0x24u+i*4u),0x123456u);
    for(unsigned i=0u;i<3u;i++)CHECK_EQ_U32(load(stage+4u+i*4u),0x123456u);
    dsound_effects_binding_reset();
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));
    store(view.descriptor,0u);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,0u,0u,source,0u,0u));
    CHECK(fatal_seen);CHECK(strstr(fatal_text,"ownership changed")!=NULL);store(view.descriptor,9u);
    store(source,0x123456u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x20u,source,4u,1u),0u);
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0x20u,source,4u,1u),0u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));CHECK_EQ_U32(view.pending_bytes,8u);
    /* Actual duplicate transfer resends adjacent DMA runtime fields. Guard must
     * turn the archived pinned assertion into a failed consumer, never ACK. */
    CHECK(dsound_effects_binding_gp_commit(iface));
    memset(output,0xAA,sizeof(output));
    bool first=dsound_effects_binding_gp_frame(iface,input,output);
    bool second=first&&dsound_effects_binding_gp_frame(iface,input,output);
    CHECK(!first||!second);
    dsound_effects_binding_reset();
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));
    CHECK_EQ_U32(dsound_effects_binding_apply(iface,5u,0u,0xBADu,0u,1u),0u);
    CHECK(dsound_effects_binding_gp_commit(iface));
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));
    live=load(view.descriptor+8u+5u*32u+8u);map_offset=live-view.state;
    CHECK_EQ_U32(view.pending_start,0x27A8u+map_offset);CHECK_EQ_U32(view.pending_bytes,0u);
    CHECK(guest_heap_destroy(view.heap));uint32_t replacement=guest_heap_create(0u,0u,0u);
    CHECK(replacement!=view.heap);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,0x20u,source,4u,0u));CHECK(fatal_seen);
    dsound_effects_binding_reset();CHECK(guest_heap_valid(replacement));CHECK(guest_heap_destroy(replacement));
    /* Device recreation cannot attach the old GP even if virtual addresses recur. */
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));uint32_t old_device_heap=view.device_heap;
    dsound_device_reset();CHECK_EQ_U32(dsound_device_create(0u,out,0u),0u);iface=load(out);
    dsound_effects_binding_gp_view untouched={.heap=0xAAAAAAAAu};
    CHECK(!dsound_effects_binding_gp_view_get(iface,&untouched));CHECK_EQ_U32(untouched.heap,0xAAAAAAAAu);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_apply(iface,5u,0x20u,source,4u,0u));CHECK(fatal_seen);
    dsound_effects_binding_reset();
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,0x7F78C0u,18608u,loc,out),0u);
    CHECK(dsound_effects_binding_gp_view_get(iface,&view));CHECK(view.device_heap!=old_device_heap);
    dsound_effects_binding_reset();
    CHECK(!dsound_hle_dsp_ack_configured());CHECK_EQ_U32(dsound_hle_dsp_ack_count(),0u);
    dsound_device_reset();environment_end();
    printf("%d checks, %d failures, %u real cores destroyed\n",checks,failures,destroyed);
    return failures?1:0;
}
