/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#include "host_runtime.h"
#include "kernel_clock.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "xdk_surface.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static bool irql(uint8_t *out) {*out=0u;return true;}
static void fatal(uint32_t address,const char *reason)
{host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED,address,0u,reason);}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+i*4u,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t device=load(SCRATCH_DATA)-8u,desc=SCRATCH_DATA+256u,format=SCRATCH_DATA+320u;
    const uint32_t d[6]={0u,3u,format,0u,0u,0u};
    const uint16_t initial[2]={0x69u,2u},rest[4]={72u,4u,2u,64u};
    const uint32_t rates[2]={44100u,49612u};uint8_t f[20];
    memcpy(f,initial,4u);memcpy(f+4u,rates,8u);memcpy(f+12u,rest,8u);
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(irql);dsound_stream_set_fatal(fatal);
    CHECK_EQ_U32(dsound_stream_create(desc,SCRATCH_DATA+128u),0u);
    const uint32_t stream=load(SCRATCH_DATA+128u);
    uint8_t header[40],parent[44];
    CHECK(kernel_guest_read_bytes(stream,header,sizeof(header)));
    CHECK(kernel_guest_read_bytes(device,parent,sizeof(parent)));


    CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream,0u,0u,1u),0u);
    xdk_dispatch_entry rows[XDK_SURFACE_COUNT];
    for(size_t i=0u;i<XDK_SURFACE_COUNT;i++) {
        rows[i]=(xdk_dispatch_entry){xdk_surface[i].address,xdk_surface[i].name,
                                    xdk_module_for_section(xdk_surface[i].section)};
    }
    CHECK(xdk_thunk_init(rows,XDK_SURFACE_COUNT));
    CHECK(recomp_lookup_manual(0x40733Bu)==NULL);
    CHECK(xdk_thunk_set_stream_virtual_handler(dsound_stream_cache_discontinuity));
    CHECK_EQ_U32(dsound_stream_register(), 10u);
    guest_region_request request={.bytes=8192u,.alignment=4096u,.protect=PAGE_READWRITE,.state=MEM_COMMIT};
    nt_status status;const uint32_t allocation=guest_region_alloc(&request,&status);
    CHECK(allocation!=0u);const uint32_t stack=allocation+4096u-8u;
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_NONE)==0);
    for(unsigned route=0u;route<4u;route++) {
        const uint32_t frame[2]={route==2u?0x29B73u:0x29B72u,stream};
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        dsound_stream_snapshot before;CHECK(dsound_stream_get_snapshot(stream,&before));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=stack;g_fs_base=8u;thunk_trace_reset();const uint64_t clock=kernel_clock_peek();
        recomp_func_t fn=route==3u?recomp_lookup(0x40733Bu):recomp_lookup_manual(0x40733Bu);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();CHECK(route<2u);CHECK(g_eax==0u&&g_esp==stack+8u);
        } else {
            CHECK(route>=2u&&host_run_result()->reason==HOST_STOP_XDK_UNIMPLEMENTED);
            CHECK(host_run_result()->guest_address==0x40733Bu&&g_eax==1u&&g_esp==stack);
        }
        host_run_disarm();
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u&&g_fs_base==8u);
        CHECK(kernel_clock_peek()==clock);
        uint32_t saved[2];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
        CHECK(memcmp(saved,frame,sizeof(frame))==0);
        uint8_t actual[44];CHECK(kernel_guest_read_bytes(stream,actual,40u)&&memcmp(actual,header,40u)==0);
        CHECK(kernel_guest_read_bytes(device,actual,44u)&&memcmp(actual,parent,44u)==0);
        dsound_stream_snapshot after;CHECK(dsound_stream_get_snapshot(stream,&after));
        if(route>=2u) {CHECK(memcmp(&after,&before,sizeof(after))==0);}
        else {CHECK(after.pause_seen&&after.pause_mode==1u&&after.cache_mask==0u);
            CHECK(after.discontinuity_seen&&after.flush_seen&&after.flush_time_low==0u&&after.flush_time_high==0u&&after.flush_flags==1u);}
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u);
        CHECK(trace[0].address==0x40733Bu&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented==(route!=3u)&&trace[0].result_known==(route<2u));
    }
    CHECK(xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(recomp_lookup_manual(0x40733Bu)==NULL);
    CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(allocation));xdk_thunk_shutdown();environment_end();
    printf("stream virtual routes: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
