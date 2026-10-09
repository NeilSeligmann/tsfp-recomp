/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_thunk.h"
#include "kernel_object.h"
#include "kernel_thread.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { fprintf(stderr,"thread object route %d: %s\n",__LINE__,#v); abort(); } } while(0)
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool ready,release_worker;
static uint32_t pseudo_scratch,pseudo_status;
/* NtCurrentThread() (-2) through ordinal 246 from the calling thread. */
static uint32_t ref_current_thread(uint32_t frame_at,uint32_t out_at)
{
    const uint32_t args[3]={0xFFFFFFFEu,0u,out_at};kernel_call_frame frame={0};
    if(!kernel_frame_build(&frame,frame_at,16u,args,3u)) abort();
    return kernel_hle_call(246u,&frame);
}
static bool has_code(uint32_t address) { return address==0x123456u; }
static void terminate(uint32_t status)
{
    (void)status;host_run_stop(HOST_STOP_THREAD_EXITED,0u,0u,"object route termination");
}
static bool confirmed(const kernel_thread_launch *launch)
{
    (void)launch;return host_run_result()->reason==HOST_STOP_THREAD_EXITED;
}
static void enter(const kernel_thread_launch *launch)
{
    pseudo_status=ref_current_thread(pseudo_scratch,pseudo_scratch+64u);
    pthread_mutex_lock(&gate);ready=true;pthread_cond_broadcast(&condition);
    while(!release_worker) (void)pthread_cond_wait(&condition,&gate);
    pthread_mutex_unlock(&gate);
    if(sigsetjmp(*host_run_jmp(),1)==0) {
        host_run_arm();kernel_call_frame frame={0};const uint32_t status=0xC1234567u;
        if(!kernel_frame_build(&frame,launch->stack_low,8u,&status,1u)) abort();
        (void)kernel_hle_call(258u,&frame);abort();
    }
    host_run_disarm();
}
static uint32_t allocate(uint32_t fixed,uint32_t bytes)
{
    guest_region_request request={.fixed_base=fixed,.bytes=bytes,.alignment=4096u,
        .protect=PAGE_READWRITE,.state=MEM_COMMIT};nt_status status;
    const uint32_t address=guest_region_alloc(&request,&status);CHECK(address!=0u);return address;
}
int main(void)
{
    const uint32_t imports=allocate(0x475000u,4096u),scratch=allocate(0u,8192u);
    const uint32_t stack=scratch+4096u-12u,output=scratch+512u;
    CHECK(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_NONE)==0);
    pseudo_scratch=allocate(0u,4096u);
    kernel_hle_init();CHECK(kernel_thread_register()==10u);CHECK(kernel_object_register()!=0u);
    CHECK(kernel_guest_write_u32(0x4757DCu,KERNEL_THUNK_VA(246u)));
    CHECK(kernel_guest_write_u32(0x4757D4u,KERNEL_THUNK_VA(250u)));
    const kernel_thread_host_ops ops={.has_code=has_code,.enter=enter,.terminate=terminate,
        .termination_confirmed=confirmed};CHECK(kernel_thread_set_host_ops(&ops));
    const uint32_t create[10]={scratch+256u,0u,0u,0u,0u,0x123456u,0u,0u,0u,0u};
    kernel_call_frame creation={0};CHECK(kernel_frame_build(&creation,scratch,44u,create,10u));
    CHECK(kernel_hle_call(255u,&creation)==STATUS_SUCCESS);
    uint32_t handle;CHECK(kernel_guest_read_u32(scratch+256u,&handle));
    pthread_mutex_lock(&gate);while(!ready) (void)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
    /* The worker referenced NtCurrentThread: success, its own mapped KTHREAD, one more reference. */
    {
        uint32_t body=0u;kernel_object_entry own;
        CHECK(pseudo_status==STATUS_SUCCESS);
        CHECK(kernel_guest_read_u32(pseudo_scratch+64u,&body)&&body!=0u);
        CHECK(kernel_object_get_thread_body_copy(body,&own));
        CHECK(own.handle==handle&&own.thread_body_references==1u&&own.references>=2u);
        /* The boot thread has no record: refused, out slot untouched. */
        CHECK(kernel_guest_write_u32(pseudo_scratch+128u,0xA5A5A5A5u));
        CHECK(ref_current_thread(pseudo_scratch+256u,pseudo_scratch+128u)==STATUS_INVALID_HANDLE);
        uint32_t untouched=0u;CHECK(kernel_guest_read_u32(pseudo_scratch+128u,&untouched)&&untouched==0xA5A5A5A5u);
        /* The balancing ObfDereferenceObject releases exactly that reference. */
        kernel_call_frame release={0};const uint32_t none[1]={0u};
        CHECK(kernel_frame_build(&release,pseudo_scratch+512u,16u,none,0u));
        kernel_frame_set_registers(&release,body,0u);(void)kernel_hle_call(250u,&release);
        CHECK(!kernel_object_get_thread_body_copy(body,&own)); /* mapped only while body references remain */
    }
    recomp_func_t fn=recomp_lookup(0x37FD83u);CHECK(fn!=NULL);
    for(unsigned pass=0u;pass<2u;pass++) {
        if(pass!=0u) {
            pthread_mutex_lock(&gate);release_worker=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
            CHECK(kernel_thread_join_all(5000u)==0u);
        }
        const uint32_t frame[3]={0x12345678u,handle,output};
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        CHECK(kernel_guest_write_u32(output-4u,0xAA55AA55u));
        CHECK(kernel_guest_write_u32(output,0xDEADBEEFu));
        CHECK(kernel_guest_write_u32(output+4u,0x55AA55AAu));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=stack;g_fs_base=8u;g_seh_ebp=9u;thunk_trace_reset();
        if(sigsetjmp(*host_run_jmp(),1)==0) {host_run_arm();fn();} else {CHECK(false);}
        host_run_disarm();CHECK(g_eax==1u&&g_esp==stack+12u);
        CHECK(g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
        CHECK(g_fs_base==8u); /* Lifted g_seh_ebp is a compiler shadow, not an ABI register. */
        uint32_t actual[3];CHECK(kernel_guest_read_bytes(output-4u,actual,sizeof(actual)));
        CHECK(actual[0]==0xAA55AA55u&&actual[1]==(pass?0xC1234567u:0x103u)&&actual[2]==0x55AA55AAu);
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==2u&&trace[0].ordinal==246u&&trace[1].ordinal==250u);
        CHECK(trace[0].result_known&&trace[1].result_known&&trace[0].result==STATUS_SUCCESS);
    }
    CHECK(kernel_thread_reset());kernel_object_reset();
    CHECK(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(scratch));CHECK(guest_region_free(imports));
    printf("thread object routes: %u checks passed\n",checks);return 0;
}
