/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_hle.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
#ifndef DSOUND_BUFFER_NO_MARKER
int recomp_has_stop_boundary(uint32_t address)
{return address==0x408040u || address==0x406FA9u || address==0x406FF0u || address==0x406879u;}
#endif
int main(int argc,char **argv)
{
    if(argc==2 && strcmp(argv[1],"--check-marker")==0)return dsound_buffer_stops_ready()?1:0;
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");uint32_t options[2];uint8_t desc[24],format[20];
    if(input==NULL || output==NULL || fread(options,sizeof(options),1u,input)!=1u ||
       fread(desc,sizeof(desc),1u,input)!=1u || fread(format,sizeof(format),1u,input)!=1u)return 2;
    fclose(input);if(options[0]>1u || options[1]==0u || options[1]>40u)return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);map_fixed(0x581000u,4096u);map_fixed(0x583000u,4096u);
    for(unsigned i=0u;i<4u;i++)store(0x4A1CE0u+4u*i,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    uint32_t device=load(SCRATCH_DATA)-8u,desc_at=SCRATCH_DATA+256u,format_at=SCRATCH_DATA+320u;
    dsound_buffer_set_enabled(true);dsound_buffer_set_irql_provider(current_irql);memcpy(desc+12u,&format_at,4u);
    for(uint32_t i=0u;i<options[1];i++) {
        uint32_t out=(options[0]?0x5835F0u:0x5818E8u)+4u*i;uint8_t tails[16];memset(tails,0xAA,sizeof(tails));
        CHECK(kernel_guest_write_bytes(desc_at,desc,sizeof(desc)));CHECK(kernel_guest_write_bytes(format_at,format,sizeof(format)));
        CHECK(kernel_guest_write_bytes(out,tails,sizeof(tails)));uint32_t record[15]={0};
        record[0]=dsound_buffer_create(device+8u,desc_at,out,0u);uint32_t buffer=load(out);
        if(options[0]) {
            uint32_t p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};CHECK(kernel_guest_write_bytes(desc_at+20u,p,sizeof(p)));
            record[1]=dsound_buffer_cache_i3dl2(buffer,desc_at+20u,0u);
            record[2]=dsound_buffer_cache_min_distance(buffer,0x3F800000u,0u);
            record[3]=dsound_buffer_cache_rolloff(buffer,0x64BC98u,1u,0u);
        }
        dsound_buffer_snapshot snapshot;CHECK(dsound_buffer_get_snapshot(buffer,&snapshot));
        memcpy(record+4u,snapshot.header,36u);record[6]-=device;record[13]=load(device+4u);record[14]=buffer-snapshot.header_address;
        CHECK(kernel_guest_read_bytes(out,tails,sizeof(tails)));memset(tails,0,4u);
        if(fwrite(record,sizeof(record),1u,output)!=1u || fwrite(tails,sizeof(tails),1u,output)!=1u)return 2;
    }
    CHECK(dsound_buffer_reset_checked());uint32_t final=load(device+4u);if(fwrite(&final,sizeof(final),1u,output)!=1u)return 2;
    fclose(output);CHECK_EQ_U32(final,6u);CHECK(dsound_device_reset_checked());environment_end();return failures?1:0;
}
