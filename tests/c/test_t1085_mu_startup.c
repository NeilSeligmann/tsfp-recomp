/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "mu_startup.h"
#include "mu_device.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    const char *good=argv[1], *bad=argv[2], *missing=argv[3], *readonly=argv[4];
    environment_begin(KERNEL_AV_PACK_HDTV); xinput_hle_init(); xinput_devices_reset(); mu_reset();
    map_fixed(0x46C000u,0x1000u); map_fixed(0x771000u,0x1000u); map_fixed(0x76E000u,0x1000u);
    char error[160];
    CHECK(mu_startup_attach(NULL,0u,error,sizeof(error))); CHECK(error[0]=='\0');
    CHECK(!mu_startup_attach(NULL,1u,error,sizeof(error))); CHECK(error[0]!='\0');
    mu_startup_image list[9];
    for(unsigned i=0;i<9u;i++) list[i]=(mu_startup_image){i/2u,i%2u,good};
    CHECK(!mu_startup_attach(list,9u,error,sizeof(error)));
    const mu_startup_image invalid[]={{4u,0u,good},{0u,2u,good},{0u,0u,NULL},{0u,0u,""}};
    for(unsigned i=0;i<4u;i++) CHECK(!mu_startup_attach(&invalid[i],1u,error,sizeof(error)));
    list[0]=(mu_startup_image){1u,1u,good}; list[1]=list[0];
    CHECK(!mu_startup_attach(list,2u,error,sizeof(error))); CHECK(!mu_attached(1u,1u));
    CHECK(strstr(error,"duplicate") != NULL);
    CHECK(mu_attach_blank(3u,0u,FATX_MIN_IMAGE,false,0u));
    list[1]=(mu_startup_image){3u,0u,good};
    CHECK(!mu_startup_attach(list,2u,error,sizeof(error))); CHECK(!mu_attached(1u,1u)); CHECK(mu_attached(3u,0u));
    /* Refusal on a later image removes only attachments from this transaction. */
    const char *failures_at_second[]={bad,missing,readonly};
    for(unsigned i=0;i<3u;i++) {
        list[1]=(mu_startup_image){0u,1u,failures_at_second[i]};
        CHECK(!mu_startup_attach(list,2u,error,sizeof(error))); CHECK(error[0]!='\0');
        CHECK(!mu_attached(1u,1u)); CHECK(!mu_attached(0u,1u)); CHECK(mu_attached(3u,0u));
        CHECK_EQ_U32(load(0x46C6E0u),0u); CHECK_EQ_U32(mu_mounted_mask(),0u);
    }
    mu_reset();
    for(unsigned i=0;i<8u;i++) list[i]=(mu_startup_image){i/2u,i%2u,good};
    CHECK(mu_startup_attach(list,8u,error,sizeof(error))); CHECK_EQ_U32(load(0x46C6E0u),0u);
    const uint32_t decl[8]={0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u};
    memcpy(kernel_guest_at(SCRATCH_DATA,32u),decl,32u);
    CHECK_EQ_U32(xinput_devices_init_empty(4u,SCRATCH_DATA),0u); CHECK_EQ_U32(xinput_devices_get(0x46C6E0u),0xF000Fu);
    for(unsigned i=0;i<8u;i++) CHECK(mu_attached(i/2u,i%2u));
    /* Attachment accepts physical image geometry; mount separately validates FATX. */
    CHECK_EQ_U32(mu_xmount(1u,1u,false,NULL),MU_ERROR_UNRECOGNIZED_VOLUME);
    CHECK_EQ_U32(mu_mounted_mask(),0u); CHECK_EQ_U32(xinput_devices_get(0x46C6E0u),0xF000Fu);
    mu_reset(); xinput_devices_reset();
    /* Own explicit model-formatted image: startup attaches, guest mount owns it,
     * root writeback survives unmount/detach/reopen. Never format a supplied file. */
    const size_t bytes = 8u*1024u*1024u;
    uint8_t *formatted = calloc(1u,bytes); CHECK(formatted != NULL);
    CHECK_EQ_U32(fatx_format(formatted,bytes,0x1085u),FATX_OK);
    FILE *file = fopen(argv[5],"wb"); CHECK(file != NULL);
    CHECK(fwrite(formatted,1u,bytes,file)==bytes); CHECK(fclose(file)==0); free(formatted);
    const mu_startup_image own={2u,0u,argv[5]};
    for(unsigned reopen=0u;reopen<2u;reopen++) {
        CHECK(mu_startup_attach(&own,1u,error,sizeof(error)));
        CHECK_EQ_U32(mu_xmount(2u,0u,false,NULL),0u);
        fatx_volume *volume=mu_volume(2u,0u); CHECK(volume != NULL);
        if(reopen==0u) CHECK_EQ_U32(fatx_write_file(volume,"STATE.DAT","explicit MU",11u),FATX_OK);
        else {
            char restored[16]={0}; uint32_t length=0u;
            CHECK_EQ_U32(fatx_read_file(volume,"STATE.DAT",restored,sizeof(restored),&length),FATX_OK);
            CHECK(length==11u && memcmp(restored,"explicit MU",11u)==0);
        }
        CHECK_EQ_U32(mu_xunmount(2u,0u),0u); CHECK(mu_detach(2u,0u));
    }
    mu_reset(); xinput_devices_reset(); environment_end();
    printf("MU startup: %d checks, %d failures\n",checks,failures); return failures!=0;
}
