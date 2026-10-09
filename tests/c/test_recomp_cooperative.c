/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "recomp_cooperative.h"
#include "host_runtime.h"
#include "recomp_abi.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
TSFP_RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_fs_base;
static bool registers_unchanged(void)
{return g_eax==1u && g_ecx==2u && g_edx==3u && g_esp==4u && g_ebx==5u &&
 g_esi==6u && g_edi==7u && g_ebp==8u && g_fs_base==9u;}
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d\n",__LINE__);}}while(0)
#ifndef RECOMP_COOPERATIVE_NO_MARKER
static int capability=1;
int recomp_has_cooperative_calls(void);
int recomp_has_cooperative_calls(void){return capability;}
#endif
static void *guard;
static _Thread_local unsigned calls;
static _Thread_local int mode;
static _Thread_local uint32_t observed;
static _Thread_local bool provider_ok;
static int userdata;
static void provider(uint32_t callee,void *data)
{
    calls++;observed=callee;
    provider_ok=data==&userdata && host_run_armed() && host_run_scope_depth()==1u;
    if(mode==1) {
        provider_ok=provider_ok && !recomp_cooperative_configure(NULL,NULL);
        recomp_call_safepoint(0x777u);
        provider_ok=provider_ok && calls==1u;
    }
    if(mode==2)host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,callee,77u,"provider original detail");
    if(mode==3)*(volatile unsigned char *)guard=9u;
}
static void run(int selected)
{
    mode=selected;calls=0u;provider_ok=false;
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();recomp_call_safepoint(0x1234u);CHECK(selected<2);
    } else {
        CHECK(selected>=2);const host_stop *stop=host_run_result();
        if(selected==2) {
            CHECK(stop->reason==HOST_STOP_XDK_UNIMPLEMENTED && stop->guest_address==0x1234u);
            CHECK(stop->ordinal==77u && stop->fault_address==0u && stop->signal_number==0);
            CHECK(stop->detail!=NULL && stop->detail[0]=='p');
        } else CHECK(stop->reason==HOST_STOP_FAULT && stop->signal_number==SIGSEGV && stop->fault_address==(uintptr_t)guard);
    }
    CHECK(calls==1u && observed==0x1234u && provider_ok);CHECK(registers_unchanged());
    CHECK(host_run_scope_depth()==0u);host_run_disarm();CHECK(!host_run_armed());
}
static host_run_scope saturated[HOST_RUN_SCOPE_MAX];
static unsigned cursor;
static void full_scope_chain(void)
{
    mode=0;calls=0u;
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();
        for(cursor=0u;cursor<HOST_RUN_SCOPE_MAX;cursor++) {
            CHECK(host_run_scope_init(&saturated[cursor]));
            if(sigsetjmp(*host_run_scope_jmp(&saturated[cursor]),1)!=0) {
                CHECK(host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
                CHECK(host_run_result()->guest_address==0x1234u);CHECK(calls==0u);
                while(host_run_scope_depth()!=0u)CHECK(host_run_scope_pop(&saturated[host_run_scope_depth()-1u]));
                host_run_disarm();return;
            }
            CHECK(host_run_scope_push(&saturated[cursor]));
        }
        recomp_call_safepoint(0x1234u);CHECK(false);
    } else {CHECK(false);host_run_disarm();}
}
static pthread_barrier_t barrier;
static _Thread_local unsigned parallel_calls;
static _Thread_local bool parallel_ok;
static void parallel_provider(uint32_t callee,void *data)
{
    parallel_calls++;parallel_ok=data==&userdata && callee==0xA5u && host_run_scope_depth()==1u;
    (void)pthread_barrier_wait(&barrier);
    recomp_call_safepoint(0x999u);parallel_ok=parallel_ok && parallel_calls==1u;
}
static void *worker(void *data)
{
    bool *okay=data;
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();recomp_call_safepoint(0xA5u);
        *okay=parallel_ok && parallel_calls==1u && host_run_scope_depth()==0u;
        host_run_disarm();
    } else {*okay=false;host_run_disarm();}
    return NULL;
}
static void threads(void)
{
    CHECK(recomp_cooperative_configure(parallel_provider,&userdata));
    CHECK(pthread_barrier_init(&barrier,NULL,2u)==0);pthread_t ids[2];bool okay[2]={false,false};
    for(unsigned i=0u;i<2u;i++)CHECK(pthread_create(&ids[i],NULL,worker,&okay[i])==0);
    for(unsigned i=0u;i<2u;i++){CHECK(pthread_join(ids[i],NULL)==0);CHECK(okay[i]);}
    CHECK(pthread_barrier_destroy(&barrier)==0);
}
int main(void)
{
    /* Default disabled calls are valid without root arming/configuration. */
    g_eax=1u;g_ecx=2u;g_edx=3u;g_esp=4u;g_ebx=5u;g_esi=6u;g_edi=7u;g_ebp=8u;g_fs_base=9u;
    recomp_call_safepoint(0u);CHECK(calls==0u && host_run_scope_depth()==0u);
    CHECK(registers_unchanged());
#ifdef RECOMP_COOPERATIVE_NO_MARKER
    CHECK(!recomp_cooperative_ready());
#else
    const int values[]={0,2,-1,1};
    for(unsigned i=0u;i<4u;i++){capability=values[i];CHECK(recomp_cooperative_ready()==(values[i]==1));}
#endif
    CHECK(calls==0u && !host_run_armed());CHECK(registers_unchanged());
    CHECK(recomp_cooperative_configure(provider,&userdata));
    guard=mmap(NULL,4096u,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);
    run(0);run(1);run(2);run(0);run(3);run(0);full_scope_chain();run(0);
    pid_t child=fork();CHECK(child>=0);
    if(child==0){recomp_call_safepoint(0xABu);_exit(3);}
    if(child>0){int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);}
    threads();CHECK(recomp_cooperative_configure(NULL,NULL));
    recomp_call_safepoint(0x999u);CHECK(calls==1u);CHECK(host_run_scope_depth()==0u && !host_run_armed());
    CHECK(munmap(guard,4096u)==0);
    printf("cooperative provider: %u checks, %u failures\n",checks,failures);return failures!=0u;
}
