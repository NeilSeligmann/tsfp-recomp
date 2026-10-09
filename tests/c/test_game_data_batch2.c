/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "game_replace.h"
ptrdiff_t g_xbox_mem_offset;
__thread uint32_t g_eax,g_ecx,g_edx,g_esp,g_esi,g_edi,g_ebx,g_ebp;
__thread int g_df;
#include "../../docs/data/t1786-batch2/all-drafts.c.txt"
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while(0)
static void reset(void) {g_eax=0x11223344;g_ecx=0x55667788;g_edx=0x99AABBCC;g_esi=0x12345678;g_edi=0x87654321;g_ebx=0x76543210;g_ebp=0xABCDEF98;g_esp=0x100000;g_df=0;}
static void preserved(void) {CHECK(g_esp==0x100004);CHECK(g_edx==0x99AABBCC);CHECK(g_esi==0x12345678);CHECK(g_edi==0x87654321);CHECK(g_ebx==0x76543210);CHECK(g_ebp==0xABCDEF98);}
int main(void) {
 void *memory=calloc(1,0x1000000);assert(memory);g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
 const uint32_t edges[]={0,1,0xFFFFFFFFu,0x80000000u};
 for(unsigned j=0;j<4;++j){uint32_t v=edges[j];reset();guest_write32(0x6B7AA8,v);for(uint32_t a=0x715F28;a<=0x715F3C;a+=4)guest_write32(a,0xCAFE);sub_0009A260();CHECK(g_ecx==v);CHECK(g_eax==0);for(uint32_t a=0x715F2C;a<=0x715F38;a+=4)CHECK(guest_read32(a)==(v?0xCAFEu:0));CHECK(guest_read32(0x715F28)==0xCAFE);CHECK(guest_read32(0x715F3C)==0xCAFE);preserved();
 reset();guest_write32(0x6B7AA8,v);for(uint32_t a=0x54EEC4;a<=0x54EEE8;a+=4)guest_write32(a,0xBEEF);sub_000177D0();CHECK(g_ecx==v);CHECK(g_eax==0);for(uint32_t a=0x54EEC8;a<=0x54EEE4;a+=4)CHECK(guest_read32(a)==(v?0xBEEFu:0));CHECK(guest_read32(0x54EEC4)==0xBEEF);CHECK(guest_read32(0x54EEE8)==0xBEEF);preserved();}
 reset();for(uint32_t a=0x7A283C;a<=0x7A2860;a+=4)guest_write32(a,0xFEED);sub_00162080();CHECK(g_eax==0);CHECK(g_ecx==0x55667788);for(uint32_t a=0x7A2840;a<=0x7A285C;a+=4)CHECK(guest_read32(a)==0);CHECK(guest_read32(0x7A283C)==0xFEED);CHECK(guest_read32(0x7A2860)==0xFEED);preserved();
 for(uint32_t idx=0;idx<13;++idx)for(unsigned f=0;f<4;++f){reset();guest_write32(g_esp+4,idx);guest_write8(0x5655B0+idx*48,f&1);guest_write8(0x5655B3+idx*48,f&2);guest_write32(0x5655BC+idx*48,0xC0010000+idx);sub_00025BB0();CHECK(g_eax==(idx<11&&f==3?0xC0010000+idx:0));uint32_t cl=idx>=11?0x88:(f&1)?(f&2):(f&1);CHECK(g_ecx==(0x55667700u|cl));preserved();}
 /* Signed 16-bit lookup, negative/wrapping index arithmetic and store neighbors. */
 for(int16_t idx=-2;idx<=2;++idx){reset();guest_write32(g_esp+4,3);guest_write32(g_esp+8,0xAABBCCDD);guest_write32(g_esp+12,0x1234);memcpy(game_host_ptr(0x7E03C6),&idx,2);uint32_t dst=0x7DE660+(uint32_t)(int32_t)idx*8;guest_write32(dst-4,0xCAFE);guest_write32(dst+8,0xBEEF);sub_00058240();CHECK(g_eax==0x1234);CHECK(g_ecx==(uint32_t)(int32_t)idx);CHECK(g_edx==0xAABBCCDD);CHECK(guest_read32(dst)==0xAABBCCDD);CHECK(guest_read32(dst+4)==0x1234);CHECK(guest_read32(dst-4)==0xCAFE);CHECK(guest_read32(dst+8)==0xBEEF);CHECK(g_esp==0x100004);CHECK(g_edi==0x87654321);CHECK(g_esi==0x12345678);}
 /* Null and empty save records preserve volatile registers and pop saved EDI. */
 for(unsigned mode=0;mode<2;++mode){reset();guest_write32(g_esp+4,mode?0x200000:0);guest_write32(0x200000,0);sub_000698D0();CHECK(g_eax==0x11223344);CHECK(g_ecx==0x55667788);preserved();}
 /* Direction flag and saved EDI alias: the clear may overwrite its own pop. */
 for(int df=0;df<2;++df){reset();g_df=df;g_esp=0x200004;guest_write32(g_esp+4,0x200000);guest_write32(0x20003C,0x300000);guest_write32(0x300000,0xBAD);sub_000698D0();CHECK(g_eax==0);CHECK(g_ecx==0);CHECK(g_edi==0);CHECK(g_esp==0x200008);CHECK(guest_read32(0x300000)==0);printf("ALIAS %d %u %u %u %u %u\n",df,g_eax,g_ecx,g_edi,g_esp,guest_read32(0x300000));}
 /* Room-list rebasing preserves each saved register unless writes alias its slot. */
 for(int count=-1;count<=2;++count){reset();guest_write32(g_esp+4,0x400000);guest_write32(0x400014,(uint32_t)count);guest_write32(0x400000,0x500000);guest_write32(0x40002C,100);for(unsigned i=0;i<2;++i){guest_write32(0x5000B8+i*0xB4,0x600000+i*0x100);guest_write32(0x600000+i*0x100,2);guest_write32(0x600004+i*0x100,10);guest_write32(0x600008+i*0x100,20);}sub_00085830();CHECK(g_eax==(uint32_t)count);CHECK(g_ecx==0x400000);CHECK(g_edi==0x87654321);CHECK(g_esi==0x12345678);CHECK(g_ebx==0x76543210);CHECK(g_ebp==0xABCDEF98);CHECK(g_esp==0x100004);for(unsigned i=0;i<2;++i){CHECK(guest_read32(0x600004+i*0x100)==(count>(int)i?110u:10u));CHECK(guest_read32(0x600008+i*0x100)==(count>(int)i?120u:20u));}if(count>0)CHECK(g_edx==0);else CHECK(g_edx==0x99AABBCC);}
 reset();g_esp=0x200004;guest_write32(g_esp+4,0x400000);guest_write32(0x400000,0x500000);guest_write32(0x400014,1);guest_write32(0x40002C,100);guest_write32(0x5000B8,0x1FFFF0);guest_write32(0x1FFFF0,4);sub_00085830();CHECK(g_eax==1);CHECK(g_ecx==0x400000);CHECK(g_edx==0);CHECK(g_esi==0x12345678+100);CHECK(g_edi==0x87654321+100);CHECK(g_ebx==0x76543210+100);CHECK(g_ebp==0xABCDEF98u+100);CHECK(g_esp==0x200008);printf("ROOMALIAS %u %u %u %u %u %u %u %u\n",g_eax,g_ecx,g_edx,g_esi,g_edi,g_ebx,g_ebp,g_esp);
 free(memory);printf("T1786 batch2 native: %u checks pass\n",checks);return 0;
}
