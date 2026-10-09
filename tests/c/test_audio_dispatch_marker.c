/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xdk_thunk.h"
#include "thunk_trace.h"
#include "recomp_abi.h"
#include <stdio.h>
TSFP_RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp;
/* Sorted measured DSOUND address facts, independent of generated tables. */
static const uint32_t required[]={
    0x00406A8Au, 0x00406AB6u, 0x004079DBu, 0x00407A02u, 0x00407A2Cu,
    0x00407A64u, 0x00407A80u, 0x00407AA4u, 0x00407ABCu, 0x00407AD8u,
    0x00407AF8u, 0x00407B14u, 0x00407B19u, 0x00407B1Eu, 0x00407B23u,
    0x00407B28u, 0x00407B40u, 0x0040805Bu, 0x004084F2u, 0x0040850Eu,
    0x00408532u, 0x00408556u, 0x0040858Bu, 0x004085AFu, 0x004085CFu,
    0x004085D4u, 0x004085D9u, 0x004085F1u, 0x00408609u, 0x00408632u,
    0x00408637u, 0x00408C0Du, 0x00408C2Du, 0x00409109u, 0x004093C8u,
    0x004093ECu, 0x00409410u, 0x0040945Au, 0x00409635u, 0x0040967Cu,
    0x0040AF54u
};
static unsigned checks,failures;
static size_t generated_abi_before;
#define CHECK(value) do {checks++;if(!(value)){failures++;printf("FAIL line%d\n",__LINE__);}} while(0)
#ifndef DSOUND_NO_DISPATCH_MARKER
static uint32_t missing;
static int refusal_value;
static size_t calls,unexpected;
int recomp_has_dispatch_boundary(uint32_t address);
int recomp_has_dispatch_boundary(uint32_t address)
{
    calls++;bool found=false;
    for(size_t i=0u;i<41u;i++)if(required[i]==address)found=true;
    if(!found)unexpected++;
    return address==missing?refusal_value:found?1:0;
}
#endif
static void check_no_state_changes(void)
{
    size_t count;(void)thunk_trace_entries(&count);
    CHECK(count==0u);CHECK(xdk_thunk_count()==0u);CHECK(xdk_thunk_abi_count()==0u);
    CHECK(xdk_thunk_generated_abi_count()==generated_abi_before);CHECK(xdk_thunk_call_count()==0u);
    CHECK(xdk_thunk_unknown_count()==0u);CHECK(xdk_thunk_unrouted_count()==0u);
    CHECK(xdk_thunk_abi_refused_count()==0u);CHECK(xdk_thunk_stop_override_count()==0u);
    CHECK(xdk_thunk_stop_override_call_count()==0u);
    for(unsigned m=0u;m<XDK_MODULE_COUNT;m++)CHECK(xdk_thunk_module_call_count((xdk_module)m)==0u);
    CHECK(g_eax==1u && g_ecx==2u && g_edx==3u && g_esp==4u);
}
int main(void)
{
    /* No guest-memory initialization or surface/handler registration is needed. */
    generated_abi_before=xdk_thunk_generated_abi_count();
    g_eax=1u;g_ecx=2u;g_edx=3u;g_esp=4u;
    CHECK(!xdk_thunk_dispatch_boundaries_ready(NULL,0u));
    CHECK(!xdk_thunk_dispatch_boundaries_ready(NULL,41u));
    CHECK(!xdk_thunk_dispatch_boundaries_ready(required,0u));
    uint32_t zero=0u;CHECK(!xdk_thunk_dispatch_boundaries_ready(&zero,1u));
#ifdef DSOUND_NO_DISPATCH_MARKER
    CHECK(!xdk_thunk_dispatch_boundaries_ready(required,41u));
    CHECK(!xdk_thunk_dispatch_boundaries_ready(required,1u));
#else
    CHECK(calls==0u);
    CHECK(xdk_thunk_dispatch_boundaries_ready(required,41u));CHECK(calls==41u);
    for(size_t i=0u;i<41u;i++) {
        missing=required[i];
        const int values[]={0,2,-1};
        for(size_t j=0u;j<3u;j++) {
            refusal_value=values[j];calls=0u;
            CHECK(!xdk_thunk_dispatch_boundaries_ready(required,41u));CHECK(calls==i+1u);
            check_no_state_changes();
        }
    }
    missing=0u;calls=0u;
    uint32_t reordered[]={required[40],required[0],required[20],required[0]};
    CHECK(xdk_thunk_dispatch_boundaries_ready(reordered,4u));CHECK(calls==4u);
    uint32_t unknown=0x004093C9u;CHECK(!xdk_thunk_dispatch_boundaries_ready(&unknown,1u));
    CHECK(unexpected==1u);
#endif
    check_no_state_changes();
    printf("audio dispatch marker: %u checks, %u failures\n",checks,failures);
    return failures!=0u;
}
