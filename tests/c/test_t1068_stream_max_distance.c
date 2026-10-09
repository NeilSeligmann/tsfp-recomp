/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1068: finite native controls for the opt-in stream SetMaxDistance raw record. */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
static uint32_t invoke(uint32_t caller,const uint32_t args[3])
{
    const uint32_t stack=SCRATCH_DATA+0x800u;store(stack,caller);
    for(unsigned i=0u;i<3u;i++)store(stack+4u+4u*i,args[i]);
    kernel_call_frame frame={0};frame.stack_ptr=stack;frame.stack_limit=stack+16u;
    return dsound_hle_call(0x4085D9u,&frame);
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
    static const uint32_t bits[]={0u,0x3F800000u,0x40400000u,0x7FC12345u,0x7F800000u,0xFFFFFFFFu};
    const uint32_t stream=create(true);configure_spatial(stream);uint32_t before[10];
    for(unsigned i=0u;i<10u;i++)before[i]=load(stream+4u*i);
    dsound_stream_snapshot snapshot;CHECK(dsound_stream_get_snapshot(stream,&snapshot));
    CHECK_EQ_U32(snapshot.cache_mask,7u);CHECK(!snapshot.max_distance_seen);
    for(unsigned i=0u;i<sizeof(bits)/sizeof(bits[0]);i++) {
        const uint32_t args[3]={stream,bits[i],1u};
        CHECK_EQ_U32(invoke(i&1u?0x29D86u:0x29ADAu,args),0u);
        CHECK(dsound_stream_get_snapshot(stream,&snapshot));CHECK(snapshot.max_distance_seen);
        CHECK_EQ_U32(snapshot.max_distance_bits,bits[i]);
        CHECK_EQ_U32(snapshot.cache_mask,7u);
        for(unsigned j=0u;j<10u;j++)CHECK_EQ_U32(load(stream+4u*j),before[j]);
    }
    const uint32_t unchanged=snapshot.max_distance_bits;
    const uint32_t bad[3]={stream,0x3F800000u,0u};
    RUN_EXPECTING_FATAL((void)invoke(0x29ADAu,bad));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(stream,&snapshot));CHECK_EQ_U32(snapshot.max_distance_bits,unchanged);
    const uint32_t other[3]={stream,0u,1u};
    RUN_EXPECTING_FATAL((void)invoke(0x29ADDu,other));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(stream,&snapshot));CHECK_EQ_U32(snapshot.max_distance_bits,unchanged);
    CHECK(dsound_stream_reset_checked());
}
static void test_scope_and_policy_refusals(void)
{
    uint32_t stream=create(false);dsound_stream_snapshot before,after;
    CHECK(dsound_stream_get_snapshot(stream,&before));
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_max_distance(stream,0x40400000u,1u));CHECK(fatal_seen);
    CHECK(dsound_stream_get_snapshot(stream,&after));CHECK_EQ_U32(after.max_distance_seen,0u);
    CHECK_EQ_U32(after.cache_mask,before.cache_mask);
    CHECK(dsound_stream_reset_checked());
    stream=create(true);configure_spatial(stream);CHECK(dsound_stream_get_snapshot(stream,&before));
    store(0x4124A8u,1u);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_stream_cache_max_distance(stream,0x40400000u,1u),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_stream_cache_max_distance(0u,0xFFFFFFFFu,0u),0x80004005u));CHECK(!fatal_seen);
    RUN_EXPECTING_FATAL(CHECK_EQ_U32(dsound_stream_cache_max_distance(0xDEADBEEFu,0xFFFFFFFFu,2u),0x80004005u));CHECK(!fatal_seen);
    store(0x4124A8u,0u);CHECK(dsound_stream_get_snapshot(stream,&after));
    CHECK_EQ_U32(after.max_distance_seen,0u);CHECK_EQ_U32(after.cache_mask,before.cache_mask);
    dsound_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_max_distance(stream,0x40400000u,1u));CHECK(fatal_seen);
    dsound_stream_set_enabled(true);CHECK(dsound_stream_reset_checked());
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);dsound_stream_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    dsound_stream_set_enabled(true);CHECK(dsound_stream_register()>=9u);
    test_record_and_exact_callers();test_scope_and_policy_refusals();
    CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());environment_end();
    printf("T1068 stream max distance: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
