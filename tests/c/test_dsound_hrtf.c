/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_hrtf.h"
#include "dsound_hle.h"
#include "kernel_critsec.h"
static uint8_t level;
static bool readable = true;
static bool provider(uint8_t *out) { *out = level; return readable; }
static uint32_t fake_enter(uint32_t cs) { (void)cs; return 0u; }
static uint32_t sentinel_leave(uint32_t cs)
{
    (void)kernel_critsec_leave_guest(cs);
    return 0xDEADBEEFu;
}
static void fatal(uint32_t address, const char *reason)
{
    (void)reason; CHECK_EQ_U32(address,0x406AB6u); longjmp(fatal_jump,1);
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x412000u,0x1000u);
    kernel_critsec_reset();
    dsound_hrtf_set_irql_provider(provider);
    dsound_hrtf_set_fatal(fatal);
    store(0x4124B4u,0x40001u); store(0x4124C4u,UINT32_MAX);
    dsound_hle_init(); CHECK_EQ_U32(dsound_hrtf_register(),1u);
    for (unsigned i = 0u; i < 256u; i++) {
        level = (uint8_t)i;
        store(0x412B58u,0x12345678u);store(0x412B88u,0x87654321u);
        for(unsigned j=0;j<11;j++)store(0x412B5Cu+j*4u,UINT32_MAX);
        CHECK_EQ_U32(dsound_hle_call(0x406AB6u,NULL),0u);
        CHECK_EQ_U32(load(0x412B5Cu),0x409D06u);
        CHECK_EQ_U32(load(0x412B80u),0x409F48u);
        CHECK_EQ_U32(load(0x412B84u),4u);
        CHECK_EQ_U32(load(0x412B58u),0x12345678u);
        CHECK_EQ_U32(load(0x412B88u),0x87654321u);
        CHECK_EQ_U32(load(0x4124C4u),UINT32_MAX);
    }
    level=0u;
    (void)kernel_critsec_enter_guest(0x4124B4u);
    dsound_hrtf_set_critical_calls(NULL,sentinel_leave);
    CHECK_EQ_U32(dsound_use_light_hrtf(),0xDEADBEEFu);
    kernel_critsec_info state; CHECK(kernel_critsec_state(0x4124B4u,&state));
    CHECK_EQ_U32(state.recursion,1u);CHECK_EQ_U32(state.owner,kernel_critsec_owner_token());
    (void)kernel_critsec_leave_guest(0x4124B4u);
    dsound_hrtf_set_critical_calls(NULL,NULL);
    for(unsigned trial=0;trial<3;trial++) {
        store(0x412B5Cu,0xCAFEBABEu);
        if(trial==0)dsound_hrtf_set_irql_provider(NULL);
        if(trial==1){dsound_hrtf_set_irql_provider(provider);readable=false;}
        if(trial==2){readable=true;dsound_hrtf_set_critical_calls(fake_enter,NULL);}
        if(setjmp(fatal_jump)==0){(void)dsound_use_light_hrtf();CHECK(false);}
        CHECK_EQ_U32(load(0x412B5Cu),0xCAFEBABEu);
    }
    dsound_hrtf_set_critical_calls(NULL,NULL);
    kernel_critsec_reset();
    map_fixed(0x500000u,0x1000u);
    for (unsigned i=0u;i<KERNEL_CRITSEC_MAX;i++) {
        const uint32_t cs=0x500000u+i*28u;
        store(cs,0x40001u);store(cs+16u,UINT32_MAX);
        (void)kernel_critsec_enter_guest(cs);(void)kernel_critsec_leave_guest(cs);
    }
    store(0x412B5Cu,0xCAFEBABEu);
    if(setjmp(fatal_jump)==0){(void)dsound_use_light_hrtf();CHECK(false);}
    CHECK_EQ_U32(load(0x412B5Cu),0xCAFEBABEu);
    CHECK_EQ_U32(kernel_critsec_table_full_count(),1u);
    kernel_critsec_reset();
    CHECK(guest_region_free(0x412000u));
    if(setjmp(fatal_jump)==0){(void)dsound_use_light_hrtf();CHECK(false);}
    CHECK_EQ_U32(kernel_critsec_tracked_count(),0u);
    environment_end();
    printf("%d checks, %d failures\n",checks,failures);return failures!=0;
}
