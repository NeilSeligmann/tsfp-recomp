/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "xdk_thunk.h"
#include "dsound_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "host_runtime.h"
#include "thunk_trace.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
TSFP_RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_fs_base;
#define STATUS 0x4073D3u
#define DISCONT 0x40733Bu
#define STREAM 0x12345678u
#define SENTINEL 0xABCD0011u
static unsigned checks, failures;
#define CHECK(x) do { checks++; if(!(x)) { failures++; printf("FAIL %d %s\n",__LINE__,#x); } } while(0)
static uint32_t missing,wrong,dispatch_missing,dispatch_wrong;
#ifndef STREAM_STATUS_NO_MARKERS
int recomp_has_stop_boundary(uint32_t address)
{ return address==missing?0:address==wrong?2:1; }
int recomp_has_dispatch_boundary(uint32_t address)
{ return address==dispatch_missing?0:address==dispatch_wrong?2:1; }
#endif
static uint32_t stack,out;
static void *guard;
static unsigned behavior;
static _Thread_local unsigned depth;
static _Thread_local bool returned,configuration_refused;
static _Thread_local host_stop stopped;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool entered,released;
static uint32_t status_handler(uint32_t stream,uint32_t output);
static uint32_t discont_handler(uint32_t stream);
static void nested(uint32_t address)
{
    uint32_t saved_eax=g_eax,saved_esp=g_esp;
    const uint32_t words[3]={address==STATUS?0x29CEAu:0x29B72u,STREAM,out+16u};
    if(!kernel_guest_write_bytes(stack+128u,words,address==STATUS?12u:8u))
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,address,0u,"nested frame failure");
    g_esp=stack+128u;depth++;
    recomp_func_t fn=recomp_lookup_manual(address);fn();
    depth--;g_eax=saved_eax;g_esp=saved_esp;
}
static uint32_t discont_handler(uint32_t stream)
{
    if(stream!=STREAM)host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,DISCONT,0u,"not owned");
    if(behavior==4u && depth==0u)nested(STATUS);
    if(behavior==5u && depth!=0u)host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,DISCONT,77u,"nested refusal");
    return 0u;
}
static uint32_t status_handler(uint32_t stream,uint32_t output)
{
    configuration_refused=!xdk_thunk_set_stream_virtual_handlers(NULL,NULL) &&
        !xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler);
    if(stream!=STREAM || behavior==1u)
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,STATUS,0u,"typed status refusal");
    if(behavior==2u)*(volatile uint8_t *)guard=1u;
    if((behavior==3u || behavior==5u) && depth==0u)nested(DISCONT);
    if(behavior==6u){
        pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&condition);
        while(!released)(void)pthread_cond_wait(&condition,&gate);
        pthread_mutex_unlock(&gate);
    }
    uint32_t previous;
    if(!kernel_guest_read_u32(output,&previous) || !kernel_guest_write_u32(output,previous) ||
       !kernel_guest_write_u32(output,1u))
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,STATUS,0u,"typed output refusal");
    return 0u;
}
static void prepare(uint32_t at,uint32_t address,uint32_t output)
{
    const uint32_t words[3]={address==STATUS?0x29CEAu:0x29B72u,STREAM,output};
    CHECK(kernel_guest_write_bytes(at,words,address==STATUS?12u:8u));
    g_esp=at;g_eax=SENTINEL;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;g_fs_base=8u;
}
static void route(uint32_t address,bool direct)
{
    returned=false;
    if(sigsetjmp(*host_run_jmp(),1)==0){
        host_run_arm();
        if(direct)xdk_thunk_stop_at(address,"compiled method","strict direct stop");
        else {recomp_func_t fn=recomp_lookup_manual(address);if(fn)fn();else xdk_thunk_stop_at(address,"fallback","strict fallback");}
        returned=true;
    }
    stopped=*host_run_result();host_run_disarm();xdk_thunk_stream_virtual_cancel_pending();
}
static void check_state(uint32_t at,uint32_t address,bool success)
{
    CHECK(g_eax==(success?0u:SENTINEL));CHECK(g_esp==at+(success?(address==STATUS?12u:8u):0u));
    CHECK(g_ecx==2u && g_edx==3u && g_ebx==4u && g_esi==5u && g_edi==6u && g_ebp==7u && g_fs_base==8u);
    CHECK(xdk_thunk_stream_virtual_active_count()==0u);CHECK(xdk_thunk_stream_virtual_pending_count()==0u);
    CHECK(host_run_scope_depth()==0u);
}
static void trace(uint32_t address,bool implemented,bool known)
{
    size_t count;const thunk_trace_entry *entries=thunk_trace_entries(&count);
    CHECK(count==1u);if(count!=1u)return;
    CHECK(entries[0].kind==THUNK_KIND_XDK && entries[0].ordinal==0u && entries[0].address==address);
    CHECK(entries[0].return_address==(address==STATUS?0x29CEAu:0x29B72u));
    CHECK(entries[0].implemented==implemented && entries[0].result_known==known);
    if(known)CHECK(entries[0].result==0u);
}
static void *worker(void *unused)
{
    (void)unused;
    const uint32_t words[3]={0x29CEAu,STREAM,out};
    (void)kernel_guest_write_bytes(stack+256u,words,sizeof(words));g_esp=stack+256u;g_eax=SENTINEL;
    route(STATUS,false);return NULL;
}
int main(void)
{
    dsound_hle_init();size_t n;const dsound_entry *sound=dsound_hle_table(&n);CHECK(n==41u);
    xdk_dispatch_entry entries[41];for(unsigned i=0u;i<41u;i++)entries[i]=(xdk_dispatch_entry){sound[i].address,sound[i].name,XDK_MODULE_DSOUND};
    CHECK(!xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));CHECK(xdk_thunk_init(entries,41u));
    guest_region_request request={0};request.bytes=8192u;request.state=MEM_COMMIT;request.protect=PAGE_READWRITE;
    nt_status result=STATUS_SUCCESS;stack=guest_region_alloc(&request,&result);CHECK(stack!=0u);out=stack+4096u;
    guard=mmap(NULL,4096u,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);
    prepare(stack,STATUS,out);route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);
#ifdef STREAM_STATUS_NO_MARKERS
    CHECK(!xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));
    if(missing==0u)goto cleanup;
#endif
    missing=STATUS;CHECK(!xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));missing=0u;
    wrong=STATUS;CHECK(!xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));wrong=0u;
    for(unsigned i=0u;i<41u;i++){
        dispatch_missing=entries[i].address;CHECK(!xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));dispatch_missing=0u;
        dispatch_wrong=entries[i].address;CHECK(!xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));dispatch_wrong=0u;
    }
    CHECK(xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));
    prepare(stack,STATUS,out);recomp_func_t first=recomp_lookup_manual(STATUS);CHECK(first!=NULL);
    CHECK(recomp_lookup_manual(STATUS)==first);CHECK(xdk_thunk_stream_virtual_pending_count()==1u);
    CHECK(!xdk_thunk_set_stream_virtual_handler(NULL));CHECK(!xdk_thunk_init(entries,41u));
    uint64_t before_trace=thunk_trace_total();
    if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();(void)recomp_lookup_manual(DISCONT);CHECK(false);}
    else CHECK(host_run_result()->guest_address==DISCONT);
    host_run_disarm();CHECK(thunk_trace_total()==before_trace);CHECK(xdk_thunk_stream_virtual_pending_count()==1u);
    xdk_thunk_stream_virtual_cancel_pending();xdk_thunk_stream_virtual_cancel_pending();
    /* A cached status wrapper cannot consume a later Discontinuity reservation. */
    CHECK(recomp_lookup_manual(DISCONT)!=NULL);
    if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();first();CHECK(false);}
    else CHECK(host_run_result()->guest_address==STATUS);
    host_run_disarm();CHECK(xdk_thunk_stream_virtual_pending_count()==1u);xdk_thunk_stream_virtual_cancel_pending();
    CHECK(xdk_thunk_set_stream_virtual_handler(discont_handler));CHECK(recomp_lookup_manual(STATUS)==NULL);
    CHECK(xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));CHECK(xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(recomp_lookup_manual(STATUS)==NULL && recomp_lookup_manual(DISCONT)==NULL);
    CHECK(xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));
    for(unsigned policy=0u;policy<2u;policy++){
        xdk_thunk_set_stop_on_missing(policy!=0u);thunk_trace_reset();prepare(stack,STATUS,out);
        CHECK(kernel_guest_write_u32(out,0xDEADBEEFu));uint8_t frame[12],after[12];CHECK(kernel_guest_read_bytes(stack,frame,12u));
        route(STATUS,false);CHECK(returned && configuration_refused);check_state(stack,STATUS,true);trace(STATUS,true,true);
        uint32_t value;CHECK(kernel_guest_read_u32(out,&value) && value==1u);CHECK(kernel_guest_read_bytes(stack,after,12u));CHECK(memcmp(frame,after,12u)==0);
        thunk_trace_reset();prepare(stack,STATUS,out);route(STATUS,true);CHECK(!returned);check_state(stack,STATUS,false);trace(STATUS,false,false);
    }
    CHECK(xdk_thunk_call_count()==0u && xdk_thunk_module_call_count(XDK_MODULE_DSOUND)==0u && xdk_thunk_count()==41u);
    const xdk_stop_override explicit_stop={STATUS,"explicit status","priority"};CHECK(xdk_thunk_stop_override_adopt(&explicit_stop,1u));
    prepare(stack,STATUS,out);route(STATUS,false);CHECK(strcmp(stopped.detail,"priority")==0);check_state(stack,STATUS,false);xdk_thunk_stop_override_reset();
    for(behavior=1u;behavior<=5u;behavior++){
        depth=0u;thunk_trace_reset();prepare(stack,behavior==4u?DISCONT:STATUS,out);route(behavior==4u?DISCONT:STATUS,false);
        bool success=behavior==3u || behavior==4u;CHECK(returned==success);check_state(stack,behavior==4u?DISCONT:STATUS,success);
        if(behavior==2u)CHECK(stopped.reason==HOST_STOP_FAULT && stopped.signal_number==SIGSEGV && stopped.fault_address==(uintptr_t)guard);
        if(behavior==5u)CHECK(stopped.guest_address==DISCONT && stopped.ordinal==77u && strcmp(stopped.detail,"nested refusal")==0);
        if(behavior>=3u){size_t count;const thunk_trace_entry *t=thunk_trace_entries(&count);CHECK(count==2u);if(count==2u)CHECK(t[0].result_known==success && t[1].result_known==success);}
        CHECK(xdk_thunk_set_stream_virtual_handlers(discont_handler,status_handler));
    }
    behavior=0u;
    for(unsigned offset=0u;offset<12u;offset++){
        thunk_trace_reset();prepare(stack,STATUS,stack+offset);route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);trace(STATUS,true,false);
    }
    prepare(stack,STATUS,UINT32_MAX-1u);route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);
    prepare(stack,STATUS,out);CHECK(kernel_guest_write_u32(stack,0x29CEBu));route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);
    /* T602: the Discontinuity re-format caller 0x29A5A is no caller of GetStatus. */
    prepare(stack,STATUS,out);CHECK(kernel_guest_write_u32(stack,0x29A5Au));route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);
    /* T605: the re-formatted stream caller 0x29D28 (sub_00029D10) is admitted, its neighbours and the Discontinuity callers are not. */
    thunk_trace_reset();prepare(stack,STATUS,out);CHECK(kernel_guest_write_u32(stack,0x29D28u));
    CHECK(kernel_guest_write_u32(out,0xDEADBEEFu));route(STATUS,false);CHECK(returned);check_state(stack,STATUS,true);
    {uint32_t value;CHECK(kernel_guest_read_u32(out,&value) && value==1u);}
    {size_t count;const thunk_trace_entry *t=thunk_trace_entries(&count);CHECK(count==1u && t[0].return_address==0x29D28u && t[0].result_known && t[0].result==0u);}
    {const uint32_t refused_callers[]={0x29D27u,0x29D29u,0x29CE9u,0x29B72u,0x29A67u,0u};
    for(unsigned i=0u;i<sizeof(refused_callers)/sizeof(refused_callers[0]);i++){
        prepare(stack,STATUS,out);CHECK(kernel_guest_write_u32(stack,refused_callers[i]));route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);
    }}
    /* The status caller does not admit Discontinuity, and the Discontinuity callers do not admit status. */
    prepare(stack,DISCONT,out);CHECK(kernel_guest_write_u32(stack,0x29D28u));route(DISCONT,false);CHECK(!returned);check_state(stack,DISCONT,false);
    prepare(stack+4096u-12u,STATUS,out+16u);route(STATUS,false);CHECK(returned);check_state(stack+4096u-12u,STATUS,true);
    CHECK(mprotect((void *)(uintptr_t)out,4096u,PROT_NONE)==0);
    g_esp=stack+4096u-8u;g_eax=SENTINEL;route(STATUS,false);CHECK(!returned);check_state(stack+4096u-8u,STATUS,false);
    CHECK(mprotect((void *)(uintptr_t)out,4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(kernel_guest_write_u32(out,0xCAFEBABEu));CHECK(mprotect((void *)(uintptr_t)out,4096u,PROT_READ)==0);
    prepare(stack,STATUS,out);route(STATUS,false);CHECK(!returned);check_state(stack,STATUS,false);
    uint32_t preserved;CHECK(kernel_guest_read_u32(out,&preserved) && preserved==0xCAFEBABEu);
    CHECK(mprotect((void *)(uintptr_t)out,4096u,PROT_READ|PROT_WRITE)==0);
    behavior=6u;entered=false;released=false;pthread_t thread;CHECK(pthread_create(&thread,NULL,worker,NULL)==0);
    pthread_mutex_lock(&gate);while(!entered)(void)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
    CHECK(xdk_thunk_stream_virtual_active_count()==1u);CHECK(!xdk_thunk_set_stream_virtual_handlers(NULL,NULL));
    xdk_thunk_stream_virtual_cancel_pending();CHECK(xdk_thunk_stream_virtual_active_count()==1u);
    prepare(stack,DISCONT,out);route(DISCONT,false);CHECK(returned);
    CHECK(g_eax==0u && g_esp==stack+8u);CHECK(xdk_thunk_stream_virtual_active_count()==1u);
    pthread_mutex_lock(&gate);released=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
    CHECK(pthread_join(thread,NULL)==0);CHECK(xdk_thunk_stream_virtual_active_count()==0u);behavior=0u;
#ifdef STREAM_STATUS_NO_MARKERS
cleanup:
#endif
    CHECK(xdk_thunk_set_stream_virtual_handler(NULL));xdk_thunk_shutdown();
    CHECK(munmap(guard,4096u)==0);CHECK(guest_region_free(stack));
    printf("stream status dispatch: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
