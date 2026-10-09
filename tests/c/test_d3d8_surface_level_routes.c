/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "d3d8_hle.h"
#include "d3d8_cube_surface.h"
#include "d3d8_reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { \
    fprintf(stderr,"surface level route failed at %d: %s\n",__LINE__,#v); abort(); } } while (0)
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
    const uint32_t metadata=allocate(0x3E1000u,0x2000u),stack=allocate(0u,4096u),texture=allocate(0u,4096u);
    const uint32_t frame_va=stack+4096u-12u;
    const xdk_dispatch_entry row={0x3D4E10u,"D3DTexture_GetSurfaceLevel2",XDK_MODULE_D3D8};
    const d3d8_surface_entry surface={row.address,row.name,1u};
    CHECK(kernel_guest_write_u8(0x3E182Eu,0xA1u));
    CHECK(xdk_thunk_init(&row,1u));CHECK(xdk_thunk_declare_abi(row.address,XDK_CC_STDCALL,2u,0u));
    CHECK(d3d8_hle_init(&surface,1u));d3d8_hle_set_fatal(fatal);
    CHECK(d3d8_cube_surface_register()==1u);
    for(unsigned route=0u;route<2u;route++){
        const uint32_t source[5]={0x40001u,0x00100000u,0u,0x04410629u,0u};
        const uint32_t frame[3]={0x1EE28u,texture,1u};
        CHECK(kernel_guest_write_bytes(texture,source,sizeof(source)));
        CHECK(kernel_guest_write_bytes(frame_va,frame,sizeof(frame)));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=frame_va;g_fs_base=0x12340000u;g_seh_ebp=0xFFFFFFFFu;thunk_trace_reset();
        recomp_func_t fn=route==0u?recomp_lookup(row.address):recomp_lookup_manual(row.address);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0){
            host_run_arm();fn();CHECK(g_eax!=0u&&g_esp==frame_va+12u);
            CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
            CHECK(g_fs_base==0x12340000u&&g_seh_ebp==0xFFFFFFFFu);
        }else{CHECK(false);}host_run_disarm();
        const uint32_t header=g_eax;
        CHECK(d3d8_cube_surface_owned(header));
        uint32_t actual[6],expected[6]={0x01050001u,0x00100400u,0u,0x03310629u,0u,texture};
        CHECK(kernel_guest_read_bytes(header,actual,sizeof(actual))&&memcmp(actual,expected,sizeof(actual))==0);
        uint32_t parent[5];CHECK(kernel_guest_read_bytes(texture,parent,sizeof(parent)));
        CHECK(parent[0]==source[0]+1u&&memcmp(parent+1,source+1,16u)==0);
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==row.address&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==header);
        CHECK(d3d8_reference_release(header)==0u);
        CHECK(!d3d8_cube_surface_owned(header)&&d3d8_cube_surface_retired(header));
        CHECK(kernel_guest_read_bytes(texture,parent,sizeof(parent))&&memcmp(parent,source,sizeof(parent))==0);
        CHECK(kernel_guest_write_u32(frame_va+4u,UINT32_MAX));
        g_eax=0xDEADBEEFu;g_esp=frame_va;thunk_trace_reset();
        if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();fn();CHECK(false);}
        else{
            CHECK(host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
            CHECK(host_run_result()->guest_address==row.address);
            CHECK(g_eax==0xDEADBEEFu&&g_esp==frame_va);
            trace=thunk_trace_entries(&count);CHECK(count==1u&&trace[0].implemented&&!trace[0].result_known);
        }host_run_disarm();
    }
    d3d8_cube_surface_reset();xdk_thunk_shutdown();d3d8_hle_shutdown();
    CHECK(guest_region_free(texture));CHECK(guest_region_free(stack));CHECK(guest_region_free(metadata));
    printf("compiled surface level routes: %u checks passed\n",checks);return 0;
}
