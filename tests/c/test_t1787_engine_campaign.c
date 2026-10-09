/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent contract cases for the frozen campaign, including rejected drafts. */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax,g_ecx,g_edx,g_esp,g_esi,g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static unsigned char mem[0x810000];
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
#ifndef T1787_DRAFT
#define T1787_DRAFT "../../docs/data/t1787-engine/proved-all-12-drafts.c.txt"
#endif
#include T1787_DRAFT
static void setup(uint32_t a,uint32_t b) {
 memset(mem,0,sizeof mem);g_esp=0x800000;g_eax=0x12345678;g_ecx=0xAABBCCDD;g_edx=0xFEDCBA98;g_esi=0xCAFE1234;
 guest_write32(g_esp,0x7777);guest_write32(g_esp+4,a);guest_write32(g_esp+8,b);
}
static void preserved(void){CHECK(g_esp==0x800004);CHECK(g_esi==0xCAFE1234);CHECK(guest_read32(0x800000)==0x7777);}
int main(void) {
 g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)mem;
 const uint32_t vals[]={0,1,2,0x80000000u,0xFFFFFFFFu,0xA5A55A5A};
 for(unsigned i=0;i<6;i++)for(unsigned j=0;j<6;j++){
 uint32_t a=vals[i],b=vals[j];
 setup(0x10000,0);guest_write32(0x10000,a);guest_write32(0x10004,b);sub_00063BF0();CHECK(g_eax==(a==b));CHECK(g_ecx==0x10000);CHECK(g_edx==0xFEDCBA98);preserved();
 setup(a,0);guest_write32(0x7AD28C,0x10000);guest_write8(0x10012,0x91);guest_write8(0x10014,0x92);sub_0010E790();CHECK(g_eax==0x10000);CHECK(g_ecx==(0xAABBCC00u|(a&255)));CHECK(guest_read8(0x10013)==(a&255));CHECK(guest_read8(0x10012)==0x91&&guest_read8(0x10014)==0x92);preserved();
 setup(a,0);sub_0010E790();CHECK(g_eax==0);CHECK(g_ecx==0xAABBCCDD);preserved();
 setup(0x10000,0);guest_write32(0x10C6C,a);sub_00124C70();CHECK(g_eax==((a>>21)&1));CHECK(g_ecx==0xAABBCCDD);preserved();
 setup(i,0);guest_write32(0x521A24+i*48,a);sub_002BE670();CHECK(g_eax==a);preserved();
 setup(0x10000,b);guest_write32(0x101F4,0x56781234);campaign_write16(0x101F6,(uint16_t)a);sub_000942D0();CHECK(g_eax==0x10000);CHECK(g_ecx==~b);CHECK(campaign_read16(0x101F6)==((uint16_t)a&(uint16_t)~b));CHECK(campaign_read16(0x101F4)==0x1234);preserved();
 setup(0,0);guest_write32(0x7AC808,i);guest_write32(0x7AC804,0x10000);guest_write32(0x10000+i*40,a);sub_00119C20();CHECK(g_eax==a);CHECK(g_ecx==0x10000);preserved();
 setup(i,0);guest_write32(0x7A2BE4+i*8,0x10000);guest_write32(0x1007C,0x20000);guest_write32(0x2003C,a);sub_002744A0();CHECK(g_eax==a);CHECK(g_ecx==0x10000);CHECK(g_edx==0x20000);preserved();
 setup(0x10000,0);guest_write32(0x1007C,0x20000);guest_write32(0x20010,a);sub_00242EC0();CHECK(g_eax==(a==1));CHECK(g_ecx==0x20000);CHECK(g_edx==a);preserved();
 setup(a,0);guest_write32(0x7AD28C,0x10000);guest_write32(0x10084,b);guest_write32(0x1008C,b);sub_0010E620();CHECK(g_eax==0x10000);CHECK(g_ecx==a);CHECK(guest_read32(0x10088)==a);CHECK(guest_read32(0x10084)==b&&guest_read32(0x1008C)==b);preserved();
 setup(a,0);sub_0010E620();CHECK(g_eax==0);CHECK(g_ecx==0xAABBCCDD);preserved();
 setup(a,0);guest_write32(0x7A2BEC,0x12345678);guest_write32(0x7A2BF4,0x87654321);sub_002744F0();CHECK(g_eax==(a==1?0x87654321:0x12345678));CHECK(g_ecx==a);preserved();
 setup(0x10000,b);guest_write32(0x100E8,a);sub_001B63B0();CHECK(g_eax==(a==b));CHECK(g_ecx==b);CHECK(g_edx==a);preserved();
 setup(0,0);guest_write32(0x6B7AA8,a);guest_write32(0x6B83C8,0xBEEF);guest_write32(0x6B83CC,0xCAFE);sub_0003EA20();CHECK(g_eax==0);CHECK(g_ecx==a);CHECK(guest_read32(0x6B83C8)==(a?0xBEEF:0));CHECK(guest_read32(0x6B83CC)==(a?0xCAFE:0));preserved();
 }
 /* Store target aliases stack arg: old load must happen before its overwrite. */
 setup(0x1234,0);guest_write32(0x7AD28C,0x800004-0x88);sub_0010E620();CHECK(guest_read32(0x800004)==0x1234);CHECK(g_ecx==0x1234);preserved();
 printf("T1787 native: %u checks, %u failures\n",checks,failures);return failures!=0;
}
