/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/mman.h>
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_effects_binding.h"
#include "dsound_effects_metadata.h"
#include "dsound_hle.h"
static unsigned fail_allocation,allocation_calls;
extern uint32_t __real_guest_heap_alloc(uint32_t heap,uint32_t bytes);
uint32_t __wrap_guest_heap_alloc(uint32_t heap,uint32_t bytes)
{
    if(fail_allocation!=0u && ++allocation_calls==fail_allocation)return 0u;
    return __real_guest_heap_alloc(heap,bytes);
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");
    if(input==NULL||output==NULL)return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_reset();
    dsound_effects_binding_reset();dsound_effects_binding_set_fatal(catching_fatal);
    map_fixed(0x412000u,0x1000u);map_fixed(0x4A1000u,0x1000u);map_fixed(0x7F7000u,0x6000u);
    uint8_t *image=kernel_guest_at(DSOUND_EFFECTS_IMAGE_ADDRESS,DSOUND_EFFECTS_IMAGE_BYTES);
    if(fread(image,1u,DSOUND_EFFECTS_IMAGE_BYTES,input)!=DSOUND_EFFECTS_IMAGE_BYTES)return 2;
    fclose(input);dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    const uint32_t out=SCRATCH_DATA+1u,loc=SCRATCH_DATA+33u;
    CHECK_EQ_U32(dsound_device_create(0u,out,0u),0u);const uint32_t iface=load(out);
    store(loc,3u);store(loc+4u,4u);memset(kernel_guest_at(out,16u),0xAAu,16u);
    const size_t initial_heaps=guest_mem_heap_count();
    dsound_effects_binding_set_enabled(true);
    /* Allocation failure rolls back and preserves output/device; retries allowed. */
    uint32_t handles[256];unsigned count=0u;
    while(count<256u&&(handles[count]=guest_heap_create(0u,0u,0u))!=0u)count++;
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out),0x8007000Eu);CHECK_EQ_U32(load(out),0xAAAAAAAAu);
    for(unsigned i=0u;i<count;i++)CHECK(guest_heap_destroy(handles[i]));
    CHECK_EQ_U32(guest_mem_heap_count(),initial_heaps);
    for(unsigned failure=1u;failure<=5u;failure++) {
        fail_allocation=failure;allocation_calls=0u;
        CHECK_EQ_U32(dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
            DSOUND_EFFECTS_IMAGE_BYTES,loc,out),0x8007000Eu);
        CHECK_EQ_U32(load(out),0xAAAAAAAAu);CHECK_EQ_U32(guest_mem_heap_count(),initial_heaps);
    }
    fail_allocation=0u;
    uint8_t original[DSOUND_EFFECTS_IMAGE_BYTES];memcpy(original,image,sizeof(original));
    image[1]^=1u;
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);image[1]^=1u;
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,iface-4u));CHECK(fatal_seen);
    store(loc+4u,5u);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);store(loc+4u,4u);
    CHECK(mprotect(kernel_guest_at(SCRATCH_DATA,4096u),4096u,PROT_READ)==0);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"not writable")!=NULL);CHECK_EQ_U32(load(out),0xAAAAAAAAu);
    CHECK(mprotect(kernel_guest_at(SCRATCH_DATA,4096u),4096u,PROT_READ|PROT_WRITE)==0);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,SCRATCH_DATA+4094u));CHECK(fatal_seen);
    CHECK_EQ_U32(guest_mem_heap_count(),initial_heaps);
    kernel_call_frame frame={0};frame.stack_ptr=SCRATCH_DATA+0x800u;frame.stack_limit=frame.stack_ptr+24u;
    store(frame.stack_ptr,0x270AFu);store(frame.stack_ptr+4u,iface);
    store(frame.stack_ptr+8u,DSOUND_EFFECTS_IMAGE_ADDRESS);
    store(frame.stack_ptr+12u,DSOUND_EFFECTS_IMAGE_BYTES);store(frame.stack_ptr+16u,loc);
    store(frame.stack_ptr+20u,out);CHECK_EQ_U32(dsound_effects_binding_register(),1u);
    const uint32_t status=dsound_hle_call(0x4079DBu,&frame),desc=load(out);
    CHECK_EQ_U32(status,0u);CHECK_EQ_U32(load(out+4u),0xAAAAAAAAu);
    CHECK(memcmp(image,original,sizeof(original))==0);CHECK_EQ_U32(guest_mem_heap_count(),initial_heaps+1u);
    uint32_t addresses[5]={desc,load(desc+8u),load(desc+16u),load(desc+32u),load(desc+24u)};
    uint32_t sizes[5]={296u,8080u,8088u,393216u,4u};
    CHECK_EQ_U32(load(desc),9u);CHECK_EQ_U32(load(desc+4u),393216u);
    CHECK(memcmp(kernel_guest_at(addresses[1],8080u),image+0x818u,8080u)==0);
    CHECK(memcmp(kernel_guest_at(addresses[2],8088u),image+0x27A8u,8088u)==0);
    for(uint32_t i=0u;i<393216u;i++)CHECK(((uint8_t *)kernel_guest_at(addresses[3],393216u))[i]==0u);
    CHECK_EQ_U32(load(addresses[4]),0u);
    CHECK(fwrite(&status,4u,1u,output)==1u);
    for(unsigned i=0u;i<5u;i++) {
        CHECK(fwrite(&addresses[i],4u,1u,output)==1u);CHECK(fwrite(&sizes[i],4u,1u,output)==1u);
        CHECK(fwrite(kernel_guest_at(addresses[i],sizes[i]),1u,sizes[i],output)==sizes[i]);
    }
    fclose(output);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);CHECK(strstr(fatal_text,"repeat binding")!=NULL);CHECK_EQ_U32(load(out),desc);
    store(desc+4u,1u);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);CHECK(strstr(fatal_text,"immutable views changed")!=NULL);store(desc+4u,393216u);
    uint8_t *code_bytes=kernel_guest_at(addresses[1],8080u);code_bytes[3]^=1u;
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"immutable views changed")!=NULL);code_bytes[3]^=1u;
    /* State/workspace are intentionally mutable: immutability check must not
     * misclassify measured CPU state writes as ownership corruption. */
    store(addresses[2]+0x1Cu,3u);store(addresses[3],99u);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"repeat binding")!=NULL);
    CHECK(!dsound_hle_dsp_ack_configured());CHECK_EQ_U32(dsound_hle_dsp_ack_count(),0u);
    uint32_t owner=0u;
    for(uint32_t generation=1u;generation<16u;generation++)
        for(uint32_t slot=1u;slot<=256u;slot++) {
            uint32_t requested,handle=(generation<<12u)|slot;
            if(guest_heap_block_size(handle,desc,&requested)&&requested==296u)owner=handle;
        }
    CHECK(owner!=0u);CHECK(guest_heap_destroy(owner));
    const uint32_t replacement=guest_heap_create(0u,0u,0u);CHECK(replacement!=owner);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"immutable views changed")!=NULL);
    dsound_effects_binding_reset();CHECK(guest_heap_valid(replacement));
    CHECK(guest_heap_destroy(replacement));CHECK_EQ_U32(guest_mem_heap_count(),initial_heaps);
    /* Quiescent reset preserves enabled policy and permits a fresh session bind. */
    CHECK_EQ_U32(dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out),0u);
    dsound_effects_binding_reset();CHECK_EQ_U32(guest_mem_heap_count(),initial_heaps);
    dsound_device_reset();environment_end();
    printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
