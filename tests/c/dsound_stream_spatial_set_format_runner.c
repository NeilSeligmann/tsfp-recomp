/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1181 native half of tests/test_t1181_spatial_set_format.py. Request: u32 step count, 20-byte format, then per step a u32
 * (unused). The runner makes the mono flags 0x10 stream with its three spatial setters, records the started state the boot
 * reaches (volume -10000, Pause1, FlushEx(0,0,1), Discontinuity) and calls SetFormat(format) per step. Response per step
 * (12 words): result, format sets, 5 format words, scope flags, cache mask, public header changed, scope rate. */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");uint32_t steps;uint8_t format[20];
    if(input==NULL || output==NULL || fread(&steps,sizeof(steps),1u,input)!=1u || fread(format,sizeof(format),1u,input)!=1u ||
       steps==0u || steps>64u)return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x4B9000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t desc_at=SCRATCH_DATA+256u,format_at=SCRATCH_DATA+320u,input_at=SCRATCH_DATA+384u;
    const uint32_t desc[6]={16u,3u,format_at,0u,0u,0u};
    const uint8_t startup[20]={0x69,0,1,0,0x44,0xAC,0,0,0xE6,0x60,0,0,36,0,4,0,2,0,64,0};
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(current_irql);
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,startup,20u));
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+128u),0u);const uint32_t stream=load(SCRATCH_DATA+128u);
    const uint32_t params[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA+512u,params,sizeof(params)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,SCRATCH_DATA+512u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream,-10000),0u);CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream,0u,0u,1u),0u);CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream),0u);
    CHECK(kernel_guest_write_bytes(input_at,format,sizeof(format)));
    for(uint32_t i=0u;i<steps;i++) {
        uint32_t record[12]={0},before[10],after[10];dsound_stream_snapshot snapshot;
        CHECK(kernel_guest_read_bytes(stream,before,sizeof(before)));
        record[0]=dsound_stream_cache_set_format(stream,input_at);
        CHECK(dsound_stream_get_snapshot(stream,&snapshot));CHECK(kernel_guest_read_bytes(stream,after,sizeof(after)));
        record[1]=snapshot.format_sets;memcpy(record+2u,snapshot.format,20u);
        record[7]=snapshot.scope.flags;record[8]=snapshot.cache_mask;record[9]=memcmp(before,after,sizeof(before))!=0;
        record[10]=snapshot.scope.sample_rate;
        if(fwrite(record,sizeof(record),1u,output)!=1u)return 2;
    }
    CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());fclose(output);environment_end();
    return failures?1:0;
}
