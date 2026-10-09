/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "d3d8_hle.h"
#include "d3d8_target_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { \
    fprintf(stderr,"target query route failed at %d: %s\n",__LINE__,#v); abort(); } } while (0)
static uint32_t allocate(uint32_t fixed,uint32_t bytes)
{
    guest_region_request request={0};
    request.fixed_base=fixed;request.bytes=bytes; request.protect=PAGE_READWRITE; request.state=MEM_COMMIT;
    nt_status status;
    uint32_t result=guest_region_alloc(&request,&status);
    CHECK(result!=0u); return result;
}
static void fatal(uint32_t address,const char *message)
{
    host_run_stop(HOST_STOP_UNIMPLEMENTED,address,0u,message);
}
int main(void)
{
    const uint32_t device=allocate(0x3E3000u,0x4000u), stack=allocate(0u,4096u);
    const uint32_t frame_va=stack+4096u-4u;
    const xdk_dispatch_entry rows[2]={{0x3D3E00u,"GetRenderTarget2",XDK_MODULE_D3D8},
                                    {0x3D3E20u,"GetDepthStencilSurface2",XDK_MODULE_D3D8}};
    const d3d8_surface_entry surface[2]={{rows[0].address,rows[0].name,1u},{rows[1].address,rows[1].name,1u}};
    CHECK(kernel_guest_write_u32(0x3E3F58u,0x3E3F60u));
    CHECK(xdk_thunk_init(rows,2u));
    for(unsigned i=0u;i<2u;i++)CHECK(xdk_thunk_declare_abi(rows[i].address,XDK_CC_STDCALL,0u,0u));
    CHECK(d3d8_hle_init(surface,2u));d3d8_hle_set_fatal(fatal);
    CHECK(d3d8_target_query_register()==2u);
    for(unsigned kind=0u;kind<2u;kind++)for(unsigned route=0u;route<2u;route++)for(unsigned empty=0u;empty<2u;empty++){
        const uint32_t header=0x3E5984u+kind*0x48u,binding=0x3E5964u+kind*4u;
        const uint32_t pointer=empty==0u?header:0u;
        const uint32_t common=0x010D0003u-kind,caller=0x1EE97u+kind*10u;
        CHECK(kernel_guest_write_u32(binding,pointer));CHECK(kernel_guest_write_u32(header,common));
        CHECK(kernel_guest_write_u32(frame_va,caller));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=frame_va;g_fs_base=0x12340000u;g_seh_ebp=0xFFFFFFFFu;thunk_trace_reset();
        recomp_func_t fn=route==0u?recomp_lookup(rows[kind].address):recomp_lookup_manual(rows[kind].address);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0){
            host_run_arm();fn();CHECK(g_eax==pointer&&g_esp==frame_va+4u);
            CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
            CHECK(g_fs_base==0x12340000u&&g_seh_ebp==0xFFFFFFFFu);
        }else{CHECK(false);}host_run_disarm();
        uint32_t saved;CHECK(kernel_guest_read_u32(header,&saved)&&saved==common+(empty==0u?1u:0u));
        CHECK(kernel_guest_read_u32(binding,&saved)&&saved==pointer);
        CHECK(kernel_guest_read_u32(frame_va,&saved)&&saved==caller);
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==rows[kind].address&&trace[0].return_address==caller);
        CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==pointer);
    }
    xdk_thunk_shutdown();d3d8_hle_shutdown();CHECK(guest_region_free(stack));CHECK(guest_region_free(device));
    printf("compiled target query routes: %u checks passed\n",checks);return 0;
}
