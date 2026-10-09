/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1068 native counterpart: exact title-return frames for SetPosition 0x408556. */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_hle.h"

#define DATA_BASE 0x23000000u
static bool current_irql(uint8_t *out){*out=0u;return true;}
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}

static uint32_t invoke(uint32_t caller,uint32_t buffer,uint32_t x,uint32_t y,uint32_t z,uint32_t apply)
{
    const uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);store(stack+4u,buffer);
    store(stack+8u,x);store(stack+12u,y);store(stack+16u,z);store(stack+20u,apply);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+24u;
    return dsound_hle_call(0x00408556u,&frame);
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    dsound_buffer_set_enabled(false);CHECK_EQ_U32(dsound_buffer_register(),4u);
    CHECK(dsound_hle_entry(0x00408556u)->state!=DSOUND_ENTRY_IMPLEMENTED);
    dsound_hle_init();map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);map_fixed(DATA_BASE,0x10000u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    d3d8_hle_set_fatal(catching_fatal);dsound_buffer_set_fatal(catching_fatal);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    uint32_t device=load(SCRATCH_DATA)-8u,desc=SCRATCH_DATA+256u,format=SCRATCH_DATA+320u;
    uint8_t d[24]={0},f[20]={0};uint32_t dw[6]={24u,16u,0u,format,0u,0u};memcpy(d,dw,sizeof(dw));
    uint16_t fw[2]={0x69u,1u};memcpy(f,fw,sizeof(fw));uint32_t rate=44000u,average=24750u;
    memcpy(f+4u,&rate,4u);memcpy(f+8u,&average,4u);uint16_t rest[4]={36u,4u,2u,64u};memcpy(f+12u,&rest,sizeof(rest));
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(current_irql);
    CHECK_EQ_U32(dsound_buffer_register(),10u);
    CHECK(dsound_hle_entry(0x00408556u)->state==DSOUND_ENTRY_IMPLEMENTED);
    uint32_t out=0x5835F0u;CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,out,0u),0u);uint32_t buffer=load(out);
    uint32_t params=SCRATCH_DATA+400u,p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(params,p,sizeof(p)));
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA_BASE,1440u),0u);

    dsound_buffer_snapshot before,after;CHECK(dsound_buffer_get_snapshot(buffer,&before));
    uint32_t references=load(device+4u);const uint32_t callers[3]={0x26EF6u,0x27C79u,0x27D28u};
    const uint32_t values[3][3]={{0x3F800000u,0x40400000u,0x40800000u},
        {0x7FC12345u,0x7F800000u,0xFF800000u},{0x80000000u,0u,0xBF800000u}};
    for(unsigned i=0u;i<3u;i++) {
        CHECK_EQ_U32(invoke(callers[i],buffer,values[i][0],values[i][1],values[i][2],1u),0u);
        CHECK(dsound_buffer_get_snapshot(buffer,&after));
        CHECK_EQ_U32(after.position_bits[0],values[i][0]);CHECK_EQ_U32(after.position_bits[1],values[i][1]);
        CHECK_EQ_U32(after.position_bits[2],values[i][2]);CHECK_EQ_U32(after.position_sets,i+1u);
        CHECK_EQ_U32(after.cache_mask,7u);CHECK_EQ_U32(after.data_sets,1u);CHECK_EQ_U32(load(device+4u),references);
        CHECK(memcmp(after.header,before.header,sizeof(before.header))==0);
    }
    dsound_buffer_snapshot unchanged=after;
    RUN_EXPECTING_FATAL((void)invoke(0x27C78u,buffer,1u,2u,3u,1u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x27C79u,buffer,1u,2u,3u,0u));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x27C79u,buffer,1u,2u,3u,2u));CHECK(fatal_seen);
    uint8_t od[24]={0};uint32_t ow[6]={24u,0u,0u,format,0u,0u};memcpy(od,ow,sizeof(od));
    CHECK(kernel_guest_write_bytes(desc,od,sizeof(od)));uint32_t ordinary_out=0x5818E8u;
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,ordinary_out,0u),0u);uint32_t ordinary=load(ordinary_out);
    CHECK_EQ_U32(dsound_buffer_set_data(ordinary,DATA_BASE+0x1000u,1440u),0u);
    RUN_EXPECTING_FATAL((void)invoke(0x26EF6u,ordinary,1u,2u,3u,1u));CHECK(fatal_seen);
    uint8_t sd[24]={0};uint32_t sw[6]={24u,16u,0u,format,0u,0u};memcpy(sd,sw,sizeof(sd));
    CHECK(kernel_guest_write_bytes(desc,sd,sizeof(sd)));uint32_t fresh_out=0x5835F4u;
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc,fresh_out,0u),0u);uint32_t fresh=load(fresh_out);
    CHECK_EQ_U32(dsound_buffer_cache_i3dl2(fresh,params,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_min_distance(fresh,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_buffer_cache_rolloff(fresh,0x64BC98u,1u,0u),0u);
    dsound_buffer_snapshot fresh_before,fresh_after;CHECK(dsound_buffer_get_snapshot(fresh,&fresh_before));
    RUN_EXPECTING_FATAL((void)invoke(0x26EF6u,fresh,1u,2u,3u,1u));CHECK(fatal_seen);
    CHECK(dsound_buffer_get_snapshot(fresh,&fresh_after));
    CHECK(memcmp(&fresh_before,&fresh_after,sizeof(fresh_before))==0);
    store(0x4124A8u,1u);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x26EF6u,buffer,1u,2u,3u,1u),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x26EF6u,0u,1u,2u,3u,0u),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x27C79u,0xDEADBEEFu,1u,2u,3u,2u),0x80004005u));CHECK(!fatal_seen);
    dsound_buffer_set_enabled(false);
    RUN_EXPECTING_FATAL((void)invoke(0x26EF6u,buffer,1u,2u,3u,1u));CHECK(fatal_seen);
    dsound_buffer_set_enabled(true);store(0x4124A8u,0u);
    CHECK(dsound_buffer_get_snapshot(buffer,&after));CHECK(memcmp(&after,&unchanged,sizeof(after))==0);
    CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("CHECKS %d FAILURES %d\n",checks,failures);return failures?1:0;
}
