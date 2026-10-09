/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_listener.h"
#include "kernel_clock.h"
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
static uint8_t irql;
static bool known=true;
static bool current_irql(uint8_t *out){*out=irql;return known;}
static uint32_t interface;
static dsound_listener_snapshot snapshot(void)
{dsound_listener_snapshot s;CHECK(dsound_listener_get_snapshot(interface,&s));return s;}
#define REFUSED(call) do {dsound_listener_snapshot cache_before=snapshot();RUN_EXPECTING_FATAL((void)(call));CHECK(fatal_seen);dsound_listener_snapshot cache_after=snapshot();CHECK(memcmp(&cache_before,&cache_after,sizeof(cache_before))==0);} while(0)
static void zero_order(void)
{
    dsound_listener_reset();dsound_listener_snapshot s=snapshot();CHECK_EQ_U32(s.cache_mask,0u);
    REFUSED(dsound_listener_cache_commit(interface));
    REFUSED(dsound_listener_cache_work());
    REFUSED(dsound_listener_cache_position(interface,0u,0u,0u,0u));
    REFUSED(dsound_listener_cache_doppler(interface,0x80000000u,0u));
    REFUSED(dsound_listener_cache_doppler(interface,0u,1u));
}
static void complete(void)
{
    CHECK_EQ_U32(dsound_listener_cache_doppler(interface,0u,0u),0u);
    REFUSED(dsound_listener_cache_doppler(interface,0u,0u));
    REFUSED(dsound_listener_cache_orientation(interface,0u,0u,0x3F800000u,0u,0x3F800000u,0u,0u));
    for(unsigned i=0u;i<3u;i++) {
        uint32_t values[3]={0u};values[i]=1u;
        REFUSED(dsound_listener_cache_position(interface,values[0],values[1],values[2],0u));
    }
    REFUSED(dsound_listener_cache_position(interface,0u,0u,0u,1u));
    CHECK_EQ_U32(dsound_listener_cache_position(interface,0u,0u,0u,0u),0u);
    for(unsigned i=0u;i<6u;i++) {
        uint32_t values[6]={0u,0u,0x3F800000u,0u,0x3F800000u,0u};values[i]^=1u;
        REFUSED(dsound_listener_cache_orientation(interface,values[0],values[1],values[2],values[3],values[4],values[5],0u));
    }
    REFUSED(dsound_listener_cache_orientation(interface,0u,0u,0x3F800000u,0u,0x3F800000u,0u,1u));
    CHECK_EQ_U32(dsound_listener_cache_orientation(interface,0u,0u,0x3F800000u,0u,0x3F800000u,0u,0u),0u);
    uint64_t clock_before=kernel_clock_peek();size_t heaps_before=guest_mem_heap_count();
    uint32_t ref_before=load(interface-4u);
    dsound_listener_snapshot pre=snapshot();
    CHECK(!pre.commit_seen);
    REFUSED(dsound_listener_cache_work());
    CHECK_EQ_U32(dsound_listener_cache_commit(interface),0u);
    CHECK_EQ_U32(dsound_listener_cache_commit(interface),0u);
    CHECK(kernel_clock_peek()==clock_before);CHECK(guest_mem_heap_count()==heaps_before);
    CHECK_EQ_U32(load(interface-4u),ref_before);
    CHECK_EQ_U32(dsound_listener_cache_work(),0u);
    CHECK_EQ_U32(dsound_listener_cache_work(),0u);
    CHECK(kernel_clock_peek()==clock_before);CHECK(guest_mem_heap_count()==heaps_before);
    CHECK_EQ_U32(load(interface-4u),ref_before);
    pre.work_seen=true;
    pre.commit_seen=true;dsound_listener_snapshot committed=snapshot();
    CHECK(memcmp(&pre,&committed,sizeof(pre))==0);
    dsound_listener_snapshot s=snapshot();CHECK_EQ_U32(s.cache_mask,7u);
    CHECK_EQ_U32(s.orientation[2],0x3F800000u);CHECK_EQ_U32(s.orientation[4],0x3F800000u);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_reset();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    capture_mapping=true;CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0u);interface=load(OUT);
    uint32_t internal=interface-8u;uint8_t header[44],after[44];CHECK(kernel_guest_read_bytes(internal,header,44u));
    dsound_listener_set_fatal(catching_fatal);dsound_listener_set_irql_provider(current_irql);
    zero_order();REFUSED(dsound_listener_cache_doppler(interface,0u,0u));
    dsound_listener_set_enabled(true);known=false;REFUSED(dsound_listener_cache_doppler(interface,0u,0u));
    known=true;irql=1u;REFUSED(dsound_listener_cache_doppler(interface,0u,0u));irql=0u;
    store(0x4124A8u,1u);REFUSED(dsound_listener_cache_doppler(interface,0u,0u));store(0x4124A8u,0u);
    zero_order();REFUSED(dsound_listener_cache_doppler(interface+4u,0u,0u));complete();
    for(unsigned i=0u;i<11u;i++) {
        uint32_t at=internal+4u*i,value=load(at);store(at,value^1u);
        dsound_listener_snapshot out,before;memset(&out,0xA5,sizeof(out));before=out;
        CHECK(!dsound_listener_get_snapshot(interface,&out));CHECK(memcmp(&out,&before,sizeof(out))==0);
        RUN_EXPECTING_FATAL((void)dsound_listener_cache_work());CHECK(fatal_seen);
        CHECK_EQ_U32(load(at),value^1u);store(at,value);CHECK_EQ_U32(snapshot().cache_mask,7u);
    }
    void *page=(void *)(uintptr_t)(internal&~4095u);
    CHECK(mprotect(page,4096u,PROT_READ)==0);zero_order();complete();
    CHECK(mprotect(page,4096u,PROT_NONE)==0);
    dsound_listener_snapshot out,before;memset(&out,0xA5,sizeof(out));before=out;
    CHECK(!dsound_listener_get_snapshot(interface,&out));CHECK(memcmp(&out,&before,sizeof(out))==0);
    RUN_EXPECTING_FATAL((void)dsound_listener_cache_work());CHECK(fatal_seen);
    CHECK(mprotect(page,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(kernel_guest_read_bytes(internal,after,44u));CHECK(memcmp(header,after,44u)==0);
    CHECK(!dsound_listener_get_snapshot(interface+4u,&out));CHECK(memcmp(&out,&before,sizeof(out))==0);
    CHECK(!dsound_listener_get_snapshot(interface,NULL));
    CHECK_EQ_U32(dsound_listener_register(),5u);
    uint32_t singleton=load(SLOT);store(SLOT,0u);
    RUN_EXPECTING_FATAL((void)dsound_listener_cache_work());CHECK(fatal_seen);store(SLOT,singleton);
    const uint32_t entries[]={0x4093ECu,0x40945Au,0x409410u},callers[]={0x27B78u,0x27B88u,0x27BA2u};
    const unsigned counts[]={3u,5u,8u};
    dsound_listener_reset();
    for(unsigned i=0u;i<3u;i++) {
        uint32_t args[8]={interface,0u,0u,0u,0u,0u,0u,0u};if(i==2u){args[3]=0x3F800000u;args[5]=0x3F800000u;}
        kernel_call_frame f={.stack_ptr=SCRATCH_DATA+256u,.stack_limit=SCRATCH_DATA+260u+counts[i]*4u};
        for(unsigned j=0u;j<counts[i];j++)store(f.stack_ptr+4u+j*4u,args[j]);
        store(f.stack_ptr,0xBADu);RUN_EXPECTING_FATAL((void)dsound_hle_call(entries[i],&f));CHECK(fatal_seen);
        CHECK_EQ_U32(snapshot().cache_mask,(1u<<i)-1u);
        store(f.stack_ptr,callers[i]);uint32_t limit=f.stack_limit;f.stack_limit=f.stack_ptr+4u;
        RUN_EXPECTING_FATAL((void)dsound_hle_call(entries[i],&f));CHECK(fatal_seen);
        CHECK_EQ_U32(snapshot().cache_mask,(1u<<i)-1u);f.stack_limit=limit;
        CHECK_EQ_U32(dsound_hle_call(entries[i],&f),0u);
    }
    dsound_listener_set_enabled(false);REFUSED(dsound_listener_cache_work());
    dsound_listener_set_enabled(true);known=false;REFUSED(dsound_listener_cache_work());
    known=true;irql=1u;REFUSED(dsound_listener_cache_work());irql=0u;
    store(0x4124A8u,1u);REFUSED(dsound_listener_cache_work());store(0x4124A8u,0u);
    CHECK_EQ_U32(dsound_listener_cache_commit(interface),0u);
    kernel_call_frame cf={.stack_ptr=SCRATCH_DATA+512u,.stack_limit=SCRATCH_DATA+520u};
    store(cf.stack_ptr,0x1CE492u);store(cf.stack_ptr+4u,interface);
    CHECK_EQ_U32(dsound_hle_call(0x407B40u,&cf),0u);
    cf.stack_limit=0u;
    CHECK_EQ_U32(dsound_hle_call(0x407B40u,&cf),0u);
    /* T605: the title's main loop calls DoWork from 0x1CE45F as well, its neighbours and the other call sites stay refused. */
    cf.stack_limit=SCRATCH_DATA+520u;
    store(cf.stack_ptr,0x1CE45Fu);
    CHECK_EQ_U32(dsound_hle_call(0x407B40u,&cf),0u);
    CHECK(snapshot().work_seen);
    /* T844: the in game frame (game state 24, call 0x458B0 at 0x1CE56E) is the third admitted site, its neighbours stay refused (T1156 admits the
       other three returns above). */
    store(cf.stack_ptr,0x1CE573u);
    CHECK_EQ_U32(dsound_hle_call(0x407B40u,&cf),0u);
    /* T1156: the other three returns of the game states (0x1CE4CA, 0x1CE50A, 0x1CE5C8, the play profile stopped at 0x1CE5C8) are admitted
       through the same gates, their neighbours stay refused. */
    const uint32_t admitted_callers[]={0x1CE4CAu,0x1CE50Au,0x1CE5C8u};
    for(unsigned i=0u;i<sizeof(admitted_callers)/sizeof(admitted_callers[0]);i++) {
        store(cf.stack_ptr,admitted_callers[i]);CHECK_EQ_U32(dsound_hle_call(0x407B40u,&cf),0u);
        cf.stack_limit=cf.stack_ptr;REFUSED(dsound_hle_call(0x407B40u,&cf));cf.stack_limit=SCRATCH_DATA+520u;
    }
    const uint32_t refused_callers[]={0x1CE45Eu,0x1CE460u,0x1CE45Au,0x1CE48Du,0x1CE497u,0x1CE493u,0u,
        0x1CE572u,0x1CE574u,0x1CE56Eu,0x1CE578u,0x1CE4C9u,0x1CE4CBu,0x1CE4C5u,0x1CE509u,0x1CE50Bu,0x1CE505u,
        0x1CE5C7u,0x1CE5C9u,0x1CE5C3u,0x1CE5CDu,0x1CE62Fu};
    for(unsigned i=0u;i<sizeof(refused_callers)/sizeof(refused_callers[0]);i++) {
        store(cf.stack_ptr,refused_callers[i]);REFUSED(dsound_hle_call(0x407B40u,&cf));
    }
    store(cf.stack_ptr,0x1CE45Fu);cf.stack_limit=cf.stack_ptr;REFUSED(dsound_hle_call(0x407B40u,&cf));
    store(cf.stack_ptr,0x1CE573u);REFUSED(dsound_hle_call(0x407B40u,&cf));
    store(cf.stack_ptr,0x1CE573u);cf.stack_limit=SCRATCH_DATA+520u;CHECK_EQ_U32(dsound_hle_call(0x407B40u,&cf),0u);cf.stack_limit=cf.stack_ptr;
    store(cf.stack_ptr,0x1CE492u);cf.stack_limit=0u;
    uint32_t frame_page=0x37000000u;map_fixed(frame_page,8192u);
    CHECK(mprotect((void *)(uintptr_t)(frame_page+4096u),4096u,PROT_NONE)==0);
    kernel_call_frame page_end={.stack_ptr=frame_page+4092u,.stack_limit=0u};
    store(page_end.stack_ptr,0x1CE492u);
    CHECK_EQ_U32(dsound_hle_call(0x407B40u,&page_end),0u);
    page_end.stack_limit=frame_page+4096u;
    CHECK_EQ_U32(dsound_hle_call(0x407B40u,&page_end),0u);
    store(cf.stack_ptr,0xBADu);REFUSED(dsound_hle_call(0x407B40u,&cf));
    store(cf.stack_ptr,0x1CE492u);cf.stack_limit=cf.stack_ptr;
    REFUSED(dsound_hle_call(0x407B40u,&cf));REFUSED(dsound_hle_call(0x407B40u,NULL));
    dsound_listener_snapshot old=snapshot();CHECK(dsound_device_reset_checked());reuse_mapping=true;
    CHECK_EQ_U32(dsound_device_create(0u,OUT,0u),0u);interface=load(OUT);CHECK_EQ_U32(interface-8u,old.identity.internal_address);
    CHECK(!dsound_listener_get_snapshot(interface,&out));CHECK(memcmp(&out,&before,sizeof(out))==0);
    RUN_EXPECTING_FATAL((void)dsound_listener_cache_work());CHECK(fatal_seen);
    dsound_listener_reset();complete();CHECK(snapshot().identity.device_heap!=old.identity.device_heap);
    dsound_listener_reset();CHECK(dsound_device_reset_checked());environment_end();
    printf("dsound listener: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
