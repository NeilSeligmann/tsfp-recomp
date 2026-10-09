/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"
#include "host_runtime.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static void fatal(uint32_t address,const char *message)
{host_run_stop(HOST_STOP_UNIMPLEMENTED,address,0u,message);}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0xD00000u,0x5000u);const uint32_t stream=0xD00000u,source=0xD03000u,wrapper=source+512u;
    store(wrapper,1u);store(wrapper+4u,0u);store(wrapper+8u,source);
    const xdk_dispatch_entry rows[2]={{0x3D92E0u,"D3DDevice_SetPixelShader",XDK_MODULE_D3D8},
        {0x3D9320u,"D3DDevice_SetPixelShaderV",XDK_MODULE_D3D8}};
    const d3d8_surface_entry surface[2]={{rows[0].address,rows[0].name,1u},{rows[1].address,rows[1].name,1u}};
    CHECK(xdk_thunk_init(rows,2u));
    for(unsigned i=0u;i<2u;i++)CHECK(xdk_thunk_declare_abi(rows[i].address,XDK_CC_STDCALL,1u,0u));
    CHECK(d3d8_hle_init(surface,2u));d3d8_hle_set_fatal(fatal);CHECK_EQ_U32(d3d8_shader_register(),2u);
    guest_region_request request={.bytes=8192u,.alignment=4096u,.protect=PAGE_READWRITE,.state=MEM_COMMIT};
    nt_status status;const uint32_t allocation=guest_region_alloc(&request,&status);
    CHECK(allocation!=0u);const uint32_t stack=allocation+4096u-8u;
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_NONE)==0);
    for(unsigned route=0u;route<4u;route++) {
        memset(kernel_guest_at(0x3D0000u,0x30000u),0,0x30000u);
        store(0x3E3F58u,D3D8_DEVICE_BASE);store(D3D8_DEVICE_BASE,stream);store(D3D8_DEVICE_BASE+4u,stream+4096u);
        const uint32_t address=rows[route==2u?1u:0u].address;
        const uint32_t argument=route==3u?UINT32_MAX:(route<2u?source:wrapper);
        const uint32_t frame[2]={0x12345678u,argument};CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        uint8_t before[0x30000];CHECK(kernel_guest_read_bytes(0x3D0000u,before,sizeof(before)));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;g_esp=stack;g_fs_base=8u;
        thunk_trace_reset();recomp_func_t fn=route==0u||route==3u?recomp_lookup(address):recomp_lookup_manual(address);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();CHECK(route!=3u);CHECK(g_eax==stream+324u&&g_esp==stack+8u);
        } else {
            CHECK(route==3u&&host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
            CHECK(g_eax==1u&&g_esp==stack);
        }
        host_run_disarm();
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u&&g_fs_base==8u);
        uint32_t saved[2];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved))&&memcmp(saved,frame,sizeof(frame))==0);
        if(route==3u) {CHECK(memcmp(before,kernel_guest_at(0x3D0000u,sizeof(before)),sizeof(before))==0);}
        else {
            CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x784u),route<2u?D3D8_DEVICE_BASE+0x924u:wrapper);
            CHECK_EQ_U32(load(D3D8_DEVICE_BASE),stream+324u);
        }
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==address&&trace[0].result_known==(route!=3u));
    }
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(allocation));xdk_thunk_shutdown();environment_end();
    printf("pixel bind routes: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
