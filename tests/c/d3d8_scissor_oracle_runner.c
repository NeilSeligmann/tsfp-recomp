/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_scissor.h"
int main(int argc, char **argv)
{
    if(argc!=3)return 2;
    FILE *in=fopen(argv[1],"rb"), *out=fopen(argv[2],"wb");
    uint8_t state[0x30000],rectangle[32],stream[64];uint32_t bias;
    if(!in || !out || fread(state,sizeof(state),1,in)!=1 ||
       fread(&bias,4,1,in)!=1 || fread(rectangle,32,1,in)!=1 ||
       fread(stream,64,1,in)!=1)return 3;
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0xD00000u,0x3000u);
    memcpy(kernel_guest_at(0x3D0000u,sizeof(state)),state,sizeof(state));
    store(0x475CD4u,bias);
    memcpy(kernel_guest_at(0xD02000u,32u),rectangle,32u);
    memcpy(kernel_guest_at(0xD00000u,64u),stream,64u);
    const uint32_t result=d3d8_set_scissors(1u,0u,0xD02000u);
    if(fwrite(&result,4,1,out)!=1 ||
       fwrite(kernel_guest_at(0x3D0000u,sizeof(state)),sizeof(state),1,out)!=1 ||
       fwrite(kernel_guest_at(0xD00000u,64u),64,1,out)!=1 ||
       fwrite(kernel_guest_at(0xD02000u,32u),32,1,out)!=1)return 4;
    fclose(in);fclose(out);environment_end();return 0;
}
