/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T601 native half of tests/test_dsound_buffer_frequency_oracle.py. Request: u32 spatial, u32 call
 * count, 24-byte descriptor, 20-byte format. The runner makes the passive buffer, applies the spatial
 * setters for flags 0x10, records SetBufferData, SetVolume(-10000) and Pause(0) (the measured order)
 * and then calls SetFrequency(22042) count times. Response per call (15 words): status, frequency,
 * frequency sets, device reference, 9 header words, cache mask. Refusals (other values, wrong order)
 * are asserted by tests/c/test_dsound_buffer.c. */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_hle.h"
#define DATA_BASE 0x23000000u
static bool current_irql(uint8_t *out){*out=0u;return true;}
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");uint32_t options[2];uint8_t desc[24],format[20];
    if(input==NULL || output==NULL || fread(options,sizeof(options),1u,input)!=1u ||
       fread(desc,sizeof(desc),1u,input)!=1u || fread(format,sizeof(format),1u,input)!=1u)return 2;
    if(options[0]>1u || options[1]==0u || options[1]>64u)return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);
    map_fixed(DATA_BASE,0x10000u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    uint32_t device=load(SCRATCH_DATA)-8u,desc_at=SCRATCH_DATA+256u,format_at=SCRATCH_DATA+320u;
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(current_irql);memcpy(desc+12u,&format_at,4u);
    uint32_t out=options[0]?0x5835F0u:0x5818E8u;
    CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,format,sizeof(format)));
    CHECK_EQ_U32(dsound_buffer_create(device+8u,desc_at,out,0u),0u);uint32_t buffer=load(out);
    if(options[0]) {
        uint32_t p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};CHECK(kernel_guest_write_bytes(desc_at+20u,p,sizeof(p)));
        CHECK_EQ_U32(dsound_buffer_cache_i3dl2(buffer,desc_at+20u,0u),0u);
        CHECK_EQ_U32(dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u),0u);
        CHECK_EQ_U32(dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u),0u);
    }
    CHECK_EQ_U32(dsound_buffer_set_data(buffer,DATA_BASE,1440u),0u);
    CHECK_EQ_U32(dsound_buffer_set_volume(buffer,-10000),0u);
    CHECK_EQ_U32(dsound_buffer_pause(buffer,0u),0u);
    for(uint32_t i=0u;i<options[1];i++) {
        uint32_t record[15]={0};dsound_buffer_snapshot snapshot;
        record[0]=dsound_buffer_set_frequency(buffer,22042u);
        CHECK(dsound_buffer_get_snapshot(buffer,&snapshot));
        record[1]=snapshot.frequency;record[2]=snapshot.frequency_sets;record[3]=load(device+4u);
        for(unsigned j=0u;j<9u;j++)record[4u+j]=load(snapshot.header_address+4u*j);
        record[13]=snapshot.cache_mask;record[14]=snapshot.data_sets;
        if(fwrite(record,sizeof(record),1u,output)!=1u)return 2;
    }
    CHECK(dsound_buffer_reset_checked());CHECK(dsound_device_reset_checked());fclose(output);environment_end();
    return failures?1:0;
}
