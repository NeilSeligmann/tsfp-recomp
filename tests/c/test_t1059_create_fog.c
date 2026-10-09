/* SPDX-License-Identifier: GPL-3.0-or-later
 * Reuse the real init environment, not its historical full-ring layout assertions. */
#define main t1059_inherited_init_main
#include "test_d3d8_init.c"
#undef main
static void test_create_device_emits_original_fog_colour_default(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    store(0x003E3EE8u,0xA1B2C3D4u);
    uint32_t hr=0xFFFFFFFFu,out=0u;
    run_create_device(0x140u,0u,&hr,&out);
    CHECK_EQ_U32(hr,0u);
    CHECK_EQ_U32(load(0x003E3EE8u),0u);
    const uint32_t begin=load(DEVICE+0x24u),end=load(DEVICE);
    uint32_t packets=0u;
    for(uint32_t cursor=begin;cursor+8u<=end;cursor+=4u){
        if(load(cursor)==0x000402A8u){packets++;CHECK_EQ_U32(load(cursor+4u),0u);}
    }
    CHECK_EQ_U32(packets,1u);
    environment_end();
}


int main(void)
{
    test_create_device_emits_original_fog_colour_default();
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    store(0x003E3EE8u,0xA1B2C3D4u);
    uint32_t hr=0u,out=0u;
    run_create_device(0x110u,0u,&hr,&out);
    CHECK_EQ_U32(hr,0x80004005u);
    CHECK_EQ_U32(load(0x003E3EE8u),0xA1B2C3D4u);
    environment_end();
    printf("T1059 native CreateDevice: %u checks, %u failures\n",(unsigned)checks,(unsigned)failures);
    return failures?1:0;
}
