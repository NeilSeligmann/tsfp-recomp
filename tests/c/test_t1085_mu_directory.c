/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "mu_device.h"
#include "kernel_file.h"
#include "xinput_hle.h"
static uint32_t call(unsigned ordinal,const uint32_t *args,unsigned count)
{
 kernel_call_frame f;memset(&f,0,sizeof f);CHECK(kernel_frame_build(&f,0x200100u,0x100u,args,count));return kernel_hle_call(ordinal,&f);
}
static void string_at(uint32_t str,uint32_t buffer,const char *name)
{
 uint16_t len=(uint16_t)strlen(name);memcpy(kernel_guest_at(buffer,len+1),name,len+1);
 memcpy(kernel_guest_at(str,2),&len,2);len++;memcpy(kernel_guest_at(str+2,2),&len,2);store(str+4,buffer);
}
static uint32_t open_path(const char *name,unsigned disposition,bool directory)
{
 string_at(0x200300,0x200400,name);store(0x200320,0);store(0x200324,0x200300);store(0x200328,0x40);
 uint32_t args[]={0x200200,0x12019F,0x200320,0x200220,0,0,3,disposition,directory?1:0x40};CHECK_EQ_U32(call(190,args,9),0);return load(0x200200);
}
static void write_file(uint32_t handle,unsigned count)
{
 memset(kernel_guest_at(0x202000,count),0x71,count);store(0x200240,0);store(0x200244,0);
 uint32_t args[]={handle,0,0,0,0x200220,0x202000,count,0x200240};CHECK_EQ_U32(call(236,args,8),0);CHECK_EQ_U32(load(0x200224),count);
}
static uint32_t query(uint32_t h,const char *mask,bool restart,unsigned length,unsigned cls,unsigned event)
{
 if(mask)string_at(0x200340,0x200600,mask);
 memset(kernel_guest_at(0x200800,0x148),0xA5,0x148);store(0x2007FC,0xAABBCCDD);store(0x200948,0xDEADBEEF);
 store(0x200220,0xABC123);store(0x200224,0xABC123);
 uint32_t args[]={h,event,0,0,0x200220,0x200800,length,cls,mask?0x200340:0,restart};
 uint32_t status=call(207,args,10);CHECK_EQ_U32(load(0x200220),status);
 CHECK_EQ_U32(load(0x2007FC),0xAABBCCDD);CHECK_EQ_U32(load(0x200948),0xDEADBEEF);return status;
}
static uint64_t u64(uint32_t at){uint64_t v;memcpy(&v,kernel_guest_at(at,8),8);return v;}
static void result(const char *name,uint64_t size,uint64_t allocated,uint32_t attr)
{
 size_t n=strlen(name);CHECK_EQ_U32(load(0x200800),0);CHECK_EQ_U32(load(0x20083C),n);
 CHECK_EQ_U32(load(0x200224),0x40+n);CHECK(memcmp(kernel_guest_at(0x200840,n),name,n)==0);
 CHECK(*(uint8_t *)kernel_guest_at(0x200840+(uint32_t)n,1)==0xA5);
 CHECK(u64(0x200828)==size);CHECK(u64(0x200830)==allocated);CHECK_EQ_U32(load(0x200838),attr);
}
static void no_entry(void){CHECK_EQ_U32(load(0x200224),0);CHECK_EQ_U32(load(0x200800),0xA5A5A5A5);}
int main(void)
{
 environment_begin(KERNEL_AV_PACK_HDTV);xinput_hle_init();kernel_file_reset();map_fixed(0x200000,0x10000);map_fixed(0x10000,0x2000);map_fixed(0x76E000,0x1000);
 store(0x10118,0x11000);store(0x11008,0x45410066);store(MU_TITLE_DRIVE_ADDRESS,0);mu_reset();CHECK(mu_enable_guest_filesystem(true));
 CHECK(mu_attach_blank(0,0,8u*1024u*1024u,true,1));char drive;CHECK_EQ_U32(mu_xmount(0,0,true,&drive),0);
 uint32_t z=open_path("\\??\\F:\\zeta.dat",2,false);write_file(z,1);
 uint32_t a=open_path("\\??\\F:\\Alpha.bin",2,false);write_file(a,17000);
 uint32_t m=open_path("\\??\\F:\\mid.bin",2,false);write_file(m,1);
 (void)open_path("\\??\\F:\\SDIR",2,true);
 fatx_volume *v=mu_volume(0,0);CHECK(v!=NULL);
 uint8_t *entries=v->image+v->data_offset+FATX_CLUSTER_BYTES; /* synthetic title directory is cluster2 */
 entries[64+1]=0x23; /* actual raw readonly/hidden/archive bits, not a host stat stand-in */
 const uint16_t date=(2u<<5)|29u,time=(12u<<11)|(34u<<5)|28u;
 for(unsigned off=0x34;off<=0x3C;off+=4){memcpy(entries+64+off,&time,2);memcpy(entries+64+off+2,&date,2);}
 const uint16_t created_date=(1u<<9)|(3u<<5)|1u,created_time=(1u<<11)|(2u<<5)|2u;
 const uint16_t accessed_date=(4u<<9)|(12u<<5)|31u,accessed_time=(23u<<11)|(59u<<5)|29u;
 memcpy(entries+64+0x38,&created_time,2);memcpy(entries+64+0x3A,&created_date,2);
 memcpy(entries+64+0x3C,&accessed_time,2);memcpy(entries+64+0x3E,&accessed_date,2);

 entries[4*64]=0xE5;entries[5*64]=0xFF;entries[6*64]=1;entries[6*64+2]='Q'; /* deleted slot, logical end, poison after end */
 uint8_t *snapshot=malloc(v->size);CHECK(snapshot!=NULL);memcpy(snapshot,v->image,v->size);
 uint32_t dir=open_path("\\??\\F:",1,true);
 CHECK_EQ_U32(query(dir,NULL,false,0x40,1,0),0xC0000004);no_entry();
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0);result("Alpha.bin",17000,32768,0x23);
 CHECK(u64(0x200808)==126278821240000000ull);CHECK(u64(0x200810)==127490111980000000ull);CHECK(u64(0x200818)==125963012960000000ull);CHECK(u64(0x200820)==125963012960000000ull);
 CHECK_EQ_U32(query(dir,"z*",false,0x148,1,0),0);result("mid.bin",1,16384,0);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0);result("SDIR",0,16384,0x10);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0);result("zeta.dat",1,16384,0);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0x80000006);no_entry();
 entries[5*64]=0;CHECK_EQ_U32(query(dir,"z*",true,0x148,1,0),0);result("zeta.dat",1,16384,0);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0x80000006);no_entry();entries[5*64]=0xFF;

 CHECK_EQ_U32(query(dir,"*.BIN",true,0x148,1,0),0);result("Alpha.bin",17000,32768,0x23);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0);result("mid.bin",1,16384,0);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0x80000006);no_entry();
 CHECK_EQ_U32(query(dir,"absent*",true,0x148,1,0),0xC000000F);no_entry();
 CHECK_EQ_U32(query(dir,"s?ir",true,0x148,1,0),0);result("SDIR",0,16384,0x10);
 CHECK_EQ_U32(query(dir,NULL,true,0x148,99,0),0xC0000003);no_entry();
 CHECK_EQ_U32(query(dir,NULL,true,0x148,1,1),0xC0000002);no_entry();
 CHECK_EQ_U32(query(z,NULL,false,0x148,1,0),0xC000000D);no_entry();
 /* Invalid raw name/timestamp/chain must refuse without consuming the next name. */
 uint8_t old=entries[64];entries[64]=43;
 CHECK_EQ_U32(query(dir,NULL,true,0x148,1,0),0xC0000001);no_entry();entries[64]=old;
 uint16_t bad=0xFFFF;memcpy(entries+64+0x36,&bad,2);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0xC0000001);no_entry();memcpy(entries+64+0x36,&date,2);
 uint16_t nonleap=(100u<<9)|(2u<<5)|29u;memcpy(entries+64+0x36,&nonleap,2);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0xC0000001);no_entry();memcpy(entries+64+0x36,&date,2);

 uint32_t first;memcpy(&first,entries+64+0x2C,4);uint16_t next;memcpy(&next,v->image+v->fat_offset+first*2,2);
 uint16_t loop=(uint16_t)first;memcpy(v->image+v->fat_offset+first*2,&loop,2);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0xC0000001);no_entry();memcpy(v->image+v->fat_offset+first*2,&next,2);
 CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0);result("Alpha.bin",17000,32768,0x23);
 CHECK(memcmp(snapshot,v->image,v->size)==0);free(snapshot);
 /* Exact physical directory limit: all 256 live slots, no 0xFF sentinel. */
 uint32_t full=open_path("\\??\\F:\\FULL",2,true);
 for(unsigned i=0;i<256;i++){char path[48];snprintf(path,sizeof path,"45410066/FULL/n%03u",i);CHECK(fatx_write_path(v,path,NULL,0)==FATX_OK);}
 for(unsigned i=0;i<256;i++){char name[8];snprintf(name,sizeof name,"n%03u",i);CHECK_EQ_U32(query(full,NULL,false,0x148,1,0),0);result(name,0,0,0);}
 CHECK_EQ_U32(query(full,NULL,false,0x148,1,0),0x80000006);no_entry();
 CHECK_EQ_U32(mu_xunmount(0,0),0);CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0xC0000008);no_entry();
 CHECK_EQ_U32(mu_xmount(0,0,true,&drive),0);CHECK_EQ_U32(query(dir,NULL,false,0x148,1,0),0xC0000008);no_entry();
 uint32_t again=open_path("\\??\\F:",1,true);CHECK_EQ_U32(query(again,NULL,false,0x148,1,0),0);result("Alpha.bin",17000,32768,0x23);
 mu_reset();CHECK(mu_enable_guest_filesystem(false));environment_end();printf("MU directory: %d checks, %d failures\n",checks,failures);return failures!=0;
}
