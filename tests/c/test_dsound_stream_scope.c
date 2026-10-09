/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dsound_stream_scope.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static unsigned checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;fprintf(stderr,"line%d: %s\n",__LINE__,#x);}} while(0)
static void put(uint8_t *p,uint32_t value,unsigned bytes)
{for(unsigned i=0u;i<bytes;i++)p[i]=(uint8_t)(value>>(i*8u));}
static void seed(uint8_t desc[24],uint8_t format[20],bool spatial)
{
    memset(desc,0,24u);memset(format,0,20u);
    put(desc,spatial?16u:0u,4u);put(desc+4u,3u,4u);put(desc+8u,0x123400u,4u);
    const unsigned channels=spatial?1u:2u;put(format,0x69u,2u);put(format+2u,channels,2u);
    put(format+4u,44100u,4u);put(format+8u,(44100u*36u*channels)>>6u,4u);
    put(format+12u,36u*channels,2u);put(format+14u,4u,2u);put(format+16u,2u,2u);put(format+18u,64u,2u);
}
static void test_host_scope(void)
{
    uint8_t desc[24],format[20];dsound_stream_scope result,before;
    for(unsigned spatial=0u;spatial<2u;spatial++) {
        seed(desc,format,spatial!=0u);CHECK(dsound_stream_scope_validate(desc,24u,format,20u,&result));
        CHECK(result.channels==(spatial?1u:2u));CHECK(result.max_packets==3u);
        CHECK(memcmp(result.descriptor,desc,24u)==0);CHECK(memcmp(result.format,format,20u)==0);
        /* Each byte contributes to the measured guard except pointer identity,
         * which is intentionally supplied by the runtime rather than hardcoded. */
        for(unsigned region=0u;region<2u;region++) {
            uint8_t *data=region?format:desc;unsigned length=region?20u:24u;
            for(unsigned i=0u;i<length;i++) {
                if(region==0u && i>=8u && i<12u)continue;
                memset(&result,0xA5,sizeof(result));before=result;data[i]^=1u;
                CHECK(!dsound_stream_scope_validate(desc,24u,format,20u,&result));
                CHECK(memcmp(&result,&before,sizeof(result))==0);data[i]^=1u;
            }
        }
    }
    for(size_t bytes=0u;bytes<24u;bytes++) {
        memset(&result,0xA5,sizeof(result));before=result;
        CHECK(!dsound_stream_scope_validate(desc,bytes,format,20u,&result));CHECK(memcmp(&result,&before,sizeof(result))==0);
    }
    for(size_t bytes=0u;bytes<20u;bytes++)CHECK(!dsound_stream_scope_validate(desc,24u,format,bytes,&result));
    put(desc+8u,UINT32_MAX-3u,4u);
    CHECK(!dsound_stream_scope_validate(desc,24u,format,20u,&result));
    CHECK(!dsound_stream_scope_validate(desc,25u,format,20u,&result));
    CHECK(!dsound_stream_scope_validate(desc,24u,format,21u,&result));
    CHECK(!dsound_stream_scope_validate(NULL,24u,format,20u,&result));
    CHECK(!dsound_stream_scope_validate(desc,24u,NULL,20u,&result));
    CHECK(!dsound_stream_scope_validate(desc,24u,format,20u,NULL));
}
static void test_guest_scope(void)
{
    uint8_t *base=mmap(NULL,12288u,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(base!=MAP_FAILED);if(base==MAP_FAILED)return;
    uint32_t addr=(uint32_t)(uintptr_t)base;uint8_t desc[24],format[20];seed(desc,format,true);
    const uint32_t format_address=addr+4096u+1u;put(desc+8u,format_address,4u);
    memcpy(base+1u,desc,24u);memcpy(base+4097u,format,20u);
    dsound_stream_scope result,before;CHECK(dsound_stream_scope_snapshot(addr+1u,&result));
    CHECK(result.descriptor_address==addr+1u);CHECK(result.format_address==format_address);
    CHECK(memcmp(result.descriptor,desc,24u)==0);CHECK(memcmp(result.format,format,20u)==0);
    CHECK(memcmp(base+1u,desc,24u)==0);CHECK(memcmp(base+4097u,format,20u)==0);
    base[4097u]^=1u;CHECK(memcmp(result.format,format,20u)==0);base[4097u]^=1u;
    put(base+9u,addr+25u,4u);memcpy(base+25u,format,20u);
    CHECK(dsound_stream_scope_snapshot(addr+1u,&result)); /* adjacent, not overlapping */
    memcpy(base+1u,desc,24u);
    CHECK(mprotect(base,4096u,PROT_READ)==0);CHECK(mprotect(base+4096u,4096u,PROT_READ)==0);
    CHECK(dsound_stream_scope_snapshot(addr+1u,&result));
    CHECK(mprotect(base,8192u,PROT_READ|PROT_WRITE)==0);
    const uint32_t bad_pointers[]={0u,addr+1u,addr+24u,UINT32_MAX-3u,addr+8192u+4090u};
    for(unsigned i=0u;i<sizeof(bad_pointers)/sizeof(bad_pointers[0]);i++) {
        put(base+9u,bad_pointers[i],4u);memset(&result,0xA5,sizeof(result));before=result;
        CHECK(!dsound_stream_scope_snapshot(addr+1u,&result));CHECK(memcmp(&result,&before,sizeof(result))==0);
    }
    put(base+9u,format_address,4u);CHECK(mprotect(base+4096u,4096u,PROT_NONE)==0);
    memset(&result,0xA5,sizeof(result));before=result;
    CHECK(!dsound_stream_scope_snapshot(addr+1u,&result));CHECK(memcmp(&result,&before,sizeof(result))==0);
    CHECK(!dsound_stream_scope_snapshot(addr+4090u,&result));CHECK(memcmp(&result,&before,sizeof(result))==0);
    CHECK(mprotect(base+4096u,4096u,PROT_READ|PROT_WRITE)==0);
    memcpy(base+4072u,desc,24u);CHECK(dsound_stream_scope_snapshot(addr+4072u,&result));
    CHECK(!dsound_stream_scope_snapshot(UINT32_MAX-3u,&result));CHECK(!dsound_stream_scope_snapshot(0u,&result));
    CHECK(!dsound_stream_scope_snapshot(addr+1u,NULL));CHECK(munmap(base,12288u)==0);
    memset(&result,0xA5,sizeof(result));before=result;
    CHECK(!dsound_stream_scope_snapshot(addr+1u,&result));CHECK(memcmp(&result,&before,sizeof(result))==0);
}
int main(void)
{
    test_host_scope();test_guest_scope();printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
