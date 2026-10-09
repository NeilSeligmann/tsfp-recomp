/* SPDX-License-Identifier: GPL-3.0-or-later */
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#ifdef TSFP_HAVE_BUFFER_STOP_ROUTES
#include "recomp_funcs.h"
#endif
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include <stdio.h>
#include <string.h>
static int failures, checks;
#define CHECK(value) do { checks++; if (!(value)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#value); failures++; } } while(0)
static int quiet(const char *format, ...) { (void)format; return 0; }
static const uint32_t addresses[]={0x00384859u,0x0040686Cu,0x00406879u,0x0040688Au,
    0x0040723Fu,0x00407286u,0x004072D4u,0x0040733Bu,0x00407388u,0x004073D3u,
    0x00407424u,0x00407883u,0x0040788Du,0x004093ADu
#ifdef TSFP_HAVE_BUFFER_STOP_ROUTES
    ,0x00406FA9u,0x00406FF0u,0x00408040u
#endif
};
static void registers(uint32_t out[8])
{
    out[0]=g_eax; out[1]=g_ecx; out[2]=g_edx; out[3]=g_ebx;
    out[4]=g_esp; out[5]=g_ebp; out[6]=g_esi; out[7]=g_edi;
}
int main(void)
{
    guest_region_request request={0}; request.bytes=4096u;
    request.protect=PAGE_READWRITE; request.state=MEM_COMMIT;
    nt_status status; uint32_t stack=guest_region_alloc(&request,&status);
    CHECK(stack!=0u); if (!stack) return 1;
    xdk_thunk_shutdown(); xdk_thunk_set_log(quiet);
    CHECK(xdk_thunk_stream_stops_ready());
#ifdef TSFP_HAVE_AUDIO_DISPATCH_MARKER
    const uint32_t audio[]={0x00409635u,0x00406AB6u,0x004079DBu,0x0040967Cu,
        0x004093C8u,0x004093ECu,0x00409410u,0x0040945Au};
    CHECK(xdk_thunk_dispatch_boundaries_ready(audio,sizeof(audio)/sizeof(audio[0])));
    const uint32_t extra=0x00406879u;
    CHECK(!xdk_thunk_dispatch_boundaries_ready(&extra,1u));
#endif
    CHECK(xdk_thunk_stop_override_count()==0u);
    const char *reason="Unrecovered stream or shared reference method; original child state is unavailable";
    for (unsigned policy=0u; policy<2u; policy++) {
        xdk_thunk_set_stop_on_missing(policy==0u);
        for (size_t index=0u; index<sizeof(addresses)/sizeof(addresses[0]); index++) {
            for (unsigned route=0u; route<(index<14u ? 3u : 4u); route++) {
                uint32_t address=addresses[index];
                const char *route_reason=index<14u ? reason :
                    "Unrecovered buffer lifetime method; original child state is unavailable";
                recomp_func_t fn=recomp_lookup(address);
                CHECK(fn!=NULL); CHECK(recomp_lookup_manual(address)==NULL);
                if (!fn) continue;
                uint8_t before[128],after[128];
                memset(before,0xA5,sizeof(before));
                uint32_t caller=0x000279D5u;
                memcpy(before,&caller,sizeof(caller));
                CHECK(kernel_guest_write_bytes(stack,before,sizeof(before)));
                g_eax=0xDEADBEEFu; g_ecx=0x11223344u; g_edx=0x55667788u;
                g_ebx=0x90ABCDEFu; g_esp=stack; g_ebp=0xAABBCCDDu;
                g_esi=0x01020304u; g_edi=0x05060708u;
                uint32_t original[8],actual[8]; registers(original);
                thunk_trace_reset();
                bool stopped=false;
                if (sigsetjmp(*host_run_jmp(),1)==0) {
                    host_run_arm();
                    if (route==0u) fn();
                    else if (route==1u) RECOMP_ICALL_SAFE_AT(address,stack+100u,caller);
                    else if (route==2u) RECOMP_ITAIL(address);
#ifdef TSFP_HAVE_BUFFER_STOP_ROUTES
                    else if (address==0x00406FA9u) sub_00406FA9();
                    else if (address==0x00406FF0u) sub_00406FF0();
                    else sub_00408040();
#endif
                } else stopped=true;
                host_run_disarm();
                CHECK(stopped);
                const host_stop *result=host_run_result();
                CHECK(result->reason==HOST_STOP_XDK_UNIMPLEMENTED);
                CHECK(result->guest_address==address && result->ordinal==0u);
                CHECK(strcmp(result->detail,route_reason)==0);
                registers(actual); CHECK(memcmp(original,actual,sizeof(original))==0);
                CHECK(kernel_guest_read_bytes(stack,after,sizeof(after)));
                CHECK(memcmp(before,after,sizeof(before))==0);
                size_t count; const thunk_trace_entry *trace=thunk_trace_entries(&count);
                CHECK(count==1u);
                if (count==1u) {
                    CHECK(trace[0].kind==THUNK_KIND_XDK && trace[0].address==address);
                    CHECK(trace[0].return_address==caller);
                    CHECK(!trace[0].implemented && !trace[0].result_known);
                }
                CHECK(xdk_thunk_stop_override_count()==0u);
            }
        }
    }
    CHECK(xdk_thunk_stop_override_call_count()==84u+8u*(sizeof(addresses)/sizeof(addresses[0])-14u));
    CHECK(guest_region_free(stack));
    printf("stream stop routes: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
