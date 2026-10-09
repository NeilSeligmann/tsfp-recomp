/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "xgrph_hle.h"
#include "xgrph_shader_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { \
    fprintf(stderr,"shader query route failed at %d: %s\n",__LINE__,#v); abort(); } } while (0)
static uint32_t allocate(void)
{
    guest_region_request request={0};
    request.bytes=4096u; request.protect=PAGE_READWRITE; request.state=MEM_COMMIT;
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
    const uint32_t stack=allocate(), input=allocate();
    const uint32_t frame_va=stack+4096u-8u;
    const xdk_dispatch_entry row={0x3E9546u,"XGSUCode_GetVertexShaderLength",XDK_MODULE_XGRPH};
    const xgrph_surface_entry utility={row.address,row.name,1u};
    CHECK(xdk_thunk_init(&row,1u));
    CHECK(xdk_thunk_declare_abi(row.address,XDK_CC_STDCALL,1u,0u));
    CHECK(xgrph_hle_init(&utility,1u));
    xgrph_hle_set_fatal(fatal);
    CHECK(xgrph_shader_query_register()==1u);
    const uint32_t headers[]={0x00042078u,0u,0xFFFF0000u,0x8001FFFFu};
    for(unsigned route=0u;route<2u;route++) {
        recomp_func_t fn=route==0u?recomp_lookup(row.address):recomp_lookup_manual(row.address);
        CHECK(fn!=NULL);
        for(unsigned variant=0u;variant<5u;variant++) {
            /* Last valid case reads only the final two bytes of the page. */
            const uint32_t source=variant==4u?input+4092u:input+1u;
            const uint32_t header=headers[variant%4u];
            CHECK(kernel_guest_write_u32(source,header));
            const uint32_t frame[2]={0x21124u,source};
            CHECK(kernel_guest_write_bytes(frame_va,frame,sizeof(frame)));
            g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
            g_esp=frame_va;g_fs_base=0x12340000u;g_seh_ebp=0xFFFFFFFFu;
            thunk_trace_reset();
            if(sigsetjmp(*host_run_jmp(),1)==0) {
                host_run_arm();fn();
                CHECK(g_eax==header>>16u&&g_esp==frame_va+8u);
                CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u);
                CHECK(g_fs_base==0x12340000u&&g_seh_ebp==0xFFFFFFFFu);
            } else {CHECK(false);}
            host_run_disarm();
            uint32_t saved[2],saved_header;
            CHECK(kernel_guest_read_bytes(frame_va,saved,sizeof(saved)));
            CHECK(memcmp(frame,saved,sizeof(frame))==0);
            CHECK(kernel_guest_read_u32(source,&saved_header)&&saved_header==header);
            size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
            CHECK(count==1u&&trace[0].address==row.address&&trace[0].return_address==frame[0]);
            CHECK(trace[0].implemented&&trace[0].result_known&&trace[0].result==header>>16u);
        }
        const uint32_t frame[2]={0x21124u,UINT32_MAX};
        CHECK(kernel_guest_write_bytes(frame_va,frame,sizeof(frame)));
        g_eax=0xDEADBEEFu;g_esp=frame_va;thunk_trace_reset();
        if(sigsetjmp(*host_run_jmp(),1)==0) {host_run_arm();fn();CHECK(false);}
        else {
            CHECK(host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
            CHECK(host_run_result()->guest_address==row.address);
            CHECK(g_eax==0xDEADBEEFu&&g_esp==frame_va);
            size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
            CHECK(count==1u&&trace[0].implemented&&!trace[0].result_known);
        }
        host_run_disarm();
    }
    xdk_thunk_shutdown();xgrph_hle_shutdown();
    CHECK(guest_region_free(input));CHECK(guest_region_free(stack));
    printf("compiled shader query routes: %u checks passed\n",checks);
    return 0;
}
