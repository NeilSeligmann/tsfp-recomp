/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_gpu.h"
#include "d3d8_callbacks.h"
#include "kernel_clock.h"
static unsigned observations,phase,registrations;
static bool last_completed;
static uint32_t old_callback,new_callback;
static void tap(unsigned ordinal,const uint32_t *args,unsigned argc,uint32_t result)
{
    (void)result;
    if(ordinal==145u){ CHECK(argc==3u);CHECK(args[0]==D3D8_DEVICE_BASE+D3D8_DEV_VBLANK_EVENT);phase=1u; }
    if(ordinal==159u){ CHECK(argc==5u);CHECK(phase==1u);phase=2u; }
}
static void observe(bool completed)
{
    CHECK(phase==2u); observations++;last_completed=completed;phase=3u;
    CHECK(kernel_clock_peek()>0u);
}
static uint32_t wait_failed(void *context){(void)context;return STATUS_UNSUCCESSFUL;}
static uint32_t wait_success(void *context){(void)context;return STATUS_SUCCESS;}
static uint32_t set_prior_one(void *context){(void)context;return 1u;}
static uint32_t stop_kernel(void *context)
{
    (void)context;d3d8_hle_fatal(0x003D3550u,"observer fixture kernel stop");
}
static void registration(uint32_t callback)
{
    registrations++;new_callback=callback;old_callback=load(D3D8_DEVICE_BASE+0x1DB8u);
}
static void reject_registration(uint32_t callback)
{
    registration(callback);d3d8_hle_fatal(0x003D3530u,"observer fixture registration rejection");
}
static void initialise(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_gpu_set_vblank_observer(NULL);d3d8_callbacks_set_vblank_observer(NULL);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE,0x100000u);store(D3D8_GLOBAL_KICKOFF_SIZE,0x10000u);
    CHECK(d3d8_gpu_create());
    store(D3D8_DEVICE_BASE+0x1DDCu,0x00400000u);kernel_clock_reset();
    observations=0u;phase=0u;registrations=0u;last_completed=false;
    d3d8_guest_set_kernel_tap(tap);
}
static void waits(void)
{
    initialise();d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(observations,0u);CHECK_EQ_U32(phase,2u);
    CHECK_EQ_U32(d3d8_gpu_vblank_count(),1u);
    CHECK(kernel_clock_peek()==KERNEL_CLOCK_FREQUENCY_HZ/60u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+D3D8_DEV_VBLANK_EVENT+4u),1u);
    d3d8_gpu_set_vblank_observer(observe);phase=0u;d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(observations,1u);CHECK(last_completed);CHECK_EQ_U32(phase,3u);
    CHECK_EQ_U32(d3d8_gpu_vblank_count(),2u);
    CHECK(kernel_hle_register(159u,wait_failed));phase=0u;d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(observations,2u);CHECK(!last_completed);
    CHECK(kernel_hle_register(159u,wait_success));CHECK(kernel_hle_register(145u,set_prior_one));
    phase=0u;d3d8_gpu_wait_vblank();CHECK_EQ_U32(observations,3u);CHECK(!last_completed);
    kernel_hle_init();phase=0u;d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(observations,4u);CHECK(!last_completed);
    CHECK_EQ_U32(d3d8_gpu_vblank_count(),5u); /* returned failures preserve routine effects */
    CHECK(kernel_register_all()>0u);CHECK(kernel_hle_register(159u,stop_kernel));phase=0u;
    RUN_EXPECTING_FATAL(d3d8_gpu_wait_vblank());CHECK(fatal_seen);
    CHECK_EQ_U32(observations,4u);CHECK_EQ_U32(phase,1u);
    CHECK(kernel_hle_register(145u,stop_kernel));phase=0u;
    RUN_EXPECTING_FATAL(d3d8_gpu_wait_vblank());CHECK(fatal_seen);
    CHECK_EQ_U32(observations,4u);CHECK_EQ_U32(phase,0u);
    CHECK(kernel_register_all()>0u);
    d3d8_gpu_reset();phase=0u;d3d8_gpu_wait_vblank();
    CHECK_EQ_U32(observations,5u);CHECK(last_completed); /* reset kept observer */
    d3d8_gpu_set_vblank_observer(NULL);d3d8_guest_set_kernel_tap(NULL);environment_end();
}
static void callback_registration(void)
{
    initialise();store(D3D8_DEVICE_BASE+0x1DB8u,0x11111111u);
    CHECK_EQ_U32(d3d8_set_vertical_blank_callback(0x22222222u),0x22222222u);
    CHECK_EQ_U32(registrations,0u);
    d3d8_callbacks_set_vblank_observer(registration);
    CHECK_EQ_U32(d3d8_set_vertical_blank_callback(0x33333333u),0x33333333u);
    CHECK_EQ_U32(old_callback,0x22222222u);CHECK_EQ_U32(new_callback,0x33333333u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DB8u),0x33333333u);
    d3d8_callbacks_set_vblank_observer(reject_registration);
    RUN_EXPECTING_FATAL((void)d3d8_set_vertical_blank_callback(0x44444444u));CHECK(fatal_seen);
    CHECK_EQ_U32(old_callback,0x33333333u);CHECK_EQ_U32(new_callback,0x44444444u);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DB8u),0x33333333u);
    d3d8_callbacks_set_vblank_observer(NULL);
    CHECK_EQ_U32(d3d8_set_vertical_blank_callback(0u),0u);
    CHECK_EQ_U32(registrations,2u);CHECK_EQ_U32(load(D3D8_DEVICE_BASE+0x1DB8u),0u);
    d3d8_guest_set_kernel_tap(NULL);environment_end();
}
int main(void)
{
    waits();callback_registration();printf("%d checks, %d failures\n",checks,failures);
    return failures!=0;
}
