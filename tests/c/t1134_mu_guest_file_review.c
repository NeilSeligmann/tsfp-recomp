/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent FABRICATED images; real published XAPI/kernel dispatch. */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "kernel_file.h"
#include "xinput_hle.h"
static uint32_t invoke(unsigned ordinal,const uint32_t *args,unsigned n)
{
 kernel_call_frame frame={0};CHECK(kernel_frame_build(&frame,0x220100,0x180,args,n));
 return kernel_hle_call(ordinal,&frame);
}
static uint32_t file(const char *path,bool create,uint32_t access)
{
 uint16_t len=(uint16_t)strlen(path),cap=len+1;
 memcpy(kernel_guest_at(0x220900,256),path,cap);
 memcpy(kernel_guest_at(0x220300,2),&len,2);memcpy(kernel_guest_at(0x220302,2),&cap,2);
 store(0x220304,0x220900);store(0x220320,0);store(0x220324,0x220300);store(0x220328,0x40);
 store(0x220200,0xBAADC0DE);
 if(create){uint32_t a[]={0x220200,access,0x220320,0x220220,0,0,3,2,0x40};return invoke(190,a,9);}
 uint32_t a[]={0x220200,access,0x220320,0x220220,3,0x40};return invoke(202,a,6);
}
static uint32_t transfer(unsigned ordinal,uint32_t h,unsigned n,uint64_t offset)
{
 memcpy(kernel_guest_at(0x220240,8),&offset,8);store(0x22021C,0x31415926);store(0x220228,0x27182818);
 uint32_t a[]={h,0,0,0,0x220220,0x221000,n,0x220240};uint32_t status=invoke(ordinal,a,8);
 CHECK_EQ_U32(load(0x22021C),0x31415926);CHECK_EQ_U32(load(0x220228),0x27182818);return status;
}
static void mount(unsigned port,unsigned slot,char expected,const char *target)
{
 uint32_t a[]={port,slot,0x220260};kernel_call_frame frame={0};
 CHECK(kernel_frame_build(&frame,0x220100,0x180,a,3));CHECK_EQ_U32(xinput_hle_call(MU_ENTRY_MOUNT,&frame),0);
 CHECK(*(char *)kernel_guest_at(0x220260,1)==expected);
 char link[16];snprintf(link,sizeof link,"\\??\\%c:",expected);
 const char *actual=kernel_file_symlink_target(link);CHECK(actual && strcmp(actual,target)==0);
}
static uint32_t unmount(unsigned port,unsigned slot)
{
 uint32_t args[]={port,slot};kernel_call_frame frame={0};
 CHECK(kernel_frame_build(&frame,0x220100,0x180,args,2));return xinput_hle_call(MU_ENTRY_UNMOUNT,&frame);
}
static void backing(unsigned port,unsigned slot,const char *path,unsigned id)
{
 CHECK(mu_attach_blank(port,slot,8u*1024u*1024u,true,id));size_t size;
 uint8_t *image=mu_image(port,slot,&size);FILE *f=fopen(path,"wb");CHECK(f!=NULL);
 if(f){CHECK(fwrite(image,1,size,f)==size);CHECK(fclose(f)==0);}
 CHECK(mu_detach(port,slot));CHECK(mu_attach_file(port,slot,path,0));
}
static void persisted(const char *path,const uint8_t *expected,unsigned length)
{
 FILE *f=fopen(path,"rb");CHECK(f!=NULL);if(!f)return;
 uint8_t *image=malloc(8u*1024u*1024u),*out=malloc(length+1);CHECK(image && out);
 if(image && out){CHECK(fread(image,1,8u*1024u*1024u,f)==8u*1024u*1024u);fatx_volume v;
 CHECK(fatx_open(&v,image,8u*1024u*1024u)==FATX_OK);uint32_t got=0;
 CHECK(fatx_read_range(&v,"8123abcd/PAIR.DAT",0,out,length+1,&got)==FATX_OK);
 CHECK_EQ_U32(got,length);CHECK(memcmp(out,expected,length)==0);}
 CHECK(fclose(f)==0);free(image);free(out);
}
int main(int argc,char **argv)
{
 CHECK(argc==4);if(argc!=4)return 1;
 environment_begin(KERNEL_AV_PACK_HDTV);xinput_hle_init();kernel_file_reset();
 map_fixed(0x220000,0x10000);map_fixed(0x10000,0x2000);map_fixed(0x76E000,0x1000);
 store(0x10118,0x11000);store(0x11008,0x8123ABCD);store(MU_TITLE_DRIVE_ADDRESS,0);
 mu_reset();CHECK(mu_enable_guest_filesystem(true));CHECK_EQ_U32(mu_register(),2);
 backing(3,1,argv[1],0x7251);backing(0,1,argv[2],0x7252);backing(2,0,argv[3],0x7253);
 store(0x10118,0xFFFFFFFC);char denied=0x55;
 CHECK_EQ_U32(mu_xmount(3,1,true,&denied),0x57);CHECK(denied==0);CHECK_EQ_U32(mu_mounted_mask(),0);
 CHECK(kernel_file_symlink_target("\\??\\M:")==NULL);store(0x10118,0x11000);
 mount(3,1,'M',"\\Device\\MU_7\\8123abcd");mount(0,1,'G',"\\Device\\MU_6\\8123abcd");mount(2,0,'J',"\\Device\\MU_5\\8123abcd");
 CHECK_EQ_U32(mu_mounted_mask(),0x92);CHECK(!mu_detach(3,1));CHECK(!mu_enable_guest_filesystem(false));
 CHECK_EQ_U32(file("\\??\\M:\\PAIR.DAT",true,0x12019F),0);uint32_t old=load(0x220200);
 uint8_t expected[16460]={0};for(unsigned i=0;i<17;i++)expected[3+i]=(uint8_t)(i*13+7);
 for(unsigned i=0;i<80;i++)expected[16380+i]=(uint8_t)(255-i*3);
 memcpy(kernel_guest_at(0x221000,17),expected+3,17);CHECK_EQ_U32(transfer(236,old,17,3),0);CHECK_EQ_U32(load(0x220224),17);
 memcpy(kernel_guest_at(0x221000,80),expected+16380,80);CHECK_EQ_U32(transfer(236,old,80,16380),0);CHECK_EQ_U32(load(0x220224),80);
 memset(kernel_guest_at(0x221000,128),0xCA,128);CHECK_EQ_U32(transfer(219,old,120,16360),0);CHECK_EQ_U32(load(0x220224),100);
 CHECK(memcmp(kernel_guest_at(0x221000,100),expected+16360,100)==0);CHECK(*(uint8_t *)kernel_guest_at(0x221064,1)==0xCA);
 uint32_t a[]={old,0x220220};CHECK_EQ_U32(invoke(198,a,2),0);persisted(argv[1],expected,sizeof expected);
 CHECK_EQ_U32(file("\\??\\M:\\PAIR.DAT",true,0x12019F),0xC0000035);CHECK_EQ_U32(load(0x220200),0xBAADC0DE);
 CHECK_EQ_U32(file("\\??\\M:\\absent\\x",true,0x12019F),0xC000003A);
 CHECK_EQ_U32(file("\\??\\M:\\PAIR.DAT",false,0x120089),0);uint32_t ro=load(0x220200);
 CHECK_EQ_U32(transfer(236,ro,1,0),0xC0000022);CHECK_EQ_U32(load(0x220224),0);
 CHECK_EQ_U32(file("\\??\\G:\\PAIR.DAT",true,0x12019F),0);uint32_t other=load(0x220200);
 uint8_t distinct[]={0xEE,0x51,0x00,0x99,0x13,0x57,0xBD};memcpy(kernel_guest_at(0x221000,sizeof distinct),distinct,sizeof distinct);
 CHECK_EQ_U32(transfer(236,other,sizeof distinct,0),0);
 CHECK_EQ_U32(unmount(0,1),0);CHECK_EQ_U32(unmount(3,1),0);
 mount(0,1,'G',"\\Device\\MU_7\\8123abcd");
 CHECK(!kernel_file_open_info(old,&(kernel_file_open){0}));CHECK_EQ_U32(transfer(219,old,1,0),0xC0000008);
 CHECK_EQ_U32(file("\\??\\G:\\PAIR.DAT",false,0x120089),0);uint32_t current=load(0x220200);
 CHECK_EQ_U32(transfer(219,current,20,0),0);CHECK_EQ_U32(load(0x220224),sizeof distinct);CHECK(memcmp(kernel_guest_at(0x221000,sizeof distinct),distinct,sizeof distinct)==0);
 CHECK_EQ_U32(unmount(0,1),0);CHECK(mu_detach(3,1));CHECK(mu_attach_file(3,1,argv[1],0));
 mount(3,1,'M',"\\Device\\MU_7\\8123abcd");CHECK_EQ_U32(file("\\??\\M:\\PAIR.DAT",false,0x120089),0);uint32_t reopened=load(0x220200);
 CHECK_EQ_U32(transfer(219,reopened,sizeof expected,0),0);CHECK_EQ_U32(load(0x220224),sizeof expected);CHECK(memcmp(kernel_guest_at(0x221000,sizeof expected),expected,sizeof expected)==0);
 mu_reset();CHECK_EQ_U32(load(MU_MASK_ADDRESS),0);CHECK(!kernel_file_open_info(reopened,&(kernel_file_open){0}));CHECK_EQ_U32(transfer(219,reopened,1,0),0xC0000008);
 CHECK(kernel_file_symlink_target("\\??\\M:")==NULL);CHECK(kernel_file_symlink_target("\\??\\J:")==NULL);
 persisted(argv[1],expected,sizeof expected);persisted(argv[2],distinct,sizeof distinct);
 CHECK(mu_enable_guest_filesystem(false));environment_end();
 printf("T1134 independent bridge: %d checks, %d failures\n",checks,failures);return failures!=0;
}
