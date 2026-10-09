/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include <sys/mman.h>
#include <sys/types.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static bool capture_mapping, reuse_mapping;
static void *saved_mapping;
void *__real_mmap(void *,size_t,int,int,int,off_t);
void *__wrap_mmap(void *address,size_t bytes,int protection,int flags,int fd,off_t offset)
{
    if(reuse_mapping){address=saved_mapping;flags=(flags&~MAP_32BIT)|MAP_FIXED_NOREPLACE;reuse_mapping=false;}
    void *result=__real_mmap(address,bytes,protection,flags,fd,offset);
    if(capture_mapping && result!=MAP_FAILED){saved_mapping=result;capture_mapping=false;}
    return result;
}
#define SLOT 0x412B30u
#define OUT SCRATCH_DATA
static dsound_device_identity observed;
static unsigned calls;
static void observe(const dsound_device_identity *identity,void *userdata,uint32_t *result)
{
    CHECK(userdata==&observed);observed=*identity;calls++;*result=0x12345678u;
    CHECK(guest_heap_valid(identity->device_heap));
}
static void existing(uint32_t internal,void *userdata,uint32_t *result)
{CHECK(internal==observed.internal_address);CHECK(userdata==&observed);*result=7u;}
static void refusal(uint32_t interface)
{
    uint32_t result=0xAABBCCDDu;unsigned before=calls;
    CHECK(!dsound_device_with_owned_identity(interface,observe,&observed,&result));
    CHECK_EQ_U32(result,0xAABBCCDDu);CHECK_EQ_U32(calls,before);
}
static uint32_t create(void)
{CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0u);return load(OUT);}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_reset();
    map_fixed(0x412000u,0x1000u);map_fixed(0x4A1000u,0x1000u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    capture_mapping=true;
    uint32_t interface=create(),internal=interface-8u,result=0u;
    uint8_t before[44],after[44];CHECK(kernel_guest_read_bytes(internal,before,sizeof(before)));
    CHECK(dsound_device_with_owned_identity(interface,observe,&observed,&result));
    CHECK_EQ_U32(result,0x12345678u);CHECK_EQ_U32(observed.internal_address,internal);
    const dsound_device_identity first=observed;
    CHECK(kernel_guest_read_bytes(internal,after,sizeof(after)));CHECK(memcmp(before,after,44u)==0);
    CHECK(dsound_device_with_owned_interface(interface,existing,&observed,&result));CHECK_EQ_U32(result,7u);
    refusal(0u);refusal(interface+4u);
    CHECK(!dsound_device_with_owned_identity(interface,NULL,&observed,&result));
    CHECK(!dsound_device_with_owned_identity(interface,observe,&observed,NULL));
    for(unsigned i=0u;i<11u;i++) {
        uint32_t value=load(internal+4u*i);store(internal+4u*i,value^1u);refusal(interface);
        CHECK_EQ_U32(load(internal+4u*i),value^1u);store(internal+4u*i,value);
        CHECK(dsound_device_with_owned_identity(interface,observe,&observed,&result));
    }
    store(SLOT,internal+4u);refusal(interface);CHECK_EQ_U32(load(SLOT),internal+4u);store(SLOT,internal);
    void *page=(void *)(uintptr_t)(internal&~4095u);
    CHECK(mprotect(page,4096u,PROT_READ)==0);
    CHECK(dsound_device_with_owned_identity(interface,observe,&observed,&result));
    CHECK(mprotect(page,4096u,PROT_NONE)==0);refusal(interface);
    CHECK(mprotect(page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(dsound_device_with_owned_identity(interface,observe,&observed,&result));
    CHECK(kernel_guest_read_bytes(internal,after,sizeof(after)));CHECK(memcmp(before,after,44u)==0);
    CHECK(dsound_device_reset_checked());refusal(interface);
    reuse_mapping=true;interface=create();CHECK_EQ_U32(interface-8u,first.internal_address);
    CHECK(dsound_device_with_owned_identity(interface,observe,&observed,&result));
    CHECK(observed.device_heap!=first.device_heap);CHECK(!guest_heap_valid(first.device_heap));
    CHECK(dsound_device_reset_checked());environment_end();
    printf("dsound device identity: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
