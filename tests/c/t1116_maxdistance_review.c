/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent finite policy controls. Synthetic owned startup state; no audio output. */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_hle.h"
#define DATA 0x25000000u
#define ENTRY 0x0040850Eu
static uint8_t irql;
static bool get_irql(uint8_t *out) {*out=irql;return true;}
int recomp_has_stop_boundary(uint32_t a)
{return a==0x408040u || a==0x406FA9u || a==0x406FF0u || a==0x406879u;}
static uint32_t call(uint32_t caller,uint32_t buffer,uint32_t bits,uint32_t apply)
{
    uint32_t sp=SCRATCH_DATA+0xA40u;
    store(sp,caller);store(sp+4u,buffer);store(sp+8u,bits);store(sp+12u,apply);
    kernel_call_frame frame={.stack_ptr=sp,.stack_limit=sp+16u};
    return dsound_hle_call(ENTRY,&frame);
}
static uint32_t make(uint32_t device,uint32_t index,unsigned stages,bool data)
{
    uint32_t desc=SCRATCH_DATA+0x200u,format=desc+0x40u;
    uint32_t dw[6]={24u,16u,0u,format,0u,0u};
    uint8_t fmt[20]={0};uint16_t kind[2]={0x69u,1u},rest[4]={36u,4u,2u,64u};
    uint32_t rate=44000u,avg=24750u;
    memcpy(fmt,kind,4u);memcpy(fmt+4u,&rate,4u);memcpy(fmt+8u,&avg,4u);memcpy(fmt+12u,rest,8u);
    CHECK(kernel_guest_write_bytes(desc,dw,sizeof(dw)));CHECK(kernel_guest_write_bytes(format,fmt,20u));
    uint32_t output=0x5835F0u+4u*index;
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,output,0u),0u);uint32_t b=load(output);
    uint32_t params=desc+0x80u,p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(params,p,sizeof(p)));
    if(stages>=1u)CHECK_EQ_U32(dsound_buffer_cache_i3dl2(b,params,0u),0u);
    if(stages>=2u)CHECK_EQ_U32(dsound_buffer_cache_min_distance(b,0x3F800000u,0u),0u);
    if(stages>=3u)CHECK_EQ_U32(dsound_buffer_cache_rolloff(b,0x64BC98u,1u,0u),0u);
    if(data)CHECK_EQ_U32(dsound_buffer_set_data(b,DATA+index*0x800u,2880u),0u);
    return b;
}
static void refused_unchanged(uint32_t b,uint32_t caller,uint32_t bits,uint32_t apply)
{
    dsound_buffer_snapshot before,after;CHECK(dsound_buffer_get_snapshot(b,&before));
    RUN_EXPECTING_FATAL((void)call(caller,b,bits,apply));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(b,&after));CHECK(memcmp(&before,&after,sizeof(before))==0);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x583000u,4096u);map_fixed(DATA,0x10000u);
    for(unsigned i=0;i<4;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_buffer_set_fatal(catching_fatal);dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);uint32_t d=load(SCRATCH_DATA)-8u;
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(get_irql);
    (void)dsound_buffer_register();CHECK(dsound_hle_entry(ENTRY)->state==DSOUND_ENTRY_IMPLEMENTED);
    uint32_t b=make(d,0u,3u,true);dsound_buffer_snapshot before,after;
    CHECK(dsound_buffer_get_snapshot(b,&before));uint32_t refs=load(d+4u);
    const uint32_t values[]={0u,0x80000000u,1u,0x807FFFFFu,0x7F812345u,0xFF812345u,
        0xFFCABCDEu,0xFF800000u,0x3EAAAAABu,0xC2480000u,0x7F7FFFFFu,0x7F7FFFFFu};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {
        CHECK_EQ_U32(call(0x280B5u,b,values[i],1u),0u);
        CHECK(dsound_buffer_get_snapshot(b,&after));
        dsound_buffer_snapshot expected=before;expected.max_distance_bits=values[i];expected.max_distance_sets=i+1u;
        CHECK(memcmp(&expected,&after,sizeof(expected))==0);CHECK_EQ_U32(load(d+4u),refs);
        CHECK_EQ_U32(load(SCRATCH_DATA+0xA40u),0x280B5u);
    }
    for(unsigned a=0;a<4;a++)if(a!=1u)refused_unchanged(b,0x280B5u,0x41012345u,a);
    refused_unchanged(b,0x280B6u,0x41012345u,1u);
    store(0x4124A8u,0x80000000u);refused_unchanged(b,0x280B5u,0x41012345u,1u);store(0x4124A8u,0u);
    irql=2u;refused_unchanged(b,0x280B5u,0x41012345u,1u);irql=0u;
    uint32_t original=load(before.header_address);store(before.header_address,original^4u);
    RUN_EXPECTING_FATAL((void)call(0x280B5u,b,0x41012345u,1u));CHECK(fatal_seen);
    store(before.header_address,original);CHECK(dsound_buffer_get_snapshot(b,&after));
    CHECK_EQ_U32(after.max_distance_sets,12u);CHECK_EQ_U32(after.max_distance_bits,values[11]);
    for(unsigned stage=0;stage<4;stage++) {
        uint32_t partial=make(d,stage+1u,stage,false);
        refused_unchanged(partial,0x280B5u,0x41012345u,1u);
    }
    CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("T1116 %d checks %d failures\n",checks,failures);return failures?1:0;
}
