/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent synthetic image/parser/bootstrap domain; no guest filesystem claim. */
#include "test_d3d8_support.h"
#include "host_options.h"
#include "mu_startup.h"
#include "mu_device.h"
#include "xinput_devices.h"
#include "xinput_hle.h"
#include "t1119_bootstrap.h"
static void begin(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);xinput_hle_init();xinput_devices_reset();mu_reset();
    map_fixed(0x46C000u,0x1000u);map_fixed(0x771000u,0x1000u);map_fixed(0x76E000u,0x1000u);
}
static void init_devices(void)
{
    const uint32_t decl[]={0x46C6E0u,8u,0x46C8A0u,4u,0x46C894u,4u,0x46C75Cu,4u};
    CHECK(kernel_guest_write_bytes(SCRATCH_DATA,decl,sizeof(decl)));
    CHECK_EQ_U32(xinput_devices_init_empty(4u,SCRATCH_DATA),0u);
}
static unsigned attached(void)
{
    unsigned mask=0;for(unsigned i=0;i<8;i++)if(mu_attached(i/2,i%2))mask|=1u<<i;return mask;
}
static int lifecycle(const char *directory)
{
    begin();char paths[8][4096],missing[4096],bad[4096],ro[4096],formatted[4096];
    for(unsigned i=0;i<8;i++)snprintf(paths[i],sizeof(paths[i]),"%s/unit %u.image",directory,i);
    snprintf(missing,sizeof(missing),"%s/missing",directory);snprintf(bad,sizeof(bad),"%s/bad",directory);
    snprintf(ro,sizeof(ro),"%s/read only",directory);snprintf(formatted,sizeof(formatted),"%s/own formatted",directory);
    char error[96];
    CHECK(mu_attach_blank(3,0,8u*1024u*1024u,true,0x1119u));CHECK_EQ_U32(mu_xmount(3,0,false,NULL),0u);
    uint8_t *preserved=mu_image(3,0,NULL);CHECK(preserved!=NULL);unsigned keep=1u<<6;
    const char *failures_at_third[]={missing,bad,ro};
    for(unsigned arm=0;arm<3;arm++) {
        const mu_startup_image list[]={{2,1,paths[5]},{0,1,paths[1]},{1,1,failures_at_third[arm]}};
        CHECK(!mu_startup_attach(list,3,error,sizeof(error)));CHECK(strstr(error,"MU image 2:")!=NULL);
        CHECK_EQ_U32(attached(),keep);CHECK(mu_image(3,0,NULL)==preserved);
        CHECK_EQ_U32(mu_mounted_mask(),keep);CHECK_EQ_U32(load(MU_MASK_ADDRESS),keep);
        CHECK_EQ_U32(load(0x46C6E0u),0u);
    }
    const mu_startup_image duplicate[]={{2,0,paths[4]},{2,0,paths[0]}};
    CHECK(!mu_startup_attach(duplicate,2,error,sizeof(error)));CHECK(strstr(error,"duplicate")!=NULL);
    CHECK_EQ_U32(attached(),keep);
    const mu_startup_image occupied[]={{2,0,paths[4]},{3,0,paths[6]}};
    CHECK(!mu_startup_attach(occupied,2,error,sizeof(error)));CHECK(strstr(error,"already attached")!=NULL);
    CHECK_EQ_U32(attached(),keep);
    struct {char before[4],text[4],after[4];} small;memset(&small,0x65,sizeof(small));
    CHECK(!mu_startup_attach(NULL,1,small.text,sizeof(small.text)));CHECK(small.text[3]==0);
    CHECK(memcmp(small.before,"eeee",4)==0 && memcmp(small.after,"eeee",4)==0);
    CHECK(!mu_startup_attach(NULL,1,NULL,0));CHECK(mu_startup_attach(NULL,0,NULL,0));
    mu_reset();xinput_devices_reset();
    const unsigned permutation[]={6,1,4,7,2,0,5,3};mu_startup_image images[8];
    for(unsigned i=0;i<8;i++){unsigned j=permutation[i];images[i]=(mu_startup_image){j/2,j%2,paths[j]};}
    CHECK(mu_startup_attach(images,8,error,sizeof(error)));CHECK(error[0]==0);CHECK_EQ_U32(attached(),255u);
    CHECK_EQ_U32(load(0x46C6E0u),0u);CHECK_EQ_U32(mu_mounted_mask(),0u);
    for(unsigned i=0;i<8;i++) {
        size_t bytes=0;uint8_t *image=mu_image(i/2,i%2,&bytes);CHECK(image!=NULL);
        CHECK(bytes==40960u+512u*i);CHECK(image[0]==0x31u+i && image[bytes-1]==0x31u+i);
        FILE *file=fopen(paths[i],"rb");CHECK(file!=NULL);uint8_t *disk=malloc(bytes);CHECK(disk!=NULL);
        CHECK(fread(disk,1,bytes,file)==bytes);CHECK(fclose(file)==0);CHECK(memcmp(image,disk,bytes)==0);free(disk);
    }
    init_devices();
    CHECK_EQ_U32(xinput_devices_get(0x46C6E0u),0xF000Fu);
    for(unsigned i=0;i<8;i++)CHECK_EQ_U32(mu_xmount(i/2,i%2,false,NULL),MU_ERROR_UNRECOGNIZED_VOLUME);
    CHECK_EQ_U32(mu_mounted_mask(),0u);mu_reset();xinput_devices_reset();
    /* Only this uniquely owned synthetic output is formatted. */
    size_t bytes=8u*1024u*1024u;uint8_t *disk=calloc(1,bytes);CHECK(disk!=NULL);
    CHECK_EQ_U32(fatx_format(disk,bytes,0x11191119u),FATX_OK);FILE *file=fopen(formatted,"wb");CHECK(file!=NULL);
    CHECK(fwrite(disk,1,bytes,file)==bytes);CHECK(fclose(file)==0);free(disk);
    const mu_startup_image one={1,0,formatted};
    const uint8_t payload[]={0,1,0xFE,0xFF,0x43,0x22,0x91,0x17,0,0x55,0xAA,0xE8,0x29};
    for(unsigned pass=0;pass<2;pass++) {
        CHECK(mu_startup_attach(&one,1,error,sizeof(error)));CHECK_EQ_U32(mu_xmount(1,0,false,NULL),0u);
        fatx_volume *volume=mu_volume(1,0);CHECK(volume!=NULL);
        if(pass==0)CHECK_EQ_U32(fatx_write_file(volume,"REVIEW.BIN",payload,sizeof(payload)),FATX_OK);
        else {uint8_t out[32]={0};size_t length=0;CHECK_EQ_U32(fatx_read_file(volume,"REVIEW.BIN",out,sizeof(out),&length),FATX_OK);
            CHECK(length==sizeof(payload) && memcmp(out,payload,sizeof(payload))==0);}
        CHECK_EQ_U32(mu_xunmount(1,0),0u);CHECK(mu_detach(1,0));
    }
    mu_reset();xinput_devices_reset();environment_end();
    printf("T1119 lifecycle %d checks %d failures\n",checks,failures);return failures?1:0;
}
int main(int argc,char **argv)
{
    if(argc<3)return 99;if(strcmp(argv[1],"lifecycle")==0)return lifecycle(argv[2]);
    options opts;bool valid=parse_options(argc-1,argv+1,&opts);
    if(!valid){puts("parser=refused");return 3;}
    printf("parser=accepted count=%u",opts.mu_image_count);
    for(unsigned i=0;i<opts.mu_image_count;i++)printf(" unit=%u/%u",opts.mu_images[i].port,opts.mu_images[i].slot);
    puts("");if(strcmp(argv[1],"parse")==0)return 0;
    begin();int status=t1119_bootstrap(opts);unsigned mask=attached();
    CHECK_EQ_U32(load(0x46C6E0u),0u);init_devices();unsigned presence=xinput_devices_get(0x46C6E0u);
    printf("boot=%d attached=%u presence=%u ownership=%u failures=%d\n",status,mask,presence,mu_mounted_mask(),failures);
    mu_reset();xinput_devices_reset();environment_end();return failures?1:status;
}
