/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1173 native counterpart: the runtime sound start's apply=1 SetMinDistance (return 0x28390) and SetRolloffCurve (return 0x283AE)
 * on an owned spatial buffer with recorded data. */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_hle.h"

#define DATA_BASE 0x23000000u
#define MINIMUM_ENTRY 0x00408532u
#define ROLLOFF_ENTRY 0x0040858Bu
static bool current_irql(uint8_t *out){*out=0u;return true;}
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}

static uint32_t minimum(uint32_t caller,uint32_t buffer,uint32_t bits,uint32_t apply)
{
    const uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);store(stack+4u,buffer);
    store(stack+8u,bits);store(stack+12u,apply);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+16u;
    return dsound_hle_call(MINIMUM_ENTRY,&frame);
}
static uint32_t rolloff(uint32_t caller,uint32_t buffer,uint32_t curve,uint32_t count,uint32_t apply)
{
    const uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);store(stack+4u,buffer);
    store(stack+8u,curve);store(stack+12u,count);store(stack+16u,apply);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+20u;
    return dsound_hle_call(ROLLOFF_ENTRY,&frame);
}
static uint32_t make_spatial(uint32_t device,uint32_t desc,uint32_t params,uint32_t out,bool data)
{
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,out,0u),0u);uint32_t buffer=load(out);
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
    if(data)CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA_BASE,1440u),0u);
    return buffer;
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    dsound_buffer_set_enabled(false);CHECK_EQ_U32(dsound_buffer_register(),4u);
    dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x581000u,4096u);
    map_fixed(0x583000u,4096u);map_fixed(DATA_BASE,0x10000u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    d3d8_hle_set_fatal(catching_fatal);dsound_buffer_set_fatal(catching_fatal);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    uint32_t device=load(SCRATCH_DATA)-8u,desc=SCRATCH_DATA+256u,format=SCRATCH_DATA+320u;
    uint8_t d[24]={0},f[20]={0};uint32_t dw[6]={24u,16u,0u,format,0u,0u};memcpy(d,dw,sizeof(dw));
    uint16_t fw[2]={0x69u,1u};memcpy(f,fw,sizeof(fw));uint32_t rate=44000u,average=24750u;
    memcpy(f+4u,&rate,4u);memcpy(f+8u,&average,4u);uint16_t rest[4]={36u,4u,2u,64u};memcpy(f+12u,rest,sizeof(rest));
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(current_irql);
    CHECK_EQ_U32(dsound_buffer_register(),10u);
    uint32_t params=SCRATCH_DATA+400u,p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(params,p,sizeof(p)));
    uint32_t buffer=make_spatial(device,desc,params,0x5835F0u,true);

    dsound_buffer_snapshot before,after;CHECK(dsound_buffer_get_snapshot(buffer,&before));
    uint32_t references=load(device+4u);
    const uint32_t values[]={0x3F800000u,0x40400000u,0x7FC12345u,0x7F800000u};
    for(unsigned i=0u;i<sizeof(values)/sizeof(values[0]);i++) {
        CHECK_EQ_U32(minimum(0x00028390u,buffer,values[i],1u),0u);
        CHECK(dsound_buffer_get_snapshot(buffer,&after));CHECK_EQ_U32(after.min_distance_bits,values[i]);
        CHECK_EQ_U32(after.min_distance_sets,i+1u);CHECK_EQ_U32(after.cache_mask,7u);
        CHECK_EQ_U32(after.data_sets,1u);CHECK_EQ_U32(load(device+4u),references);
        CHECK(memcmp(after.header,before.header,sizeof(before.header))==0);
    }
    const uint32_t curves[]={0x4B9114u,0x4B9124u};
    for(unsigned i=0u;i<2u;i++) {
        CHECK_EQ_U32(rolloff(0x000283AEu,buffer,curves[i],4u,1u),0u);
        CHECK(dsound_buffer_get_snapshot(buffer,&after));CHECK_EQ_U32(after.curve_address,curves[i]);
        CHECK_EQ_U32(after.curve_count,4u);CHECK_EQ_U32(after.rolloff_sets,i+1u);CHECK_EQ_U32(after.cache_mask,7u);
        CHECK_EQ_U32(load(device+4u),references);CHECK(memcmp(after.header,before.header,sizeof(before.header))==0);
    }
    dsound_buffer_snapshot unchanged=after;
    /* wrong callers, caller and apply swapped, apply other than 0 or 1, a non zero global audio state */
    RUN_EXPECTING_FATAL((void)minimum(0x0002838Fu,buffer,0x3F800000u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)minimum(0x00027AF6u,buffer,0x3F800000u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)minimum(0x00028390u,buffer,0x3F800000u,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)minimum(0x00028390u,buffer,0x3F800000u,2u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283ADu,buffer,0x4B9114u,4u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x00027B06u,buffer,0x4B9114u,4u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283AEu,buffer,0x4B9114u,4u,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283AEu,buffer,0x4B9114u,4u,2u));CHECK(fatal_seen);
    store(0x4124A8u,1u);RUN_EXPECTING_FATAL((void)minimum(0x00028390u,buffer,0x40800000u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283AEu,buffer,0x4B9114u,4u,1u));CHECK(fatal_seen);store(0x4124A8u,0u);
    CHECK(dsound_buffer_get_snapshot(buffer,&after));CHECK(memcmp(&after,&unchanged,sizeof(after))==0);

    /* a spatial buffer without recorded data, an ordinary buffer and a fresh spatial buffer (no startup cache) are refused */
    uint32_t nodata=make_spatial(device,desc,params,0x5835F4u,false);
    dsound_buffer_snapshot nodata_before,nodata_after;CHECK(dsound_buffer_get_snapshot(nodata,&nodata_before));
    RUN_EXPECTING_FATAL((void)minimum(0x00028390u,nodata,0x3F800000u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283AEu,nodata,0x4B9114u,4u,1u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(nodata,&nodata_after));CHECK(memcmp(&nodata_before,&nodata_after,sizeof(nodata_before))==0);
    uint8_t od[24]={0};uint32_t ow[6]={24u,0u,0u,format,0u,0u};memcpy(od,ow,sizeof(ow));
    CHECK(kernel_guest_write_bytes(desc,od,sizeof(od)));uint32_t ordinary_out=0x5818E8u;
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,ordinary_out,0u),0u);uint32_t ordinary=load(ordinary_out);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,DATA_BASE+0x1000u,1440u),0u);
    dsound_buffer_snapshot ordinary_before,ordinary_after;CHECK(dsound_buffer_get_snapshot(ordinary,&ordinary_before));
    RUN_EXPECTING_FATAL((void)minimum(0x00028390u,ordinary,0x3F800000u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283AEu,ordinary,0x4B9114u,4u,1u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(ordinary,&ordinary_after));CHECK(memcmp(&ordinary_before,&ordinary_after,sizeof(ordinary_before))==0);
    memcpy(d,dw,sizeof(d));CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));
    uint32_t fresh_out=0x5835F8u;CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,fresh_out,0u),0u);uint32_t fresh=load(fresh_out);
    dsound_buffer_snapshot fresh_before,fresh_after;CHECK(dsound_buffer_get_snapshot(fresh,&fresh_before));
    RUN_EXPECTING_FATAL((void)minimum(0x00028390u,fresh,0x3F800000u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)rolloff(0x000283AEu,fresh,0x4B9114u,4u,1u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(fresh,&fresh_after));CHECK(memcmp(&fresh_before,&fresh_after,sizeof(fresh_before))==0);
    /* the startup callers still take their ordered apply=0 cache on the fresh buffer */
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(fresh,params,0u),0u);
    CHECK_EQ_U32(minimum(0x00027AF6u,fresh,0x3F800000u,0u),0u);
    CHECK_EQ_U32(rolloff(0x00027B06u,fresh,0x64BC98u,1u,0u),0u);
    CHECK(dsound_buffer_get_snapshot(fresh,&fresh_after));CHECK_EQ_U32(fresh_after.cache_mask,7u);
    CHECK_EQ_U32(fresh_after.min_distance_sets,0u);CHECK_EQ_U32(fresh_after.rolloff_sets,0u);
    CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("CHECKS %d FAILURES %d\n",checks,failures);return failures?1:0;
}
