/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_shader.h"
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");
    uint32_t args[3];uint8_t state[0x30000],source[4096],stream[4096];
    if(!in || !out || fread(args,sizeof(args),1,in)!=1 ||
       fread(state,sizeof(state),1,in)!=1 || fread(source,sizeof(source),1,in)!=1 ||
       fread(stream,sizeof(stream),1,in)!=1)return 3;
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(0xD00000u,0x5000u);
    memcpy(kernel_guest_at(0x3D0000u,sizeof(state)),state,sizeof(state));
    memcpy(kernel_guest_at(0xD03000u,sizeof(source)),source,sizeof(source));
    memcpy(kernel_guest_at(0xD00000u,sizeof(stream)),stream,sizeof(stream));
    const uint32_t result=(args[0] == 0x003D5630u ? d3d8_set_vertex_shader(args[1],args[2]) : d3d8_set_vertex_shader_handle(args[1],args[2]));
    if(fwrite(&result,4,1,out)!=1 ||
       fwrite(kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state),1,out)!=1 ||
       fwrite(kernel_guest_at(0xD00000u,sizeof(stream)),sizeof(stream),1,out)!=1 ||
       fwrite(kernel_guest_at(0xD03000u,sizeof(source)),sizeof(source),1,out)!=1)return 4;
    fclose(in);fclose(out);environment_end();return 0;
}
