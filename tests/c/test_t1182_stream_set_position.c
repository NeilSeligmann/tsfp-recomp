/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1182: native controls for the passive IDirectSoundStream_SetPosition (0x408609) raw record. */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
static uint32_t invoke(uint32_t caller,const uint32_t args[5])
{
    const uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);
    for(unsigned i=0u;i<5u;i++)store(stack+4u+4u*i,args[i]);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+24u;
    return dsound_hle_call(0x408609u,&frame);
}
static uint32_t create(bool spatial)
{
    const uint32_t output=SCRATCH_DATA+128u,desc=SCRATCH_DATA+256u,format=SCRATCH_DATA+320u;
    const uint16_t channels=spatial?1u:2u,block=(uint16_t)(36u*channels);
    const uint32_t average=(44100u*(uint32_t)block)>>6u;
    const uint32_t d[6]={spatial?16u:0u,3u,format,0u,0u,0u};
    uint8_t f[20]={0x69,0,0,0,0x44,0xAC,0,0,0,0,0,0,0,0,4,0,2,0,64,0};
    memcpy(f+2u,&channels,sizeof(channels));memcpy(f+8u,&average,sizeof(average));
    memcpy(f+12u,&block,sizeof(block));
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
    CHECK_EQ_U32(dsound_stream_create(desc,output),0u);return load(output);
}
static void configure_spatial(uint32_t stream)
{
    const uint32_t params=SCRATCH_DATA+400u;
    const uint32_t values[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(params,values,sizeof(values)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,params,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);
}
static void test_record_and_exact_callers(void)
{
    static const uint32_t xyz[][3]={{0u,0u,0x3D4CCCCDu},{0x3F800000u,0x40000000u,0x40400000u},{0x7FC12345u,0x7F800000u,0xFFFFFFFFu},
                                    {0x80000000u,0u,0xBF800000u}};
    static const uint32_t callers[]={0x29ACAu,0x29E2Bu,0x29FADu,0x29E2Bu};
    const uint32_t stream=create(true);configure_spatial(stream);uint32_t before[10];
    for(unsigned i=0u;i<10u;i++)before[i]=load(stream+4u*i);
    dsound_stream_snapshot snapshot;CHECK(dsound_stream_get_snapshot(stream,&snapshot));
    CHECK_EQ_U32(snapshot.cache_mask,7u);CHECK_EQ_U32(snapshot.position_sets,0u);
    for(unsigned i=0u;i<sizeof(xyz)/sizeof(xyz[0]);i++) {
        const uint32_t args[5]={stream,xyz[i][0],xyz[i][1],xyz[i][2],1u};
        CHECK_EQ_U32(invoke(callers[i],args),0u);
        CHECK(dsound_stream_get_snapshot(stream,&snapshot));
        CHECK_EQ_U32(snapshot.position_sets,i+1u);
        CHECK_EQ_U32(snapshot.position_bits[0],xyz[i][0]);CHECK_EQ_U32(snapshot.position_bits[1],xyz[i][1]);
        CHECK_EQ_U32(snapshot.position_bits[2],xyz[i][2]);CHECK_EQ_U32(snapshot.cache_mask,7u);
        for(unsigned j=0u;j<10u;j++)CHECK_EQ_U32(load(stream+4u*j),before[j]);
    }
    dsound_stream_snapshot unchanged=snapshot;
    const uint32_t bad_apply[5]={stream,1u,2u,3u,0u},bad_apply2[5]={stream,1u,2u,3u,2u},ok[5]={stream,1u,2u,3u,1u};
    RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,bad_apply));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,bad_apply2));CHECK(fatal_seen);
    const uint32_t wrong[]={0x29AC9u,0x29ACBu,0x29E2Au,0x29E2Cu,0x29FACu,0x29FAEu,0u};
    for(unsigned i=0u;i<sizeof(wrong)/sizeof(wrong[0]);i++){RUN_EXPECTING_FATAL((void)invoke(wrong[i],ok));CHECK(fatal_seen);}
    store(0x4124A8u,1u);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x29ACAu,ok),0x80004005u));CHECK(!fatal_seen);
    const uint32_t null_args[5]={0u,1u,2u,3u,0u},bad_args[5]={0xDEADBEEFu,1u,2u,3u,2u};
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x29ACAu,null_args),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(invoke(0x29E2Bu,bad_args),0x80004005u));CHECK(!fatal_seen);
    dsound_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,ok));CHECK(fatal_seen);
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(NULL);
    RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,ok));CHECK(fatal_seen);
    dsound_stream_set_irql_provider(current_irql);store(0x4124A8u,0u);
    CHECK(dsound_stream_get_snapshot(stream,&snapshot));CHECK(memcmp(&snapshot,&unchanged,sizeof(snapshot))==0);
    CHECK(dsound_stream_reset_checked());
}
static void test_scope_and_policy_refusals(void)
{
    const uint32_t args[5]={0u,0x3F800000u,0x40000000u,0x40400000u,1u};
    uint32_t ordinary=create(false);dsound_stream_snapshot before,after;
    CHECK(dsound_stream_get_snapshot(ordinary,&before));
    uint32_t a[5]={ordinary,args[1],args[2],args[3],1u};
    RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,a));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(ordinary,&after));CHECK_EQ_U32(after.position_sets,0u);
    CHECK(dsound_stream_reset_checked());
    /* a spatial stream without the three startup setters is refused, so is an unowned address */
    uint32_t partial=create(true);CHECK(dsound_stream_get_snapshot(partial,&before));
    a[0]=partial;RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,a));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(partial,&after));CHECK_EQ_U32(after.position_sets,0u);
    a[0]=0x1000u;RUN_EXPECTING_FATAL((void)invoke(0x29ACAu,a));CHECK(fatal_seen);
    CHECK(dsound_stream_reset_checked());
    uint32_t spatial=create(true);configure_spatial(spatial);a[0]=spatial;
    dsound_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_stream_set_position(spatial,1u,2u,3u,1u));CHECK(fatal_seen);
    dsound_stream_set_enabled(true);CHECK(dsound_stream_get_snapshot(spatial,&after));CHECK_EQ_U32(after.position_sets,0u);
    CHECK_EQ_U32(dsound_stream_set_position(spatial,1u,2u,3u,1u),0u);
    CHECK(dsound_stream_get_snapshot(spatial,&after));CHECK_EQ_U32(after.position_sets,1u);
    CHECK(dsound_stream_reset_checked());
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);dsound_stream_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    dsound_stream_set_enabled(false);CHECK_EQ_U32(dsound_stream_register(),7u);
    CHECK(dsound_hle_entry(0x408609u)->state!=DSOUND_ENTRY_IMPLEMENTED);
    dsound_stream_set_enabled(true);CHECK_EQ_U32(dsound_stream_register(),10u);
    CHECK(dsound_hle_entry(0x408609u)->state==DSOUND_ENTRY_IMPLEMENTED);
    test_record_and_exact_callers();test_scope_and_policy_refusals();
    CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("T1182 stream SetPosition: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
