/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    uint32_t ready, calls, alignment;
    if (input == NULL || output == NULL || fread(&ready,4u,1u,input)!=1u ||
        fread(&calls,4u,1u,input)!=1u || fread(&alignment,4u,1u,input)!=1u) return 2;
    fclose(input);
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init(); dsound_device_reset();
    map_fixed(0x412000u,0x1000u); map_fixed(0x4A1000u,0x1000u);
    dsound_hle_set_codec_state((ready & 1u) ? DSOUND_CODEC_READY : DSOUND_CODEC_NOT_READY);
    const uint32_t out = SCRATCH_DATA + alignment;
    for (uint32_t i=0u; i<calls; i++) {
        if (i != 0u && (ready & 2u)) dsound_hle_set_codec_state((ready & 1u) ? DSOUND_CODEC_NOT_READY : DSOUND_CODEC_READY);
        memset(kernel_guest_at(out,16u),0xAAu,16u);
        const uint32_t result=dsound_device_create(0u,out,0u);
        const uint32_t address=load(0x412B30u);
        uint32_t words[11]; memcpy(words,kernel_guest_at(address,44u),44u);
        /* Normalize only owned self links/interface address; omitted nested words
         * are POLICY zero, not compared as original-allocation equivalence. */
        words[4]-=address; words[5]-=address;
        uint8_t bytes[16];memcpy(bytes,kernel_guest_at(out,16u),16u);
        uint32_t interface;memcpy(&interface,bytes,4u);
        if(result==0u){interface-=address;memcpy(bytes,&interface,4u);}
        if(fwrite(&result,4u,1u,output)!=1u || fwrite(words,44u,1u,output)!=1u ||
            fwrite(bytes,16u,1u,output)!=1u)return 2;
    }
    fclose(output);
    const uint32_t address=load(0x412B30u);
    CHECK(address!=0u);
    dsound_device_reset();
    CHECK_EQ_U32(load(0x412B30u),0u);
    CHECK_EQ_U32(guest_mem_heap_count(),0u);
    CHECK(dsound_hle_codec_state()==(((ready & 1u) != 0u) ^ ((ready & 2u) != 0u && calls > 1u) ? DSOUND_CODEC_READY:DSOUND_CODEC_NOT_READY));
    environment_end();
    return failures==0?0:1;
}
