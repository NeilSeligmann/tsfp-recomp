/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");
    uint32_t header[5];uint8_t state[0x30000],data[256],definition[240],usage[64],stream[4096];
    if(!in || !out || fread(header,sizeof(header),1,in)!=1 ||
       fread(state,sizeof(state),1,in)!=1 || fread(data,sizeof(data),1,in)!=1 ||
       fread(definition,sizeof(definition),1,in)!=1 || fread(usage,sizeof(usage),1,in)!=1 ||
       fread(stream,sizeof(stream),1,in)!=1)return 3;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(header[0],header[1]);map_fixed(0xA00000u,0x1000u);map_fixed(0x540000u,0x10000u);
    memcpy(kernel_guest_at(0x3D0000u,sizeof(state)),state,sizeof(state));
    memcpy(kernel_guest_at(0xA00000u,sizeof(data)),data,sizeof(data));
    memcpy(kernel_guest_at(0xA00100u,sizeof(definition)),definition,sizeof(definition));
    memcpy(kernel_guest_at(0x5496E0u,sizeof(usage)),usage,sizeof(usage));
    const uint32_t start=load(D3D8_DEVICE_BASE);
    memcpy(kernel_guest_at(start,sizeof(stream)),stream,sizeof(stream));
    const uint32_t result=d3d8_set_pixel_shader_constants(header[3],0xA00000u,header[4]);
    const uint32_t end=load(D3D8_DEVICE_BASE);
    if(fwrite(&result,4,1,out)!=1 || fwrite(&end,4,1,out)!=1 ||
       (end>start && fwrite(kernel_guest_at(start,end-start),end-start,1,out)!=1) ||
       fwrite(kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state),1,out)!=1 ||
       fwrite(kernel_guest_at(0xA00000u,sizeof(data)),sizeof(data),1,out)!=1 ||
       fwrite(kernel_guest_at(0xA00100u,sizeof(definition)),sizeof(definition),1,out)!=1 ||
       fwrite(kernel_guest_at(0x5496E0u,sizeof(usage)),sizeof(usage),1,out)!=1 ||
       fwrite(kernel_guest_at(start,sizeof(stream)),sizeof(stream),1,out)!=1)return 4;
    fclose(in);fclose(out);environment_end();return 0;
}
