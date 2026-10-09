/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "d3d8_hle.h"
#include "d3d8_shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { \
    fprintf(stderr,"vertex program route failed at %d: %s\n",__LINE__,#v); abort(); } } while (0)
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
    const uint32_t device=allocate(0x3E3000u,0x4000u);
    const uint32_t stack=allocate(0u,4096u), input=allocate(0u,4096u), stream=allocate(0u,4096u);
    const uint32_t frame_va=stack+4096u-12u;
    const xdk_dispatch_entry row={0x3D59E0u,"D3DDevice_LoadVertexShader",XDK_MODULE_D3D8};
    const d3d8_surface_entry utility={row.address,row.name,1u};
    CHECK(kernel_guest_write_u32(0x3E3F58u,0x3E3F60u));
    CHECK(xdk_thunk_init(&row,1u));
    CHECK(xdk_thunk_declare_abi(row.address,XDK_CC_STDCALL,2u,0u));
    CHECK(d3d8_hle_init(&utility,1u));d3d8_hle_set_fatal(fatal);
    CHECK(d3d8_shader_register()==1u);
    uint32_t source[17];source[0]=0x00042078u;
    for(unsigned i=1u;i<17u;i++)source[i]=0x10203040u+i;
    CHECK(kernel_guest_write_bytes(input,source,sizeof(source)));
    for(unsigned route=0u;route<2u;route++)for(unsigned mode=0u;mode<2u;mode++) {
        recomp_func_t fn=route==0u?recomp_lookup(row.address):recomp_lookup_manual(row.address);
        CHECK(fn!=NULL);
        const uint32_t frame[3]={0x226F1u,input,0u};
        CHECK(kernel_guest_write_bytes(frame_va,frame,sizeof(frame)));
        CHECK(kernel_guest_write_u32(0x3E3F60u,stream));
        CHECK(kernel_guest_write_u32(0x3E3F64u,stream+4096u));
        CHECK(kernel_guest_write_u32(0x3E3F68u,mode==0u?0x4203u:0x4213u));
        uint32_t old_cache[16];for(unsigned i=0u;i<16u;i++)old_cache[i]=0xAAAAAAAAu;
        CHECK(kernel_guest_write_bytes(0x3E5008u,old_cache,sizeof(old_cache)));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=frame_va;g_fs_base=0x12340000u;g_seh_ebp=0xFFFFFFFFu;thunk_trace_reset();
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();
            CHECK(g_eax==stream+76u&&g_esp==frame_va+12u);
            CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
            CHECK(g_fs_base==0x12340000u&&g_seh_ebp==0xFFFFFFFFu);
        }else{CHECK(false);}
        host_run_disarm();
        uint32_t packet[19],cache[16],saved[3];
        CHECK(kernel_guest_read_bytes(stream,packet,sizeof(packet)));
        CHECK(packet[0]==0x41E9Cu&&packet[1]==0u&&packet[2]==0x400B00u);
        CHECK(memcmp(packet+3,source+1,64u)==0);
        CHECK(kernel_guest_read_bytes(0x3E5008u,cache,sizeof(cache)));
        CHECK(memcmp(cache,mode==0u?source+1:old_cache,sizeof(cache))==0);
        CHECK(kernel_guest_read_bytes(frame_va,saved,sizeof(saved))&&memcmp(frame,saved,sizeof(frame))==0);
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==row.address&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==stream+76u);
        CHECK(kernel_guest_write_u32(frame_va+4u,UINT32_MAX));
        g_eax=0xDEADBEEFu;g_esp=frame_va;thunk_trace_reset();
        if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();fn();CHECK(false);}
        else{
            CHECK(host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
            CHECK(host_run_result()->guest_address==row.address);
            CHECK(g_eax==0xDEADBEEFu&&g_esp==frame_va);
            trace=thunk_trace_entries(&count);
            CHECK(count==1u&&trace[0].implemented&&!trace[0].result_known);
        }
        host_run_disarm();
    }
    xdk_thunk_shutdown();d3d8_hle_shutdown();
    CHECK(guest_region_free(stream));CHECK(guest_region_free(input));
    CHECK(guest_region_free(stack));CHECK(guest_region_free(device));
    printf("compiled vertex program routes: %u checks passed\n",checks);return 0;
}
