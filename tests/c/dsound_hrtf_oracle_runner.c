/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_hrtf.h"
#include "kernel_critsec.h"
static uint8_t level;
static uint32_t leave_result;
static bool provider(uint8_t *out) { *out = level; return true; }
static uint32_t leave(uint32_t cs) { (void)kernel_critsec_leave_guest(cs); return leave_result; }
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");
    uint32_t args[2];uint8_t state[0x5000];
    if(!input||!output||fread(args,sizeof(args),1,input)!=1||fread(state,sizeof(state),1,input)!=1)return 2;
    fclose(input);level=(uint8_t)args[0];leave_result=args[1];
    environment_begin(KERNEL_AV_PACK_HDTV);map_fixed(0x410000u,sizeof(state));
    kernel_critsec_reset();memcpy(kernel_guest_at(0x410000u,sizeof(state)),state,sizeof(state));
    dsound_hrtf_set_irql_provider(provider);dsound_hrtf_set_critical_calls(NULL,leave);
    uint32_t result=dsound_use_light_hrtf();
    if(fwrite(&result,4u,1,output)!=1||fwrite(kernel_guest_at(0x410000u,sizeof(state)),sizeof(state),1,output)!=1)return 2;
    fclose(output);kernel_critsec_reset();environment_end();return 0;
}
