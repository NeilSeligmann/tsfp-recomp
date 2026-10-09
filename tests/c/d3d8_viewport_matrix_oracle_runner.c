/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_viewport_matrix.h"
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");
    uint32_t args[4],constants[4];uint8_t state[0x30000];
    if(!in || !out || fread(args,sizeof(args),1,in)!=1 || fread(state,sizeof(state),1,in)!=1 || fread(constants,sizeof(constants),1,in)!=1)return 3;
    environment_begin(KERNEL_AV_PACK_HDTV);
    memcpy(kernel_guest_at(0x3D0000u,sizeof(state)),state,sizeof(state));
    const uint32_t addresses[]={0x475CCCu,0x475CD4u,0x4760C4u,0x475C78u};
    for(unsigned i=0;i<4;i++)store(addresses[i],constants[i]);
    uint32_t result=args[0]?d3d8_multiply_matrix(args[1],args[2],args[3]):d3d8_rebuild_viewport_matrix();
    fwrite(&result,4,1,out);fwrite(kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state),1,out);
    fclose(in);fclose(out);environment_end();return 0;
}
