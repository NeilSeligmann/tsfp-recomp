/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T681 native half of tests/test_dsound_completion_oracle.py. Request: u32 mode (0 stream, 1 buffer), u32 step
 * count, 20-byte format, then per step u32 op, u32 a, u32 b.
 * Stream (the stereo startup stream after its recorded start, SetFormat is not run so the format is the 44100 one):
 *   1 Process(size a, completion words present b), 2 GetStatus, 3 Pause(mode a), 4 GetInfo.
 * Buffer (ordinary buffer after the whole sound start): 1 Play(flags a), 2 GetStatus, 3 Stop (T733), 4 Pause(mode a), 5 SetFrequency(a).
 * The virtual clock stands still at 0, so nothing completes: exactly the state the original shows under Unicorn
 * (which has no hardware to drain a packet). Response per step (8 words): result, status or info word 0, info words
 * 1 to 3, the packet's completed word, the packet's status word, a spare. Timing is asserted by tests/c/test_dsound_completion.c. */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_completion.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#define DATA_BASE 0x23000000u
static bool current_irql(uint8_t *out){*out=0u;return true;}
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}
static uint64_t zero_clock(void){return 0u;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");uint32_t mode,steps;uint8_t format[20];
    if(input==NULL || output==NULL || fread(&mode,sizeof(mode),1u,input)!=1u || mode>1u ||
       fread(&steps,sizeof(steps),1u,input)!=1u || fread(format,sizeof(format),1u,input)!=1u || steps==0u || steps>64u)return 2;
    uint32_t plan[64][3];if(fread(plan,sizeof(plan[0]),steps,input)!=steps)return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x4B9000u,4096u);map_fixed(0x581000u,4096u);
    map_fixed(0x583000u,4096u);map_fixed(DATA_BASE,0x10000u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t device=load(SCRATCH_DATA)-8u,desc_at=SCRATCH_DATA+256u,format_at=SCRATCH_DATA+320u;
    const uint32_t out_at=SCRATCH_DATA+0x400u,words_at=SCRATCH_DATA+0x500u,packet_at=SCRATCH_DATA+0x600u;
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(current_irql);
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(current_irql);
    dsound_completion_set_enabled(true);dsound_completion_set_clock(zero_clock,49612u);
    dsound_stream_set_completion(true,dsound_completion_note_pause);
    dsound_buffer_set_completion(true,dsound_completion_buffer_started);
    dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
    uint32_t object;
    if(mode==0u) {
        const uint32_t desc[6]={0u,3u,format_at,0u,0u,0u};
        CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,format,20u));
        CHECK_EQ_U32(dsound_stream_create(desc_at,out_at),0u);object=load(out_at);
        CHECK_EQ_U32(dsound_stream_cache_volume(object,-10000),0u);CHECK_EQ_U32(dsound_stream_cache_pause(object,1u),0u);
        CHECK_EQ_U32(dsound_stream_cache_flush_ex(object,0u,0u,1u),0u);CHECK_EQ_U32(dsound_stream_cache_discontinuity(object),0u);
    } else {
        uint8_t d[24]={0};const uint32_t dw[6]={24u,0u,0u,format_at,0u,0u};memcpy(d,dw,sizeof(d));
        CHECK(kernel_guest_write_bytes(desc_at,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format_at,format,20u));
        CHECK_EQ_U32(dsound_buffer_create(device+8u,desc_at,0x5818E8u,0u),0u);object=load(0x5818E8u);
        CHECK_EQ_U32(dsound_buffer_set_data(object,DATA_BASE,1440u),0u);CHECK_EQ_U32(dsound_buffer_set_volume(object,-10000),0u);
        CHECK_EQ_U32(dsound_buffer_pause(object,0u),0u);CHECK_EQ_U32(dsound_buffer_set_frequency(object,22042u),0u);
    }
    for(uint32_t i=0u;i<steps;i++) {
        uint32_t record[8]={0};
        store(words_at,0xA5A5A5A5u);store(words_at+4u,0xA5A5A5A5u);store(words_at+8u,0xA5A5A5A5u);store(words_at+12u,0xA5A5A5A5u);
        if(mode==0u && plan[i][0]==1u) {
            const uint32_t packet[6]={DATA_BASE,plan[i][1],plan[i][2]?words_at+64u:0u,plan[i][2]?words_at+68u:0u,0u,0u};
            store(words_at+64u,0xDEADu);store(words_at+68u,0xBEEFu);
            CHECK(kernel_guest_write_bytes(packet_at,packet,sizeof(packet)));
            record[0]=dsound_completion_stream_process(object,packet_at,0u);
        } else if(mode==0u && plan[i][0]==2u) {
            record[0]=dsound_completion_stream_status(object,words_at);
        } else if(mode==0u && plan[i][0]==3u) {
            record[0]=dsound_stream_cache_pause(object,plan[i][1]);
        } else if(mode==0u && plan[i][0]==4u) {
            record[0]=dsound_completion_stream_info(object,words_at);
            for(unsigned j=1u;j<4u;j++)record[1u+j]=load(words_at+4u*j);
        } else if(mode==1u && plan[i][0]==1u) {
            record[0]=dsound_completion_buffer_play(object,plan[i][1]);
        } else if(mode==1u && plan[i][0]==2u) {
            record[0]=dsound_completion_buffer_status(object,words_at);
        } else if(mode==1u && plan[i][0]==3u) {
            record[0]=dsound_completion_buffer_stop(object);
        } else if(mode==1u && plan[i][0]==4u) {
            record[0]=dsound_buffer_pause(object,plan[i][1]);
        } else if(mode==1u && plan[i][0]==5u) {
            record[0]=dsound_buffer_set_frequency(object,plan[i][1]);
        } else return 2;
        record[1]=load(words_at);record[5]=load(words_at+64u);record[6]=load(words_at+68u);
        if(fwrite(record,sizeof(record),1u,output)!=1u)return 2;
    }
    CHECK(dsound_stream_reset_checked());CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());
    fclose(output);environment_end();
    return failures?1:0;
}
