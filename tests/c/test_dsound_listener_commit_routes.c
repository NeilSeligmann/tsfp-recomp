/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_listener.h"
#include "host_runtime.h"
#include "kernel_clock.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static bool irql(uint8_t *out) {*out=0u;return true;}
static void fatal(uint32_t address,const char *reason)
{host_run_stop(HOST_STOP_UNIMPLEMENTED,address,0u,reason);}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+i*4u,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t interface=load(SCRATCH_DATA),device=interface-8u;
    dsound_listener_set_enabled(true);dsound_listener_set_irql_provider(irql);
    dsound_listener_set_fatal(fatal);
    CHECK_EQ_U32(dsound_listener_cache_doppler(interface,0u,0u),0u);
    CHECK_EQ_U32(dsound_listener_cache_position(interface,0u,0u,0u,0u),0u);
    CHECK_EQ_U32(dsound_listener_cache_orientation(interface,0u,0u,0x3F800000u,0u,0x3F800000u,0u,0u),0u);
    uint8_t parent[44];CHECK(kernel_guest_read_bytes(device,parent,sizeof(parent)));
    const xdk_dispatch_entry row={0x409109u,"IDirectSound_CommitDeferredSettings",XDK_MODULE_DSOUND};
    CHECK(xdk_thunk_init(&row,1u));CHECK(xdk_thunk_declare_abi(row.address,XDK_CC_STDCALL,1u,0u));
    CHECK_EQ_U32(dsound_listener_register(),5u);
    guest_region_request request={.bytes=8192u,.alignment=4096u,.protect=PAGE_READWRITE,.state=MEM_COMMIT};
    nt_status status;const uint32_t allocation=guest_region_alloc(&request,&status);
    CHECK(allocation!=0u);const uint32_t stack=allocation+4096u-8u;
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_NONE)==0);
    for(unsigned route=0u;route<4u;route++) {
        const uint32_t frame[2]={route==3u?0x28880u:0x2887Fu,route==2u?interface+4u:interface};
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        dsound_listener_snapshot before;CHECK(dsound_listener_get_snapshot(interface,&before));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=stack;g_fs_base=8u;thunk_trace_reset();const uint64_t clock=kernel_clock_peek();
        recomp_func_t fn=route==1u?recomp_lookup_manual(row.address):recomp_lookup(row.address);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();CHECK(route<2u);CHECK(g_eax==0u&&g_esp==stack+8u);
        } else {
            CHECK(route>=2u&&host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
            CHECK(host_run_result()->guest_address==row.address&&g_eax==1u&&g_esp==stack);
        }
        host_run_disarm();
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u&&g_fs_base==8u);
        CHECK(kernel_clock_peek()==clock);
        uint32_t saved[2];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
        CHECK(memcmp(saved,frame,sizeof(frame))==0);
        uint8_t actual[44];CHECK(kernel_guest_read_bytes(device,actual,44u)&&memcmp(actual,parent,44u)==0);
        dsound_listener_snapshot after;CHECK(dsound_listener_get_snapshot(interface,&after));
        if(route>=2u) {CHECK(memcmp(&after,&before,sizeof(after))==0);}
        else {CHECK(after.commit_seen);after.commit_seen=before.commit_seen;
            CHECK(memcmp(&after,&before,sizeof(after))==0);}
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==row.address&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented&&trace[0].result_known==(route<2u));
    }
    dsound_listener_reset();
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(allocation));xdk_thunk_shutdown();
    CHECK(dsound_device_reset_checked());environment_end();
    printf("listener commit routes: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
