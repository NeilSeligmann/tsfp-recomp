/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <stdio.h>
#ifndef T1479_EMBEDDED
__thread uint32_t g_eax,g_ecx,g_edx,g_esp,g_esi,g_edi,g_ebx;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x810000];
static unsigned checks,failures;
#define CHECK(e) do { ++checks; if (!(e)) {++failures;fprintf(stderr,"line %d: %s\n",__LINE__,#e);} } while(0)
#endif
void sub_002ED7B0(void);void sub_00346250(void);void sub_00350D40(void);void sub_00350E70(void);void sub_00356020(void);void sub_0035E590(void);void sub_0037A6F0(void);
static void census002_prepare(uint32_t stack) { memset(memory,0,sizeof memory);g_esp=stack;guest_write32(stack,0x10000u);g_eax=0xaabbccddu;g_ecx=0x11223344u;g_edx=0x12345678u;g_esi=0x44556677u;g_edi=0x55667788u;g_ebx=0xabcdef01u;g_df=0; }
static void test_t1479_census002(void) {
 g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
 census002_prepare(0x1000u); const uint32_t reset[]={0x761c58u,0x7625f4u,0x761c68u,0x761c6cu,0x761c70u,0x761c74u,0x761bdcu,0x761be0u,0x761be4u,0x761be8u};for(unsigned i=0;i<10;i++)guest_write32(reset[i],0xffffffffu);sub_002ED7B0();for(unsigned i=0;i<10;i++)CHECK(guest_read32(reset[i])==0u);CHECK(guest_read32(0x761c7cu)==1u && g_eax==0 && g_ecx==0x11223344u && g_edx==0x12345678u && g_esp==0x1004u);
 for(int count=-1;count<=33;count++) {census002_prepare(0x1000u);guest_write32(0x7670e0u,(uint32_t)count);guest_write32(0x1004u,0x11111111u);guest_write32(0x1008u,0xaabbccddu);guest_write32(0x100cu,0x33333333u);sub_00346250();if(count>=32)CHECK(g_eax==0xaabbccddu && g_edx==0x12345678u && g_ecx==(uint32_t)count);else {uint32_t dest=0x766f60u+(uint32_t)count*12u;CHECK(guest_read32(dest)==0x11111111u && guest_read32(dest+4u)==0xaabbcc00u && guest_read32(dest+8u)==0x33333333u);CHECK(guest_read32(0x7670e0u)==(uint32_t)(count+1) && g_eax==(uint32_t)count*12u && g_ecx==(uint32_t)(count+1) && g_edx==0x33333333u);}CHECK(g_esp==0x1004u);}
 /* First queue store aliases the second stack argument: read it after the store. */
 census002_prepare(0x766f58u);guest_write32(0x7670e0u,0u);guest_write32(g_esp+4u,0x12345678u);guest_write32(g_esp+8u,0xffffffffu);guest_write32(g_esp+12u,0x87654321u);sub_00346250();CHECK(guest_read32(0x766f64u)==0x12345600u && guest_read32(0x766f68u)==0x12345600u);
 for(unsigned mode=0;mode<2;mode++)for(unsigned flag=0;flag<2;flag++)for(int index=-1;index<=1;index++) {census002_prepare(0x1000u);guest_write8(0x7de455u,mode?10u:0u);guest_write32(0x7de458u,flag?0x80000u:0u);uint16_t word=(uint16_t)index;memcpy(memory+0x20048u,&word,2u);guest_write32(0x1004u,0x20000u);guest_write32(0x53c508u+(uint32_t)index*20u,0x10204081u);sub_00350D40();CHECK(g_eax==((mode||flag)?0x10204081u:0u));CHECK(g_ecx==((mode||flag)?(uint32_t)index*5u:0x11223344u) && g_edx==0x12345678u && g_esp==0x1004u);
 const uint32_t masks[]={1u,2u,0x80u};for(unsigned m=0;m<3;m++){g_esp=0x1000u;guest_write32(0x1008u,masks[m]);sub_00350E70();uint32_t expected=m==0?1u:(mode||flag)?(0x10204081u&masks[m]):0u;CHECK(g_eax==expected && g_ecx==masks[m] && g_esp==0x1004u);}}
 for(int index=-1;index<=16;index++){census002_prepare(0x1000u);guest_write32(0x1004u,(uint32_t)index);guest_write32(0x1008u,0x1234u);guest_write32(0x100cu,0x5678u);sub_00356020();if(index<0||index>=16)CHECK(g_eax==0xaabbccddu && g_edx==0x12345678u && g_ebx==0xabcdef01u);else {uint32_t base=0x774180u+(uint32_t)index*0x30cu;CHECK(guest_read32(base)==(uint32_t)index && guest_read32(base+4u)==0x1234u && guest_read32(base+32u)==0x5678u);CHECK(g_eax==(uint32_t)index*0x30cu && g_ecx==0x5678u && g_edx==0x12345601u && g_ebx==0xabcdef01u);CHECK(guest_read8(base+52u)==1u && guest_read8(base+8u)==1u && guest_read8(base+9u)==0u);}CHECK(g_esp==0x1004u);}
 /* A legitimate caller stack aliases the saved EBX slot, without overwriting RET. */
 census002_prepare(0x7741a4u);guest_write32(g_esp+4u,0u);guest_write32(g_esp+8u,0x1234u);guest_write32(g_esp+12u,0x5678u);guest_write8(0x7741b4u,0xa4u);sub_00356020();CHECK(g_ebx==0x5678u && g_edx==0x12345601u && g_esp==0x7741a8u && guest_read32(0x7741a4u)==0x10000u && guest_read8(0x7741b4u)==0xa5u);
 for(int count=-2;count<=3;count++)for(int extra=-1;extra<=2;extra++)for(int limit=-1;limit<=4;limit++)for(unsigned local=0;local<2;local++){census002_prepare(0x1000u);guest_write32(0x76bbbcu,(uint32_t)count);guest_write32(0x76bbc4u,(uint32_t)extra);guest_write32(0x1004u,local);guest_write32(0x1008u,(uint32_t)limit);sub_0035E590();unsigned expected=count>=limit || (!local && count+extra>=limit);CHECK(g_eax==expected && g_ecx==(uint32_t)limit && g_esp==0x1004u);CHECK(g_edx==(count>=limit?0x12345678u:local?local:(uint32_t)(count+extra)));}
 for(unsigned path=0;path<4;path++){census002_prepare(0x1000u);guest_write32(0x1004u,0x20000u);guest_write32(0x1008u,0x40000u);guest_write32(0x20020u,path?8u:7u);guest_write32(0x2007cu,path>=2?0x30000u:0u);guest_write32(0x30aa0u,path>=3?0x40000u:0u);guest_write32(0x40014u,0x11u);guest_write32(0x40018u,0x22u);guest_write32(0x4001cu,0x33u);sub_0037A6F0();CHECK(g_eax==(path==0?0x20000u:path<3?0u:0x33u));CHECK(g_ecx==(path==3?0x40000u:0x11223344u) && g_edx==(path==3?0x22u:0x12345678u) && g_esp==0x1004u);CHECK(guest_read32(0x40000u)==(path==3?0x11u:0u));}

}

#ifndef T1479_EMBEDDED
int main(void) { test_t1479_census002();printf("%u checks %u failures\n",checks,failures);return failures?1:0;}
#endif
