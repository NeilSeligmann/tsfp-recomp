/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "d3d8_hle.h"
#include "d3d8_scissor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { \
    fprintf(stderr,"scissor route failed at %d: %s\n",__LINE__,#v); abort(); } } while (0)
static uint32_t allocate(uint32_t fixed,uint32_t bytes)
{
    guest_region_request request={0};
    request.bytes=bytes;request.fixed_base=fixed;
    request.protect=PAGE_READWRITE;request.state=MEM_COMMIT;
    nt_status status;
    uint32_t result=guest_region_alloc(&request,&status);
    CHECK(result!=0u);return result;
}
int main(void)
{
    const uint32_t device_region=allocate(0x3E3000u,0x4000u);
    const uint32_t bias_region=allocate(0x475000u,0x1000u);
    const uint32_t stack=allocate(0u,0x1000u);
    const uint32_t stream=allocate(0u,0x1000u);
    const uint32_t rect_va=stack+512u;
    const uint32_t device=0x3E3F60u,rect[4]={0u,0u,640u,480u};
    CHECK(kernel_guest_write_u32(0x3E3F58u,device));
    CHECK(kernel_guest_write_u32(device+4u,stream+0x1000u));
    CHECK(kernel_guest_write_u32(0x475CD4u,0x3F000000u));
    CHECK(kernel_guest_write_bytes(rect_va,rect,sizeof(rect)));
    const xdk_dispatch_entry row={0x3D4470u,"D3DDevice_SetScissors",XDK_MODULE_D3D8};
    const d3d8_surface_entry utility={0x3D4470u,"D3DDevice_SetScissors",1u};
    CHECK(xdk_thunk_init(&row,1u));
    CHECK(xdk_thunk_declare_abi(row.address,XDK_CC_STDCALL,3u,0u));
    CHECK(d3d8_hle_init(&utility,1u));
    CHECK(d3d8_scissor_register()==1u);
    for(unsigned route=0u;route<2u;route++) {
        const uint32_t frame[4]={0x1A0C4u,1u,0u,rect_va};
        uint8_t before[64],after[64];memset(before,0xA5u,sizeof(before));
        CHECK(kernel_guest_write_bytes(stream,before,sizeof(before)));
        CHECK(kernel_guest_write_u32(device,stream));
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;g_esp=stack;
        g_fs_base=0x12340000u;g_seh_ebp=0xFFFFFFFFu;
        thunk_trace_reset();
        recomp_func_t fn=route==0u?recomp_lookup(row.address):recomp_lookup_manual(row.address);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();
            CHECK(g_eax==16u&&g_esp==stack+16u);
            CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u);
            CHECK(g_esi==5u&&g_edi==6u&&g_ebp==7u);
            CHECK(g_fs_base==0x12340000u&&g_seh_ebp==0xFFFFFFFFu);
        } else {CHECK(false);}
        host_run_disarm();
        const uint32_t packet[9]={0x80200u,0u,0u,0x402B4u,0u,0x402C0u,0u,0x402E0u,0u};
        CHECK(kernel_guest_read_bytes(stream,after,sizeof(after)));
        CHECK(memcmp(after,packet,sizeof(packet))==0);
        CHECK(memcmp(after+36u,before+36u,28u)==0);
        uint32_t cursor,count,exclusive,cached[4],saved[4];
        CHECK(kernel_guest_read_u32(device,&cursor)&&cursor==stream+36u);
        CHECK(kernel_guest_read_u32(device+0x1C00u,&count)&&count==1u);
        CHECK(kernel_guest_read_u32(device+0x1C04u,&exclusive)&&exclusive==0u);
        CHECK(kernel_guest_read_bytes(device+0x1B80u,cached,sizeof(cached)));
        CHECK(memcmp(cached,rect,sizeof(rect))==0);
        CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
        CHECK(memcmp(saved,frame,sizeof(frame))==0);
        size_t n;const thunk_trace_entry *trace=thunk_trace_entries(&n);
        CHECK(n==1u&&trace[0].address==row.address&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==16u);
    }
    xdk_thunk_shutdown();d3d8_hle_shutdown();
    CHECK(guest_region_free(stream));CHECK(guest_region_free(stack));
    CHECK(guest_region_free(bias_region));CHECK(guest_region_free(device_region));
    printf("compiled scissor routes: %u checks passed\n",checks);return 0;
}
