/* Independent explicit MU-policy controls; no real USB or FATX reference claim. */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#define TYPE 0x46C6E0u
#define DECL SCRATCH_DATA
#define FIRST (SCRATCH_DATA + 0x260u)
#define SECOND (SCRATCH_DATA + 0x2a0u)
static const uint32_t decl[8]={TYPE,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u};
static uint32_t presence(unsigned index) { return 1u << (index/2u + 16u*(index%2u)); }
static void changes(uint32_t added, uint32_t removed)
{
    memset(kernel_guest_at(FIRST,16u),0x61,16u); memset(kernel_guest_at(SECOND,16u),0x72,16u);
    CHECK_EQ_U32(xinput_devices_changes(TYPE,FIRST,SECOND),(added|removed)!=0u);
    CHECK_EQ_U32(load(FIRST),added); CHECK_EQ_U32(load(SECOND),removed);
    CHECK_EQ_U32(load(FIRST+4u),0x61616161u); CHECK_EQ_U32(load(SECOND+4u),0x72727272u);
}
static void attach(unsigned index) { CHECK(mu_attach_blank(index/2u,index%2u,8u*1024u*1024u,true,0x11070000u+index)); }
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    xinput_hle_init(); xinput_devices_reset(); mu_reset();
    map_fixed(0x46C000u,0x1000u); map_fixed(0x771000u,0x1000u); map_fixed(0x76E000u,0x1000u);
    memcpy(kernel_guest_at(DECL,32u),decl,32u);
    attach(1u); attach(6u);
    CHECK_EQ_U32(load(TYPE),0u); CHECK_EQ_U32(load(TYPE+4u),0u);
    CHECK_EQ_U32(xinput_devices_init_empty(4u,DECL),0u);
    uint32_t cold=presence(1u)|presence(6u);
    CHECK_EQ_U32(load(TYPE),cold);
    CHECK_EQ_U32(xinput_devices_peek(TYPE,FIRST,SECOND),cold);
    CHECK_EQ_U32(load(FIRST),0u); CHECK_EQ_U32(load(SECOND),0u);
    CHECK_EQ_U32(load(TYPE+4u),cold);
    changes(cold,0u); changes(0u,0u);
    uint32_t rest=0u;
    for(unsigned i=0;i<8;i++) if(i!=1u && i!=6u) { attach(i); rest|=presence(i); }
    CHECK_EQ_U32(load(TYPE),0x000F000Fu);
    changes(rest,0u);
    CHECK_EQ_U32(load(0x46C75Cu),0u);
    for(unsigned i=0;i<8;i++) CHECK(mu_attached(i/2u,i%2u));
    /* Mount subset in a different order; mounted-mask bits must never replace presence. */
    const unsigned order[4]={7u,2u,5u,0u}; uint32_t mounted=0u;
    for(unsigned k=0;k<4;k++) {
        unsigned i=order[k]; char letter='?';
        CHECK_EQ_U32(mu_xmount(i/2u,i%2u,true,&letter),0u);
        CHECK_EQ_U32((unsigned char)letter,0x46u+i); mounted|=1u<<i;
        CHECK_EQ_U32(mu_mounted_mask(),mounted); CHECK_EQ_U32(load(MU_MASK_ADDRESS),mounted);
        CHECK_EQ_U32(load(TYPE),0x000F000Fu); changes(0u,0u);
        CHECK(!mu_detach(i/2u,i%2u));
    }
    CHECK_EQ_U32(mu_mounted_mask(),0xA5u);
    CHECK(!mu_attach_blank(0u,0u,FATX_MIN_IMAGE,false,0u));
    CHECK(!mu_attach_blank(4u,0u,FATX_MIN_IMAGE,false,0u));
    CHECK(!mu_attach_blank(0u,2u,FATX_MIN_IMAGE,false,0u));
    CHECK(!mu_attach_blank(1u,0u,1u,false,0u)); changes(0u,0u);
    /* Reconnect an unmounted slot; peek is nonconsuming and reports BOTH directions later. */
    CHECK(mu_detach(1u,1u)); attach(3u);
    CHECK_EQ_U32(xinput_devices_peek(TYPE,FIRST,SECOND),0x000F000Fu);
    CHECK_EQ_U32(load(FIRST),0x000F000Fu); CHECK_EQ_U32(load(SECOND),presence(3u));
    changes(presence(3u),presence(3u));
    CHECK(mu_detach(2u,0u));
    CHECK_EQ_U32(xinput_devices_get(TYPE),0x000F000Fu & ~presence(4u)); changes(0u,0u);
    attach(4u); changes(presence(4u),0u);
    /* Adapter reset preserves explicit attachment, while initialized tables seed a fresh insertion. */
    xinput_devices_reset(); memset(kernel_guest_at(TYPE,12u),0,12u);
    CHECK_EQ_U32(xinput_devices_init_empty(4u,DECL),0u); changes(0x000F000Fu,0u);
    CHECK_EQ_U32(mu_mounted_mask(),0xA5u); CHECK_EQ_U32(load(MU_MASK_ADDRESS),0xA5u);
    /* Global cleanup must clear both host ownership and its guest mirror, while announcing removals. */
    mu_reset();
    CHECK_EQ_U32(mu_mounted_mask(),0u);
    CHECK_EQ_U32(load(MU_MASK_ADDRESS),0u);
    CHECK_EQ_U32(load(TYPE),0u); changes(0u,0x000F000Fu);
    for(unsigned i=0;i<8;i++) CHECK(!mu_attached(i/2u,i%2u));
    xinput_devices_reset(); environment_end();
    printf("T1107 lifecycle: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
