/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "d3d8_first_vblank.h"
#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_vblank_effects.h"
#include "kernel_clock.h"
#include <pthread.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static uint32_t fs,esp,low,high;
static unsigned invocations;
static uint32_t expected_count=2u,expected_flip_index,expected_flags;
static bool fail_invoke,trap_invoke;
static bool invoke(uint32_t address,uint32_t stack_low,uint32_t stack_high,const uint32_t record[3])
{
    d3d8_first_vblank_snapshot s;d3d8_first_vblank_get_snapshot(&s);
    CHECK(s.attempted && !s.delivered);CHECK_EQ_U32(address,0x22020u);
    CHECK_EQ_U32(stack_low,low);CHECK_EQ_U32(stack_high,high);
    CHECK_EQ_U32(record[0],expected_count);CHECK_EQ_U32(record[1],expected_flip_index);CHECK_EQ_U32(record[2],expected_flags);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),expected_count);invocations++;
    if(trap_invoke)d3d8_hle_fatal(0x22020u,"synthetic callback body stop");
    return !fail_invoke;
}
static void seed(void)
{
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    const uint32_t offsets[]={0x1DB8u,0x1DE8u,0x1DE4u,0x1DECu,0x1DDCu,0x1D9Cu,0x1DA8u,0x2448u,0x244Cu};
    const uint32_t values[]={0x22020u,1u,0u,0u,0x02480104u,0u,0u,0u,0u};
    for(unsigned i=0u;i<9u;i++)store(D3D8_DEVICE_BASE+offsets[i],values[i]);
    uint8_t irql=0u;CHECK(kernel_guest_write_bytes(fs+0x24u,&irql,1u));store(esp,0x3D455u);
}
static void begin(void)
{seed();d3d8_first_vblank_configure(true,invoke,low,high);d3d8_first_vblank_bind_owner(1u,fs,0x3801D9u,0x37FE1Du);}
static void refused(uint32_t callee,uint32_t base,uint32_t stack,uint32_t irql)
{
    uint32_t count=load(D3D8_DEVICE_BASE+0x1DE8u);unsigned calls=invocations;
    RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(callee,base,stack,irql));CHECK(fatal_seen);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),count);CHECK_EQ_U32(invocations,calls);
    d3d8_first_vblank_snapshot s;d3d8_first_vblank_get_snapshot(&s);CHECK(!s.attempted);
}
static void *other_thread(void *unused)
{
    (void)unused;RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u));return NULL;
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);fs=0x7C0000u;esp=SCRATCH_DATA+128u;
    low=0x7D0000u;high=low+4096u;map_fixed(fs,4096u);map_fixed(low,4096u);seed();
    d3d8_first_vblank_configure(false,NULL,0u,0u);
    d3d8_first_vblank_bind_owner(1u,fs,0x3801D9u,0x37FE1Du);
    d3d8_first_vblank_poll(0x1538C0u,0u,0u,UINT32_MAX);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),1u);CHECK_EQ_U32(invocations,0u);
    d3d8_first_vblank_configure(true,invoke,low,high);
    d3d8_first_vblank_bind_owner(7u,fs,0x1234u,0x37FE1Du);
    refused(0x1538C0u,fs,esp,0u);
    begin();RUN_EXPECTING_FATAL(d3d8_first_vblank_configure(true,invoke,low,high-1u));CHECK(fatal_seen);
    d3d8_first_vblank_poll(0x22030u,0u,0u,UINT32_MAX);
    refused(0x1538C0u,fs+1u,esp,0u);refused(0x1538C0u,fs,esp,1u);
    store(esp,0u);refused(0x1538C0u,fs,esp,0u);store(esp,0x3D455u);
    uint8_t pcr=1u;CHECK(kernel_guest_write_bytes(fs+0x24u,&pcr,1u));refused(0x1538C0u,fs,esp,0u);pcr=0u;CHECK(kernel_guest_write_bytes(fs+0x24u,&pcr,1u));
    pthread_t thread;int created=pthread_create(&thread,NULL,other_thread,NULL);int joined=created?created:pthread_join(thread,NULL);
    CHECK(created==0 && joined==0);CHECK(fatal_seen);CHECK_EQ_U32(invocations,0u);
    const uint32_t offsets[]={0x1DB8u,0x1DE8u,0x1DE4u,0x1DECu,0x1DDCu,0x1D9Cu,0x1DA8u,0x2448u,0x244Cu};
    for(unsigned i=0u;i<9u;i++){uint32_t at=D3D8_DEVICE_BASE+offsets[i],value=load(at);store(at,value^1u);refused(0x1538C0u,fs,esp,0u);store(at,value);}
    store(0x3E3F58u,D3D8_DEVICE_BASE+4u);refused(0x1538C0u,fs,esp,0u);store(0x3E3F58u,D3D8_DEVICE_BASE);
    CHECK(mprotect((void *)(uintptr_t)low,4096u,PROT_READ)==0);refused(0x1538C0u,fs,esp,0u);
    CHECK(mprotect((void *)(uintptr_t)low,4096u,PROT_NONE)==0);refused(0x1538C0u,fs,esp,0u);
    CHECK(mprotect((void *)(uintptr_t)low,4096u,PROT_READ|PROT_WRITE)==0);
    void *owner_page=(void *)(uintptr_t)(fs&~4095u);
    CHECK(mprotect(owner_page,4096u,PROT_NONE)==0);refused(0x1538C0u,fs,esp,0u);
    CHECK(mprotect(owner_page,4096u,PROT_READ|PROT_WRITE)==0);
    void *caller_page=(void *)(uintptr_t)(esp&~4095u);
    CHECK(mprotect(caller_page,4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u));CHECK(fatal_seen);
    CHECK(mprotect(caller_page,4096u,PROT_READ|PROT_WRITE)==0);
    void *extra_page=(void *)(uintptr_t)((D3D8_DEVICE_BASE+0x2448u)&~4095u);
    CHECK(mprotect(extra_page,4096u,PROT_NONE)==0);refused(0x1538C0u,fs,esp,0u);
    CHECK(mprotect(extra_page,4096u,PROT_READ|PROT_WRITE)==0);
    void *page=(void *)(uintptr_t)((D3D8_DEVICE_BASE+0x1DE8u)&~4095u);
    CHECK(mprotect(page,4096u,PROT_READ)==0);refused(0x1538C0u,fs,esp,0u);
    CHECK(mprotect(page,4096u,PROT_NONE)==0);
    RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u));CHECK(fatal_seen);
    CHECK(mprotect(page,4096u,PROT_READ|PROT_WRITE)==0);
    d3d8_first_vblank_configure(true,invoke,fs+128u,fs+256u);d3d8_first_vblank_bind_owner(1u,fs,0x3801D9u,0x37FE1Du);refused(0x1538C0u,fs,esp,0u);
    begin();
    uint32_t old_esp=esp;esp=D3D8_DEVICE_BASE+0x2300u;store(esp,0x3D455u);
    refused(0x1538C0u,fs,esp,0u);esp=old_esp;
    begin();RUN_EXPECTING_FATAL(d3d8_first_vblank_bind_owner(2u,fs,0x3801D9u,0x37FE1Du));CHECK(fatal_seen);
    uint8_t before[0x2450],after[0x2450];CHECK(kernel_guest_read_bytes(D3D8_DEVICE_BASE,before,sizeof(before)));
    uint64_t clock=kernel_clock_peek();d3d8_gpu_stats stats=d3d8_gpu_get_stats();
    d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u);CHECK_EQ_U32(invocations,1u);
    CHECK(kernel_guest_read_bytes(D3D8_DEVICE_BASE,after,sizeof(after)));uint32_t count=2u;memcpy(before+0x1DE8u,&count,4u);CHECK(memcmp(before,after,sizeof(before))==0);
    CHECK(kernel_clock_peek()==clock);d3d8_gpu_stats now=d3d8_gpu_get_stats();CHECK(memcmp(&stats,&now,sizeof(stats))==0);
    d3d8_first_vblank_snapshot s;d3d8_first_vblank_get_snapshot(&s);CHECK(s.attempted && s.delivered);
    RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u));CHECK(fatal_seen);CHECK_EQ_U32(invocations,1u);
    begin();fail_invoke=true;RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u));CHECK(fatal_seen);
    d3d8_first_vblank_get_snapshot(&s);CHECK(s.attempted && !s.delivered);CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),2u);fail_invoke=false;
    begin();trap_invoke=true;RUN_EXPECTING_FATAL(d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u));CHECK(fatal_seen);CHECK_EQ_U32(fatal_address,0x22020u);
    d3d8_first_vblank_get_snapshot(&s);CHECK(s.attempted && !s.delivered);trap_invoke=false;
    begin();CHECK(mprotect(extra_page,4096u,PROT_READ)==0);
    d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u);
    CHECK(mprotect(extra_page,4096u,PROT_READ|PROT_WRITE)==0);
    /* T372 coupling: the measured start count is the number of waits applied, not the constant 1. */
    d3d8_first_vblank_reset();
    store(D3D8_DEVICE_BASE+0x1DE8u,0u);store(D3D8_DEVICE_BASE+0x1DDCu,0x02480104u);
    d3d8_vblank_effects_configure(true);
    for(unsigned wait=0u;wait<3u;wait++)(void)d3d8_vblank_effects_apply(0x1000u*(wait+1u));
    CHECK_EQ_U32(d3d8_vblank_effects_applied(),3u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),3u);
    begin();store(D3D8_DEVICE_BASE+0x1DE8u,3u);
    store(D3D8_DEVICE_BASE+0x1DE8u,1u);refused(0x1538C0u,fs,esp,0u); /* the uncoupled constant is now stale */
    store(D3D8_DEVICE_BASE+0x1DE8u,3u);
    expected_count=4u;invocations=0u;
    d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u);CHECK_EQ_U32(invocations,1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),4u);
    d3d8_vblank_effects_configure(false);
    /* T407 flip model: a Swap's flip was queued and completed by the next wait. The preflight then
     * proves the drained queue (consumer index = flips processed = producer index, both slots
     * empty) instead of demanding zeros, takes the threshold the queue left, and when the startup
     * event's count meets it updates it as the helper does (flags 2). */
    d3d8_first_vblank_reset();
    store(D3D8_DEVICE_BASE+0x1DE8u,0u);store(D3D8_DEVICE_BASE+0x1DDCu,0x02480104u);
    d3d8_vblank_effects_configure(true);d3d8_flip_configure(true);d3d8_flip_reset();
    for(unsigned wait=0u;wait<3u;wait++)(void)d3d8_vblank_effects_apply(0x1000u*(wait+1u));
    d3d8_flip_lock();
    CHECK(d3d8_flip_queue_refusal_locked(d3d8_flip_swap_method_data(0x1234000u,0u))==NULL);
    (void)d3d8_flip_queue_locked(d3d8_flip_swap_method_data(0x1234000u,0u));
    d3d8_flip_unlock();
    (void)d3d8_vblank_effects_apply(0x4000u);
    const uint32_t flip_count=load(D3D8_DEVICE_BASE+0x1DE8u),flip_threshold=load(D3D8_DEVICE_BASE+0x1DECu);
    CHECK_EQ_U32(flip_count,4u);CHECK_EQ_U32(flip_threshold,5u);CHECK_EQ_U32(d3d8_flip_hardware_get().flips,1u);
    begin();
    store(D3D8_DEVICE_BASE+0x1DE8u,flip_count);store(D3D8_DEVICE_BASE+0x1DE4u,1u);
    store(D3D8_DEVICE_BASE+0x1DF4u,1u);store(D3D8_DEVICE_BASE+0x1DECu,flip_threshold);
    store(D3D8_DEVICE_BASE+0x1D9Cu,0u);store(D3D8_DEVICE_BASE+0x1DA8u,0u);
    store(D3D8_DEVICE_BASE+0x1DF4u,2u);refused(0x1538C0u,fs,esp,0u); /* a queued flip not yet processed */
    store(D3D8_DEVICE_BASE+0x1DF4u,1u);
    store(D3D8_DEVICE_BASE+0x1DE4u,0u);refused(0x1538C0u,fs,esp,0u); /* consumer index is not the flips done */
    store(D3D8_DEVICE_BASE+0x1DE4u,1u);
    store(D3D8_DEVICE_BASE+0x1D9Cu,1u);refused(0x1538C0u,fs,esp,0u); /* a slot still pending */
    store(D3D8_DEVICE_BASE+0x1D9Cu,0u);
    expected_count=5u;expected_flip_index=1u;expected_flags=2u;invocations=0u;
    d3d8_first_vblank_poll(0x1538C0u,fs,esp,0u);CHECK_EQ_U32(invocations,1u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DE8u),5u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DECu),6u);
    /* With the flip model off the same bookkeeping is still refused: zeros are demanded. */
    d3d8_vblank_effects_configure(false);
    begin();store(D3D8_DEVICE_BASE+0x1DE4u,1u);store(D3D8_DEVICE_BASE+0x1DF4u,1u);
    store(D3D8_DEVICE_BASE+0x1DECu,flip_threshold);
    CHECK(d3d8_flip_enabled()&&!d3d8_vblank_effects_enabled());refused(0x1538C0u,fs,esp,0u); /* both opt-ins are needed */
    d3d8_flip_configure(false);
    begin();store(D3D8_DEVICE_BASE+0x1DE4u,1u);refused(0x1538C0u,fs,esp,0u);
    d3d8_first_vblank_reset();environment_end();printf("first vblank: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
