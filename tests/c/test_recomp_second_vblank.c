/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "recomp_second_vblank.h"
#include "host_runtime.h"
#include "kernel_clock.h"
#include <pthread.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static uint32_t fs=0x7C0000u, low=0x7D0000u, high=0x7D1000u, esp;
static unsigned calls;
static bool producer_done=true;
static int mode;
static void *guard;
static char reason[256];
static void credit(bool success);
static void host_fatal(uint32_t address,const char *text)
{ snprintf(reason,sizeof(reason),"%s",text); host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,address,0u,reason); }
static bool confirmed(uint32_t handle,uint32_t base)
{
    CHECK_EQ_U32(handle,2u); CHECK_EQ_U32(base,0x7E0000u);
    CHECK(!recomp_second_vblank_reset());
    CHECK(!recomp_second_vblank_configure(false,NULL,0u,0u,NULL));
    if(mode==5)credit(true);
    return producer_done;
}
static bool callback(uint32_t address,uint32_t lo,uint32_t hi,const uint32_t payload[3])
{
    CHECK_EQ_U32(address,0x22020u); CHECK_EQ_U32(lo,low); CHECK_EQ_U32(hi,high);
    CHECK_EQ_U32(payload[1],0u); CHECK_EQ_U32(payload[2],0u);
    calls++; CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),payload[0]);
    CHECK(!recomp_second_vblank_reset());
    if (payload[0]==3u && mode==1) host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,0x22020u,77u,"callback detail");
    if (payload[0]==3u && mode==2) *(volatile uint8_t *)guard=1u;
    if (payload[0]==3u && mode==6) return false;
    if (mode==4) credit(true);
    if (payload[0]==3u && mode==3) recomp_second_vblank_note_wait_completed(true,2u,0x7E0000u);
    store(0x563918u,load(0x563918u)+1u); return true;
}
static void seed(void)
{
    store(0x3E3F58u,0x3E3F60u); store(0x563918u,0u); store(esp,0x3D455u);
    const uint32_t offsets[]={0x1DB8u,0x1DE8u,0x1DE4u,0x1DECu,0x1DDCu,0x1D9Cu,0x1DA8u,0x2448u,0x244Cu};
    const uint32_t values[]={0u,1u,0u,0u,0x02480104u,0u,0u,0u,0u};
    for(unsigned i=0u;i<9u;i++)store(0x3E3F60u+offsets[i],values[i]);
    CHECK(kernel_guest_write_u8(fs+0x24u,0u));
    CHECK(recomp_second_vblank_configure(true,callback,low,high,confirmed));
    recomp_second_vblank_bind_owner(1u,fs,0x3801D9u,0x37FE1Du);
    recomp_second_vblank_note_registration(0x22020u,1u,fs);
    store(0x3E3F60u+0x1DB8u,0x22020u);
    producer_done=true;mode=0;
}
static void run_poll(bool expected_stop)
{
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm(); recomp_second_vblank_poll(0x1538C0u,1u,fs,esp,0u); CHECK(!expected_stop);
    } else CHECK(expected_stop);
    CHECK(host_run_scope_depth()==0u); host_run_disarm();
    recomp_second_vblank_snapshot s;recomp_second_vblank_get_snapshot(&s);CHECK(!s.inflight);
}
static void *worker(void *arg)
{
    bool success=arg!=NULL;
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();recomp_second_vblank_note_wait_completed(success,2u,0x7E0000u);
    }
    host_run_disarm();return NULL;
}
static void credit(bool success)
{pthread_t t;CHECK(pthread_create(&t,NULL,worker,success?(void *)&calls:NULL)==0);CHECK(pthread_join(t,NULL)==0);}
static void first(void)
{seed();credit(true);run_poll(false);CHECK_EQ_U32(load(0x563918u),1u);store(esp,0x3D66Du);}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);d3d8_hle_set_fatal(host_fatal);
    esp=SCRATCH_DATA+128u;map_fixed(fs,4096u);map_fixed(low,4096u);map_fixed(0x563000u,4096u);
    guard=mmap(NULL,4096u,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);
    CHECK(recomp_second_vblank_configure(false,NULL,0u,0u,NULL));
    recomp_second_vblank_note_registration(0u,0u,0u);recomp_second_vblank_note_wait_completed(true,0u,0u);
    recomp_second_vblank_poll(0x1538C0u,0u,0u,0u,99u);CHECK_EQ_U32(calls,0u);
    for(unsigned variant=0u;variant<5u;variant++) {
        seed();
        CHECK(recomp_second_vblank_configure(true,callback,low,high,confirmed));
        recomp_second_vblank_bind_owner(1u,fs,0x3801D9u,0x37FE1Du);
        store(0x3E3F60u+0x1DB8u,variant==0u?0x1234u:0u);
        void *slot_page=(void *)(uintptr_t)((0x3E3F60u+0x1DB8u)&~4095u);
        void *global_page=(void *)(uintptr_t)(0x3E3F58u&~4095u);
        if(variant==1u)CHECK(mprotect(slot_page,4096u,PROT_READ)==0);
        if(variant==2u)CHECK(mprotect(slot_page,4096u,PROT_NONE)==0);
        if(variant==3u)CHECK(mprotect(global_page,4096u,PROT_NONE)==0);
        if(variant==4u)store(0x3E3F58u,0x3E3F64u);
        unsigned before_calls=calls;
        if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();recomp_second_vblank_note_registration(0x22020u,1u,fs);CHECK(false);}
        else CHECK(host_run_result()->reason==HOST_STOP_XDK_UNIMPLEMENTED);
        host_run_disarm();
        if(variant==1u || variant==2u)CHECK(mprotect(slot_page,4096u,PROT_READ|PROT_WRITE)==0);
        if(variant==3u)CHECK(mprotect(global_page,4096u,PROT_READ|PROT_WRITE)==0);
        CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),1u);CHECK_EQ_U32(load(0x563918u),0u);CHECK_EQ_U32(calls,before_calls);
        CHECK_EQ_U32(load(0x3E3F60u+0x1DB8u),variant==0u?0x1234u:0u);
        recomp_second_vblank_snapshot rejected;recomp_second_vblank_get_snapshot(&rejected);
        CHECK(rejected.refused && !rejected.registered);
    }
    first();credit(false);run_poll(true);CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);
    first();credit(true);uint64_t clock=kernel_clock_peek();run_poll(false);
    CHECK_EQ_U32(load(0x563918u),2u);CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),3u);CHECK(kernel_clock_peek()==clock);run_poll(true);
    first();credit(true);producer_done=false;run_poll(true);CHECK_EQ_U32(load(0x563918u),1u);run_poll(true);
    first();credit(true);store(0x3E3F60u+0x2448u,1u);run_poll(true);CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);
    first();credit(true);store(esp,0x3D455u);run_poll(true);
    first();credit(true);CHECK(kernel_guest_write_u8(fs+0x24u,1u));run_poll(true);
    CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);
    first();credit(true);CHECK(mprotect((void *)(uintptr_t)0x563000u,4096u,PROT_READ)==0);run_poll(true);
    CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);CHECK(mprotect((void *)(uintptr_t)0x563000u,4096u,PROT_READ|PROT_WRITE)==0);
    first();credit(true);CHECK(mprotect((void *)(uintptr_t)fs,4096u,PROT_READ)==0);run_poll(true);
    CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);CHECK(mprotect((void *)(uintptr_t)fs,4096u,PROT_READ|PROT_WRITE)==0);
    first();credit(true);CHECK(mprotect((void *)(uintptr_t)low,4096u,PROT_READ)==0);run_poll(true);
    CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);CHECK(mprotect((void *)(uintptr_t)low,4096u,PROT_READ|PROT_WRITE)==0);
    for(mode=1;mode<=6;) {
        int selected=mode;first();credit(true);mode=selected;run_poll(true);
        const host_stop *stop=host_run_result();
        if(selected==1){CHECK_EQ_U32(stop->ordinal,77u);CHECK_EQ_U32(stop->guest_address,0x22020u);CHECK(strcmp(stop->detail,"callback detail")==0);}
        if(selected==2){CHECK(stop->reason==HOST_STOP_FAULT);CHECK(stop->fault_address==(uintptr_t)guard);}
        recomp_second_vblank_snapshot s;recomp_second_vblank_get_snapshot(&s);CHECK(s.second_attempted && !s.second_delivered);run_poll(true);mode=selected+1;
    }
    first();credit(true);credit(true); /* second producer completion must refuse, not accumulate */
    recomp_second_vblank_snapshot s;recomp_second_vblank_get_snapshot(&s);CHECK(s.credits==1u && s.refused);run_poll(true);CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);
    CHECK(recomp_second_vblank_configure(false,NULL,0u,0u,NULL));CHECK(recomp_second_vblank_reset());
    first();
    if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();recomp_second_vblank_note_registration(0x22020u,1u,fs);CHECK(false);}
    else CHECK(host_run_result()->reason==HOST_STOP_XDK_UNIMPLEMENTED);
    host_run_disarm();recomp_second_vblank_get_snapshot(&s);CHECK(s.refused);run_poll(true);
    seed();mode=4;run_poll(true);CHECK_EQ_U32(load(0x3E3F60u+0x1DE8u),2u);
    CHECK(recomp_second_vblank_configure(false,NULL,0u,0u,NULL));
    CHECK(munmap(guard,4096u)==0);environment_end();printf("second vblank: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
