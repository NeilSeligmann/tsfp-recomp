/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_indexed.h"
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");
    uint32_t args[6],constants[5];uint8_t state[0x30000],indices[0x10000],mapping[32],buffers[256];
    if(!input||!output||fread(args,sizeof(args),1,input)!=1||fread(state,sizeof(state),1,input)!=1||
       fread(indices,sizeof(indices),1,input)!=1||fread(mapping,sizeof(mapping),1,input)!=1||
       fread(constants,sizeof(constants),1,input)!=1||fread(buffers,sizeof(buffers),1,input)!=1)return 2;
    fclose(input);environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(args[0],args[1]);map_fixed(0x00A00000u,0x20000u);map_fixed(0x00549000u,0x1000u);
    map_fixed(0x004A1000u,0x1000u);
    memcpy(kernel_guest_at(0x3D0000u,sizeof(state)),state,sizeof(state));
    memcpy(kernel_guest_at(0xA00000u,sizeof(indices)),indices,sizeof(indices));
    memcpy(kernel_guest_at(0xA10000u,sizeof(buffers)),buffers,sizeof(buffers));
    memcpy(kernel_guest_at(0x5496A0u,sizeof(mapping)),mapping,sizeof(mapping));
    const uint32_t addresses[5]={0x475C78u,0x4A1BB4u,0x4A1BB0u,0x475CACu,0x475CD4u};
    for(unsigned i=0u;i<5u;i++)store(addresses[i],constants[i]);
    const uint32_t start=load(D3D8_DEVICE_BASE);
    const uint32_t result=d3d8_draw_indexed_vertices(args[2],args[3],args[4]);
    const uint32_t end=load(D3D8_DEVICE_BASE);
    if(fwrite(&result,4u,1,output)!=1||fwrite(kernel_guest_at(start,end-start),end-start,1,output)!=1||
       fwrite(kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state),1,output)!=1||
       fwrite(kernel_guest_at(0xA00000u,sizeof(indices)),sizeof(indices),1,output)!=1)return 2;
    fclose(output);environment_end();return 0;
}
