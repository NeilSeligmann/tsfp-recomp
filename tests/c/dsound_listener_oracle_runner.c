/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_listener.h"
#include "dsound_hle.h"
static bool current_irql(uint8_t *out){*out=0u;return true;}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);uint32_t interface=load(SCRATCH_DATA);
    uint8_t before[44],after[44];CHECK(kernel_guest_read_bytes(interface-8u,before,44u));
    dsound_listener_set_enabled(true);dsound_listener_set_irql_provider(current_irql);
    uint32_t record[14]={0};
    record[0]=dsound_listener_cache_doppler(interface,0u,0u);
    record[1]=dsound_listener_cache_position(interface,0u,0u,0u,0u);
    record[2]=dsound_listener_cache_orientation(interface,0u,0u,0x3F800000u,0u,0x3F800000u,0u,0u);
    dsound_listener_snapshot s;CHECK(dsound_listener_get_snapshot(interface,&s));
    record[3]=s.cache_mask;record[4]=s.doppler_bits;memcpy(record+5u,s.position,12u);memcpy(record+8u,s.orientation,24u);
    CHECK(kernel_guest_read_bytes(interface-8u,after,44u));CHECK(memcmp(before,after,44u)==0);
    CHECK_EQ_U32(load(interface-4u),6u);
    FILE *output=fopen(argv[1],"wb");if(output==NULL)return 2;
    if(fwrite(record,sizeof(record),1u,output)!=1u)return 2;
    fclose(output);dsound_listener_reset();CHECK(dsound_device_reset_checked());environment_end();return failures?1:0;
}
