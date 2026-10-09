/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_thread.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

TSFP_RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_fs_base,g_seh_ebp;
recomp_func_t recomp_lookup_kernel(uint32_t address);
static _Atomic unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { fprintf(stderr,"wait thunk %d: %s\n",__LINE__,#v);abort(); } } while(0)
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool ready,release_worker;
static bool has_code(uint32_t address) {return address==0x123456u;}
static void terminate(uint32_t status)
{
    (void)status;host_run_stop(HOST_STOP_THREAD_EXITED,0u,0u,"thunk target exit");
}
static bool confirmed(const kernel_thread_launch *launch)
{
    (void)launch;return host_run_result()->reason==HOST_STOP_THREAD_EXITED;
}
static void enter(const kernel_thread_launch *launch)
{
    pthread_mutex_lock(&gate);ready=true;pthread_cond_broadcast(&condition);
    while(!release_worker) {(void)pthread_cond_wait(&condition,&gate);}
    pthread_mutex_unlock(&gate);
    const uint32_t stack=launch->stack_low+4096u-20u;
    const uint32_t frame[5]={0x38004Cu,0xFFFFFFFEu,1u,0u,launch->stack_low+64u};
    CHECK(mprotect((void *)(uintptr_t)(launch->stack_low+4096u),4096u,PROT_NONE)==0);
    const int64_t intervals[2]={-80000,-160000};
    for(unsigned interval=0u;interval<2u;interval++) {
        const int64_t timeout=intervals[interval];
        CHECK(kernel_guest_write_bytes(frame[4],&timeout,sizeof(timeout)));
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_fs_base=8u;g_seh_ebp=9u;g_esp=stack;
        const uint64_t clock=kernel_clock_peek();struct timespec before,after;
        thunk_trace_reset();
        CHECK(clock_gettime(CLOCK_MONOTONIC,&before)==0);
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();recomp_lookup_kernel(KERNEL_THUNK_VA(234u))();
        } else { CHECK(false); }
        host_run_disarm();CHECK(clock_gettime(CLOCK_MONOTONIC,&after)==0);
        const int64_t elapsed=(after.tv_sec-before.tv_sec)*INT64_C(1000000000)+after.tv_nsec-before.tv_nsec;
        CHECK(elapsed>=(interval==0u?8000000:16000000));
        printf("thunk interval %u: elapsed %lld ns\n",interval,(long long)elapsed);
        CHECK(g_eax==0x102u&&g_esp==stack+20u);
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
        CHECK(g_fs_base==8u&&g_seh_ebp==9u&&kernel_clock_peek()==clock);
        CHECK(kernel_thread_active_wait_count()==0u);
        uint32_t saved[5];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
        CHECK(memcmp(saved,frame,sizeof(frame))==0);
        size_t trace_count;
        const thunk_trace_entry *trace=thunk_trace_entries(&trace_count);
        CHECK(trace_count==1u&&trace[0].ordinal==234u);
        CHECK(trace[0].return_address==frame[0]&&trace[0].implemented&&trace[0].result_known);
    }
    CHECK(mprotect((void *)(uintptr_t)(launch->stack_low+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();kernel_call_frame termination_frame={0};const uint32_t status=77u;
        if(!kernel_frame_build(&termination_frame,launch->stack_low,8u,&status,1u))abort();
        (void)kernel_hle_call(258u,&termination_frame);abort();
    }
    host_run_disarm();
}
static void refused(uint32_t handle,kernel_thread_wait_refusal reason)
{
    (void)handle;(void)reason;
    host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED,0x38004Cu,234u,"unsupported wait scope");
}
int main(void)
{
    guest_region_request request={.bytes=8192u,.alignment=4096u,.protect=PAGE_READWRITE,.state=MEM_COMMIT};
    nt_status status;const uint32_t allocation=guest_region_alloc(&request,&status);
    CHECK(allocation!=0u);const uint32_t stack=allocation+4096u-20u;
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_NONE)==0);
    kernel_hle_init();CHECK(kernel_thread_register()==10u);
    const kernel_thread_host_ops ops={.has_code=has_code,.enter=enter,.terminate=terminate,
        .termination_confirmed=confirmed,.wait_refused=refused};
    CHECK(kernel_thread_set_host_ops(&ops));
    const uint32_t create_args[10]={allocation+512u,0u,0u,0u,0u,0x123456u,0u,0u,0u,0u};
    kernel_call_frame creation={0};
    CHECK(kernel_frame_build(&creation,allocation,44u,create_args,10u));
    CHECK(kernel_hle_call(255u,&creation)==STATUS_SUCCESS);
    uint32_t handle;CHECK(kernel_guest_read_u32(allocation+512u,&handle));
    pthread_mutex_lock(&gate);while(!ready){(void)pthread_cond_wait(&condition,&gate);}pthread_mutex_unlock(&gate);
    const uint64_t zero=0u;CHECK(kernel_guest_write_bytes(allocation+1024u,&zero,sizeof(zero)));
    recomp_func_t fn=recomp_lookup_kernel(KERNEL_THUNK_VA(234u));CHECK(fn!=NULL);
    for(unsigned pass=0u;pass<4u;pass++) {
        if(pass==1u) {
            pthread_mutex_lock(&gate);release_worker=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
            CHECK(kernel_thread_join_all(5000u)==0u);
        }
        const uint32_t frame[5]={0x38004Cu,pass==2u?0xDEADBEEFu:handle,1u,
            pass==3u?1u:0u,pass==0u?allocation+1024u:0u};
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_fs_base=8u;g_seh_ebp=9u;g_esp=stack;
        const uint64_t clock=kernel_clock_peek();thunk_trace_reset();
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();CHECK(pass!=3u);
            const uint32_t expected=pass==0u?0x102u:pass==1u?STATUS_SUCCESS:STATUS_INVALID_HANDLE;
            CHECK(g_eax==expected&&g_esp==stack+20u);
        } else {
            CHECK(pass==3u);CHECK(host_run_result()->reason==HOST_STOP_KERNEL_UNIMPLEMENTED);
            CHECK(host_run_result()->ordinal==234u&&g_eax==1u&&g_esp==stack);
        }
        host_run_disarm();
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
        CHECK(g_fs_base==8u&&g_seh_ebp==9u);CHECK(kernel_clock_peek()==clock);
        CHECK(kernel_thread_active_wait_count()==0u);
        uint32_t saved[5];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
        CHECK(memcmp(saved,frame,sizeof(frame))==0);
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].ordinal==234u&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented&&trace[0].result_known==(pass!=3u));
    }
    CHECK(kernel_thread_reset());kernel_object_reset();
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(allocation));
    printf("kernel wait thunk: %u checks passed\n",checks);return 0;
}
