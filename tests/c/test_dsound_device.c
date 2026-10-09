/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <pthread.h>
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#define SLOT 0x412B30u
#define OUT SCRATCH_DATA
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV); dsound_hle_init(); dsound_device_reset();
    dsound_device_set_fatal(catching_fatal);
    map_fixed(0x412000u,0x1000u);map_fixed(0x4A1000u,0x1000u);
    memset(kernel_guest_at(OUT,16u),0xAAu,16u);
}
static void check_output(void)
{ CHECK_EQ_U32(load(OUT),0xAAAAAAAAu); CHECK_EQ_U32(load(OUT+12u),0xAAAAAAAAu); }
static void test_cold_preflight_and_cache_tampering(void)
{
    initialise();dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    RUN_EXPECTING_FATAL((void)dsound_device_create(1u,OUT,0u));CHECK(fatal_seen);check_output();
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,OUT,1u));CHECK(fatal_seen);check_output();
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,0x412FFFu,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,SLOT,0u));CHECK(fatal_seen);
    store(SLOT,0x12345678u);
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,OUT,0u));CHECK(fatal_seen);check_output();
    CHECK_EQ_U32(guest_mem_heap_count(),0u);store(SLOT,0u);
    CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0u);
    const uint32_t address=load(SLOT);CHECK_EQ_U32(load(OUT),address+8u);
    CHECK_EQ_U32(load(address+4u),6u);memset(kernel_guest_at(OUT,16u),0xAAu,16u);
    for(unsigned i=0u;i<11u;i++) {
        const uint32_t saved=load(address+i*4u);store(address+i*4u,saved^1u);
        RUN_EXPECTING_FATAL((void)dsound_device_create(0u,OUT,0u));CHECK(fatal_seen);check_output();
        store(address+i*4u,saved);
    }
    for(unsigned i=0u;i<2u;i++) {
        store(address+4u,i==0u?0u:UINT32_MAX);
        RUN_EXPECTING_FATAL((void)dsound_device_create(0u,OUT,0u));CHECK(fatal_seen);check_output();
    }
    store(address+4u,6u);
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,address+4u,0u));CHECK(fatal_seen);
    CHECK_EQ_U32(load(address+4u),6u);
    store(SLOT,0u);
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,OUT,0u));CHECK(fatal_seen);check_output();
    store(SLOT,address);
    CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0u);CHECK_EQ_U32(load(address+4u),7u);
    const dsound_codec_state policy=dsound_hle_codec_state();dsound_device_reset();
    CHECK_EQ_U32(load(SLOT),0u);CHECK_EQ_U32(guest_mem_heap_count(),0u);
    CHECK(dsound_hle_codec_state()==policy);environment_end();
}
static void test_heap_failure_and_generation(void)
{
    initialise();uint32_t heaps[256];unsigned count=0u;
    while(count<256u && (heaps[count]=guest_heap_create(0u,0u,0u))!=0u)count++;
    CHECK_EQ_U32(count,256u);
    CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0x8007000Eu);check_output();
    CHECK_EQ_U32(load(SLOT),0u);
    for(unsigned i=0u;i<count;i++)CHECK(guest_heap_destroy(heaps[i]));
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0u);
    const uint32_t address=load(SLOT);
    /* Locate the actual owner without assuming a particular heap handle. */
    uint32_t owner=0u;
    for(uint32_t generation=1u;generation<16u;generation++)
        for(uint32_t slot=1u;slot<=256u;slot++) {
            const uint32_t handle=(generation<<12u)|slot;uint32_t bytes;
            if(guest_heap_block_size(handle,address,&bytes)&&bytes==44u)owner=handle;
        }
    CHECK(owner!=0u);CHECK(guest_heap_destroy(owner));
    const uint32_t replacement=guest_heap_create(0u,GUEST_HEAP_CHUNK_MIN,0u);
    CHECK(replacement!=owner);
    const uint32_t reused=guest_heap_alloc(replacement,44u);
    CHECK(reused!=0u);memset(kernel_guest_at(OUT,16u),0xAAu,16u);
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u,OUT,0u));CHECK(fatal_seen);check_output();
    dsound_device_reset();CHECK_EQ_U32(load(SLOT),address);
    CHECK(guest_heap_valid(replacement));CHECK(guest_heap_destroy(replacement));
    environment_end();
}
static void *cached_worker(void *argument)
{
    const uint32_t output=*(uint32_t *)argument;
    for(unsigned i=0u;i<50u;i++)if(dsound_device_create(0u,output,0u)!=0u)return (void *)1;
    return NULL;
}
static void test_dispatch_and_concurrent_cache(void)
{
    initialise();dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_register(),1u);
    const uint32_t args[3]={0u,OUT,0u};kernel_call_frame frame;memset(&frame,0,sizeof(frame));
    CHECK(kernel_frame_build(&frame,call_scratch+4u,0x100u,args,3u));
    store(frame.stack_ptr-4u,0x279D5u);store(frame.stack_ptr,0xBADu);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x409635u,&frame));CHECK(fatal_seen);
    CHECK_EQ_U32(load(SLOT),0u);check_output();
    store(frame.stack_ptr-4u,0xBADu);store(frame.stack_ptr,0x279D5u);
    frame.stack_limit=frame.stack_ptr+12u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x409635u,&frame));CHECK(fatal_seen);
    frame.stack_limit=frame.stack_ptr+16u;
    CHECK_EQ_U32(dsound_hle_call(0x409635u,&frame),0u);
    const uint32_t address=load(SLOT);const uint64_t calls=dsound_hle_entry(0x409635u)->call_count;
    uint32_t outputs[4]={OUT+0x40u,OUT+0x60u,OUT+0x80u,OUT+0xA0u};pthread_t workers[4];
    for(unsigned i=0u;i<4u;i++)CHECK(pthread_create(&workers[i],NULL,cached_worker,&outputs[i])==0);
    for(unsigned i=0u;i<4u;i++){void *result=NULL;CHECK(pthread_join(workers[i],&result)==0);CHECK(result==NULL);}
    CHECK_EQ_U32(load(address+4u),206u);
    for(unsigned i=0u;i<4u;i++)CHECK_EQ_U32(load(outputs[i]),address+8u);
    CHECK(!dsound_hle_dsp_ack_configured());CHECK_EQ_U32(dsound_hle_dsp_ack_count(),0u);
    dsound_device_reset();CHECK(dsound_hle_entry(0x409635u)->call_count==calls);
    CHECK(dsound_hle_entry(0x409635u)->state==DSOUND_ENTRY_IMPLEMENTED);
    environment_end();
}
int main(void)
{
    test_cold_preflight_and_cache_tampering();test_heap_failure_and_generation();
    test_dispatch_and_concurrent_cache();
    printf("%d checks, %d failures\n",checks,failures);return failures==0?0:1;
}
