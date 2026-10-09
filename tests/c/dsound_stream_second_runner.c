/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T602 native half of tests/test_dsound_stream_{set_format,second_*}_oracle.py. Request: u32 step count,
 * 20-byte format, then per step u32 op and u32 argument (1 SetFormat(format), 2 Pause(argument),
 * 3 Discontinuity, 4 SetVolume(argument)). The runner makes the stereo 44100 startup stream and records
 * the started state the boot reaches (volume -10000, Pause1, FlushEx(0,0,1), Discontinuity) before the
 * steps. Response per step (24 words): status, format sets, pause seen, pause mode, flush seen,
 * discontinuity seen, volume seen, volume, device reference, 10 header words, 5 format words.
 * Refusals (other formats, wrong scope or order) are asserted by tests/c/test_dsound_stream_second.c. */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");uint32_t steps;uint8_t format[20];
    if(input==NULL || output==NULL || fread(&steps,sizeof(steps),1u,input)!=1u ||
       fread(format,sizeof(format),1u,input)!=1u || steps==0u || steps>64u)return 2;
    uint32_t plan[64][2];if(fread(plan,sizeof(plan[0]),steps,input)!=steps)return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t device=load(SCRATCH_DATA)-8u,desc_at=SCRATCH_DATA+256u,format_at=SCRATCH_DATA+320u,input_at=SCRATCH_DATA+384u;
    const uint32_t desc[6]={0u,3u,format_at,0u,0u,0u};
    const uint8_t startup[20]={0x69,0,2,0,0x44,0xAC,0,0,0xCC,0xC1,0,0,72,0,4,0,2,0,64,0};
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(current_irql);
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,startup,sizeof(startup)));
    CHECK_EQ_U32(dsound_stream_create(desc_at,SCRATCH_DATA+128u),0u);const uint32_t stream=load(SCRATCH_DATA+128u);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream,-10000),0u);CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
    CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream,0u,0u,1u),0u);CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream),0u);
    CHECK(kernel_guest_write_bytes(input_at,format,sizeof(format)));
    for(uint32_t i=0u;i<steps;i++) {
        uint32_t record[24]={0};dsound_stream_snapshot snapshot;
        switch(plan[i][0]) {
        case 1u:record[0]=dsound_stream_cache_set_format(stream,input_at);break;
        case 2u:record[0]=dsound_stream_cache_pause(stream,plan[i][1]);break;
        case 3u:record[0]=dsound_stream_cache_discontinuity(stream);break;
        case 4u:record[0]=dsound_stream_cache_volume(stream,(int32_t)plan[i][1]);break;
        default:return 2;
        }
        CHECK(dsound_stream_get_snapshot(stream,&snapshot));
        record[1]=snapshot.format_sets;record[2]=snapshot.pause_seen;record[3]=snapshot.pause_mode;
        record[4]=snapshot.flush_seen;record[5]=snapshot.discontinuity_seen;record[6]=snapshot.volume_seen;
        record[7]=(uint32_t)snapshot.volume;record[8]=load(device+4u);
        for(unsigned j=0u;j<10u;j++)record[9u+j]=load(stream+4u*j);
        record[12]-=device;memcpy(record+19u,snapshot.format,20u);
        if(fwrite(record,sizeof(record),1u,output)!=1u)return 2;
    }
    CHECK(dsound_stream_reset_checked());CHECK(dsound_device_reset_checked());fclose(output);environment_end();
    return failures?1:0;
}
