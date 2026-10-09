/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_stream.h"
#include "dsound_hle.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");uint32_t options[3];uint8_t desc[24],format[20];
    if(input==NULL || output==NULL || fread(options,sizeof(options),1u,input)!=1u ||
       fread(desc,sizeof(desc),1u,input)!=1u || fread(format,sizeof(format),1u,input)!=1u)return 2;
    fclose(input);if(options[0]>1u || options[1]>3u || options[2]==0u || options[2]>10u)return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    /* Synthetic safe slots; table safety membership is distinct from the original
     * ordered vtable fingerprints already measured by T161. */
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t device=load(SCRATCH_DATA)-8u,desc_at=SCRATCH_DATA+256u,format_at=SCRATCH_DATA+320u;
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(current_irql);
    memcpy(desc+8u,&format_at,4u);
    for(uint32_t i=0u;i<options[2];i++) {
        uint32_t out=SCRATCH_DATA+128u+16u*i+options[1];uint8_t tails[16];memset(tails,0xAA,sizeof(tails));
        CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,format,sizeof(format)));
        CHECK(kernel_guest_write_bytes(out,tails,sizeof(tails)));uint32_t statuses[4]={0};
        statuses[0]=dsound_stream_create(desc_at,out);uint32_t stream=load(out);
        if(options[0]!=0u) {
            uint32_t parameters[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
            CHECK(kernel_guest_write_bytes(desc_at+20u,parameters,sizeof(parameters)));
            statuses[1]=dsound_stream_cache_i3dl2(stream,desc_at+20u,0u);
            statuses[2]=dsound_stream_cache_min_distance(stream,0x3F800000u,0u);
            statuses[3]=dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u);
        }
        dsound_stream_snapshot snapshot;CHECK(dsound_stream_get_snapshot(stream,&snapshot));
        uint32_t record[27]={0};memcpy(record,statuses,sizeof(statuses));memcpy(record+4u,snapshot.header,40u);
        record[7]-=device;record[14]=load(device+4u);record[15]=snapshot.cache_mask;
        memcpy(record+16u,snapshot.i3dl2,36u);record[25]=snapshot.min_distance_bits;record[26]=snapshot.curve_count;
        CHECK(kernel_guest_read_bytes(out,tails,sizeof(tails)));uint32_t normalized=0u;memcpy(tails,&normalized,4u);
        if(fwrite(record,sizeof(record),1u,output)!=1u || fwrite(tails,sizeof(tails),1u,output)!=1u)return 2;
    }
    CHECK(dsound_stream_reset_checked());uint32_t final=load(device+4u);
    if(fwrite(&final,sizeof(final),1u,output)!=1u)return 2;
    fclose(output);CHECK_EQ_U32(final,6u);CHECK(dsound_device_reset_checked());environment_end();return failures?1:0;
}
