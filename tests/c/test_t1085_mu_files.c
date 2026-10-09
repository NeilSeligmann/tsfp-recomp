/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "kernel_file.h"
#include "kernel_object.h"
#include "xinput_hle.h"
static uint32_t call(unsigned ordinal, const uint32_t *args, unsigned count)
{
 kernel_call_frame f; memset(&f,0,sizeof f);
 CHECK(kernel_frame_build(&f,0x200100u,0x100u,args,count));
 return kernel_hle_call(ordinal,&f);
}
static uint32_t open_file(const char *name, unsigned disposition, uint32_t access)
{
 uint8_t *b=kernel_guest_at(0x200400u,256u); memcpy(b,name,strlen(name)+1);
 uint8_t *str=kernel_guest_at(0x200300u,8u); uint16_t len=(uint16_t)strlen(name);
 memcpy(str,&len,2); len++;memcpy(str+2,&len,2);store(0x200304u,0x200400u);
 store(0x200320u,0);store(0x200324u,0x200300u);store(0x200328u,0x40);
 store(0x200200u,0xAABBCCDDu);
 if(disposition==99){uint32_t args[]={0x200200u,access,0x200320u,0x200220u,3,0x40};return call(202,args,6);}
 uint32_t args[]={0x200200u,access,0x200320u,0x200220u,0,0,3,disposition,0x40};
 return call(190,args,9);
}
static uint32_t io(unsigned ordinal,uint32_t handle,uint32_t count,uint64_t offset)
{
 memcpy(kernel_guest_at(0x200240u,8u),&offset,8);
 uint32_t args[]={handle,0,0,0,0x200220u,0x200500u,count,0x200240u};
 return call(ordinal,args,8);
}
int main(int argc,char **argv)
{
 CHECK(argc==2); if(argc!=2)return 1;
 environment_begin(KERNEL_AV_PACK_HDTV); xinput_hle_init(); kernel_file_reset();
 map_fixed(0x200000u,0x10000u);map_fixed(0x10000u,0x2000u);map_fixed(0x76E000u,0x1000u);
 store(0x10118u,0x11000u);store(0x11008u,0x45410066u);store(MU_TITLE_DRIVE_ADDRESS,0);
 mu_reset();CHECK(mu_enable_guest_filesystem(true));
 CHECK(mu_attach_blank(0,0,8u*1024u*1024u,true,1));
 size_t size;uint8_t *image=mu_image(0,0,&size);FILE *f=fopen(argv[1],"wb");CHECK(f!=NULL);
 if(f!=NULL){CHECK(fwrite(image,1,size,f)==size);CHECK(fclose(f)==0);}
 CHECK(mu_detach(0,0));CHECK(mu_attach_file(0,0,argv[1],0));
 store(0x10118u,0xFFFFfffCu);char denied=0x7f;
 CHECK_EQ_U32(mu_xmount(0,0,true,&denied),0x57u);CHECK(denied==0);
 CHECK_EQ_U32(mu_mounted_mask(),0);CHECK(kernel_file_symlink_target("\\??\\F:")==NULL);
 store(0x10118u,0x11000u);
 CHECK_EQ_U32(mu_register(),2);
 uint32_t mount_args[]={0,0,0x200260u};kernel_call_frame mount_frame;
 CHECK(kernel_frame_build(&mount_frame,0x200100u,0x100u,mount_args,3));
 CHECK_EQ_U32(xinput_hle_call(MU_ENTRY_MOUNT,&mount_frame),0);
 char drive=(char)*(uint8_t *)kernel_guest_at(0x200260u,1);CHECK(drive=='F');
 const char *target=kernel_file_symlink_target("\\??\\F:");CHECK(target!=NULL && strcmp(target,"\\Device\\MU_7\\45410066")==0);
 CHECK_EQ_U32(open_file("\\??\\F:\\STATE.DAT",2,0x12019F),0);uint32_t h=load(0x200200u);
 memcpy(kernel_guest_at(0x200500u,16),"persistent-bytes",16);
 CHECK_EQ_U32(io(236,h,16,3),0);CHECK_EQ_U32(load(0x200224u),16);
 memset(kernel_guest_at(0x200500u,32),0xA5,32);
 CHECK_EQ_U32(io(219,h,32,0),0);CHECK_EQ_U32(load(0x200224u),19);
 CHECK(memcmp(kernel_guest_at(0x200500u,19),"\0\0\0persistent-bytes",19)==0);
 CHECK_EQ_U32(io(219,h,1,19),0xC0000011u);
 uint32_t flush_args[]={h,0x200220u};CHECK_EQ_U32(call(198,flush_args,2),0);
 f=fopen(argv[1],"rb");CHECK(f!=NULL);uint8_t *persisted=malloc(size);CHECK(persisted!=NULL);
 if(f && persisted){CHECK(fread(persisted,1,size,f)==size);CHECK(fclose(f)==0);fatx_volume disk;
 CHECK(fatx_open(&disk,persisted,size)==FATX_OK);uint32_t got;uint8_t bytes[32];
 CHECK(fatx_read_range(&disk,"45410066/STATE.DAT",0,bytes,32,&got)==FATX_OK && got==19);
 CHECK(memcmp(bytes,"\0\0\0persistent-bytes",19)==0);}free(persisted);

 CHECK_EQ_U32(open_file("\\??\\F:\\missing\\child",2,0x12019F),0xC000003Au);
 CHECK_EQ_U32(open_file("\\??\\F:\\STATE.DAT",2,0x12019F),0xC0000035u);
 CHECK_EQ_U32(open_file("\\??\\F:\\STATE.DAT",1,0x120089),0);uint32_t ro=load(0x200200u);
 CHECK_EQ_U32(io(236,ro,1,0),0xC0000022u);
 CHECK(!mu_enable_guest_filesystem(false));
 CHECK_EQ_U32(mu_xunmount(0,0),0);CHECK(kernel_file_symlink_target("\\??\\F:")==NULL);
 CHECK(!kernel_file_open_info(h,&(kernel_file_open){0}));CHECK(io(219,h,1,0)!=0);
 CHECK(mu_detach(0,0));CHECK(mu_attach_file(0,0,argv[1],0));CHECK_EQ_U32(mu_xmount(0,0,true,&drive),0);
 CHECK(io(219,h,1,0)!=0); /* reused device/volume never revives the old handle */
 CHECK_EQ_U32(open_file("\\??\\F:\\STATE.DAT",99,0x120089),0);uint32_t reopened=load(0x200200u);
 memset(kernel_guest_at(0x200500u,32),0xA5,32);CHECK_EQ_U32(io(219,reopened,32,0),0);
 CHECK(memcmp(kernel_guest_at(0x200500u,19),"\0\0\0persistent-bytes",19)==0);
 CHECK(mu_attach_blank(1,0,8u*1024u*1024u,true,2));
 CHECK_EQ_U32(mu_xmount(1,0,true,&drive),0);CHECK(drive=='H');
 target=kernel_file_symlink_target("\\??\\H:");CHECK(target && strcmp(target,"\\Device\\MU_6\\45410066")==0);
 CHECK_EQ_U32(mu_xunmount(0,0),0);CHECK_EQ_U32(mu_xmount(0,0,true,&drive),0);
 target=kernel_file_symlink_target("\\??\\F:");CHECK(target && strcmp(target,"\\Device\\MU_7\\45410066")==0);
 mu_reset();CHECK_EQ_U32(load(MU_MASK_ADDRESS),0);CHECK(!kernel_file_open_info(reopened,&(kernel_file_open){0}));
 CHECK(mu_enable_guest_filesystem(false)); environment_end();
 printf("MU guest files: %d checks, %d failures\n",checks,failures);return failures!=0;
}
