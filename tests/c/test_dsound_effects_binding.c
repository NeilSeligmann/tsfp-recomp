/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_effects_binding.h"
#include "dsound_effects_metadata.h"
#include "dsound_hle.h"
static void owned_callback(uint32_t address,void *userdata,uint32_t *result)
{ CHECK_EQ_U32(address,load(0x412B30u));*(bool *)userdata=true;*result=123u; }
int main(void)
{
    /* The dsstdfx image's measured place in the retail XBE, pinned as literals
     * (section header: vaddr 0x7F78C0, 18608 bytes; the pytest side re-measures the
     * same numbers from the image). Every bind below drives the code through the
     * macros, so without this pin a mutated address would move test and code
     * together and the title's real bind at 0x7F78C0 would be the first failure. */
    CHECK_EQ_U32(DSOUND_EFFECTS_IMAGE_ADDRESS,0x007F78C0u);
    CHECK_EQ_U32(DSOUND_EFFECTS_IMAGE_BYTES,18608u);
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();dsound_device_reset();
    dsound_effects_binding_reset();dsound_effects_binding_set_fatal(catching_fatal);
    map_fixed(0x412000u,0x1000u);map_fixed(0x4A1000u,0x1000u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    const uint32_t out=SCRATCH_DATA,loc=out+32u;store(loc,3u);store(loc+4u,4u);
    CHECK_EQ_U32(dsound_device_create(0u,out,0u),0u);const uint32_t iface=load(out);
    bool called=false;uint32_t result=0xAAu;
    CHECK(dsound_device_with_owned_interface(iface,owned_callback,&called,&result));
    CHECK(called);CHECK_EQ_U32(result,123u);called=false;
    CHECK(!dsound_device_with_owned_interface(iface+1u,owned_callback,&called,&result));CHECK(!called);
    const uint32_t internal=iface-8u,saved=load(internal);
    store(internal,saved^1u);
    CHECK(!dsound_device_with_owned_interface(iface,owned_callback,&called,&result));CHECK(!called);
    store(internal,saved);
    for(unsigned i=0u;i<11u;i++) {
        const uint32_t old=load(internal+i*4u);store(internal+i*4u,old^1u);
        CHECK(!dsound_device_with_owned_interface(iface,owned_callback,&called,&result));CHECK(!called);
        store(internal+i*4u,old);
    }
    store(out,0xAAAAAAAAu);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);CHECK_EQ_U32(load(out),0xAAAAAAAAu);
    CHECK(strstr(fatal_text,"policy is disabled")!=NULL);
    /* The measured default-boot state (T72a): NOT_READY Create leaves [0x581988] zero and
     * the title passes a NULL interface. The refusal must carry the derived original
     * behavior (NULL-device map at 0x004079F2..F6, fault at 0x00406BCA), distinct from a
     * foreign nonzero interface which stays a generic ownership refusal. */
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(0u,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"NULL interface")!=NULL);
    CHECK(strstr(fatal_text,"0x000279D5")!=NULL);
    CHECK(strstr(fatal_text,"0x00406BCA")!=NULL);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface+4u,DSOUND_EFFECTS_IMAGE_ADDRESS,
        DSOUND_EFFECTS_IMAGE_BYTES,loc,out));CHECK(fatal_seen);
    CHECK(strstr(fatal_text,"ownership/header/generation")!=NULL);
    CHECK(strstr(fatal_text,"NULL interface")==NULL);
    dsound_effects_binding_set_enabled(true);
    /* Reacquiring after fatal proves both device/effects locks were released. */
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,0u,0u,loc,out));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface+1u,0u,0u,loc,out));CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_effects_binding_bind(iface,0u,0u,loc,loc));CHECK(fatal_seen);
    CHECK_EQ_U32(load(loc),3u);CHECK_EQ_U32(load(out),0xAAAAAAAAu);
    CHECK_EQ_U32(guest_mem_heap_count(),1u);CHECK_EQ_U32(dsound_effects_binding_register(),1u);
    kernel_call_frame frame={0};frame.stack_ptr=(SCRATCH_DATA+0x800u);frame.stack_limit=(SCRATCH_DATA+0x800u)+24u;
    store(frame.stack_ptr,0xBADu);
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x4079DBu,&frame));CHECK(fatal_seen);
    store(frame.stack_ptr,0x270AFu);store(frame.stack_ptr+4u,iface);
    store(frame.stack_ptr+8u,0u);store(frame.stack_ptr+12u,0u);
    store(frame.stack_ptr+16u,loc);store(frame.stack_ptr+20u,out);
    frame.stack_limit=frame.stack_ptr+20u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x4079DBu,&frame));CHECK(fatal_seen);
    frame.stack_limit=frame.stack_ptr+24u;
    RUN_EXPECTING_FATAL((void)dsound_hle_call(0x4079DBu,&frame));CHECK(fatal_seen);
    CHECK(!dsound_hle_dsp_ack_configured());CHECK_EQ_U32(dsound_hle_dsp_ack_count(),0u);
    dsound_effects_binding_reset();dsound_device_reset();CHECK_EQ_U32(guest_mem_heap_count(),0u);
    environment_end();printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
