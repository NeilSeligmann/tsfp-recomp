/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xdk_thunk.h"
#include "thunk_trace.h"
#include "recomp_abi.h"
#include <stdio.h>
TSFP_RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp;
static const uint32_t expected[]={0x384859u,0x40686Cu,0x406879u,0x40688Au,
    0x40723Fu,0x407286u,0x4072D4u,0x40733Bu,0x407388u,0x4073D3u,
    0x407424u,0x407883u,0x40788Du,0x4093ADu};
static uint32_t refused;
static int refusal_value, calls, unexpected;
int recomp_has_stop_boundary(uint32_t address);
int recomp_has_stop_boundary(uint32_t address)
{
    calls++;
    bool found=false;
    for (size_t i=0u;i<14u;i++) if (expected[i]==address) found=true;
    if (!found) unexpected++;
    return address==refused ? refusal_value : found ? 1 : 0;
}
int main(void)
{
    int failures=0;
    g_eax=1u;g_ecx=2u;g_edx=3u;g_esp=4u;
    calls=0;
    if (!xdk_thunk_stream_stops_ready() || calls!=14) failures++;
    for (size_t i=0u;i<14u;i++) {
        refused=expected[i];
        for (int value=0;value<=2;value+=2) {
            refusal_value=value;calls=0;
            if (xdk_thunk_stream_stops_ready() || calls!=(int)i+1) failures++;
        }
    }
    refused=0u;calls=0;
    if (!xdk_thunk_stream_stops_ready() || calls!=14 || unexpected) failures++;
    size_t count; (void)thunk_trace_entries(&count);
    if (count || xdk_thunk_call_count() || xdk_thunk_stop_override_call_count() ||
        xdk_thunk_stop_override_count() || g_eax!=1u || g_ecx!=2u || g_edx!=3u || g_esp!=4u)
        failures++;
    printf("stream marker complete/28 incomplete states: %d failures\n",failures);
    return failures!=0;
}
