/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_event_handle.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_thunk.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) {fprintf(stderr,"event route %d: %s\n",__LINE__,#v);abort();}} while(0)
recomp_func_t recomp_lookup_kernel(uint32_t address);
static uint32_t allocate(uint32_t fixed,uint32_t bytes)
{
    guest_region_request request={.fixed_base=fixed,.bytes=bytes,.alignment=4096u,
        .protect=PAGE_READWRITE,.state=MEM_COMMIT};nt_status status;
    uint32_t result=guest_region_alloc(&request,&status);CHECK(result!=0u);return result;
}
static void seed(uint32_t stack,uint32_t pcr)
{
    g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
    g_esp=stack;g_fs_base=pcr;thunk_trace_reset();
}
static void invoke(recomp_func_t fn)
{
    if(sigsetjmp(*host_run_jmp(),1)==0) {host_run_arm();fn();} else {CHECK(false);}
    host_run_disarm();
}
int main(void)
{
    uint32_t imports=allocate(0x475000u,4096u),tls_index=allocate(0x771000u,4096u);
    uint32_t scratch=allocate(0u,8192u),pcr=allocate(0u,4096u);
    uint32_t stack=scratch+4096u-20u;
    CHECK(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_NONE)==0);
    CHECK(kernel_guest_write_u32(pcr+4u,pcr+0x100u));
    CHECK(kernel_guest_write_u32(pcr+0x100u,pcr+0x200u));
    CHECK(kernel_guest_write_u32(pcr+0x204u,0xDEADBEEFu));
    CHECK(kernel_guest_write_u32(0x475898u,KERNEL_THUNK_VA(189u)));
    kernel_hle_init();CHECK(kernel_object_register()!=0u);CHECK(kernel_event_handle_register()==2u);
    const uint32_t original_frame[5]={0x12345678u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(stack,original_frame,sizeof(original_frame)));
    seed(stack,pcr);recomp_func_t wrapper=recomp_lookup(0x37FF30u);CHECK(wrapper!=NULL);
    uint64_t clock=kernel_clock_peek();invoke(wrapper);uint32_t handle=g_eax;
    CHECK(handle!=0u&&g_esp==stack+20u);
    CHECK(g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u&&g_fs_base==pcr);
    CHECK(kernel_clock_peek()==clock);
    uint32_t last_error;CHECK(kernel_guest_read_u32(pcr+0x204u,&last_error));CHECK(last_error==0u);
    uint32_t saved[5];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
    CHECK(memcmp(saved,original_frame,16u)==0&&saved[4]==handle);
    kernel_object_entry entry;CHECK(kernel_object_get_copy(handle,&entry)&&entry.kind==KERNEL_OBJECT_EVENT);
    CHECK(kernel_object_live_count()==1u);CHECK(kernel_object_release(handle));
    CHECK(kernel_object_live_count()==0u);
    size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
    CHECK(count==1u&&trace[0].ordinal==189u&&trace[0].return_address==0x37FF67u);
    CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==STATUS_SUCCESS);
    /* T270: the sixth caller, XNET 0x431D5F, passes bManualReset 1 (lpEventAttributes 0, bInitialState 0,
     * lpName 0), so the wrapper hands NtCreateEvent Type 0 (NotificationEvent). It now succeeds. */
    const uint32_t manual_frame[5]={0x12345678u,0u,1u,0u,0u};
    CHECK(kernel_guest_write_bytes(stack,manual_frame,sizeof(manual_frame)));
    seed(stack,pcr);clock=kernel_clock_peek();invoke(wrapper);handle=g_eax;
    CHECK(handle!=0u&&g_esp==stack+20u&&kernel_clock_peek()==clock);
    CHECK(kernel_guest_read_u32(pcr+0x204u,&last_error));CHECK(last_error==0u);
    CHECK(kernel_object_get_copy(handle,&entry)&&entry.kind==KERNEL_OBJECT_EVENT);
    CHECK(entry.event_type==0u&&entry.event_initial_state==0u);
    CHECK(kernel_object_live_count()==1u);CHECK(kernel_object_release(handle));
    trace=thunk_trace_entries(&count);
    CHECK(count==1u&&trace[0].ordinal==189u&&trace[0].return_address==0x37FF67u);
    CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==STATUS_SUCCESS);
    recomp_func_t thunk=recomp_lookup_kernel(KERNEL_THUNK_VA(189u));CHECK(thunk!=NULL);
    /* Raw thunk: Type 0 and Type 1 succeed, Type 2 is still refused with the output untouched. */
    for(unsigned pass=0u;pass<3u;pass++) {
        const uint32_t frame[5]={0x12345678u,scratch+512u,0u,pass==0u?1u:pass==1u?0u:2u,0u};
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        CHECK(kernel_guest_write_u32(scratch+512u,0xDEADBEEFu));
        seed(stack,pcr);clock=kernel_clock_peek();invoke(thunk);
        CHECK(g_eax==(pass==2u?STATUS_NOT_IMPLEMENTED:STATUS_SUCCESS)&&g_esp==stack+20u);
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u&&g_fs_base==pcr);
        CHECK(kernel_clock_peek()==clock);
        CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved))&&memcmp(saved,frame,sizeof(frame))==0);
        uint32_t output;CHECK(kernel_guest_read_u32(scratch+512u,&output));
        if(pass==2u) {CHECK(output==0xDEADBEEFu&&kernel_object_live_count()==0u);}
        else {
            CHECK(output!=0u&&kernel_object_live_count()==1u);
            CHECK(kernel_object_get_copy(output,&entry)&&entry.event_type==(pass==0u?1u:0u));
            CHECK(kernel_object_release(output));
        }
        trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].ordinal==189u&&trace[0].result_known&&trace[0].result==g_eax);
    }
    kernel_object_reset();
    CHECK(mprotect((void *)(uintptr_t)(scratch+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(scratch));CHECK(guest_region_free(pcr));
    CHECK(guest_region_free(imports));CHECK(guest_region_free(tls_index));
    printf("event routes: %u checks passed\n",checks);return 0;
}
