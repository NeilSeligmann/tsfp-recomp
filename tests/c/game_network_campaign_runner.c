/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1790 original-instruction expectations: descriptor mask/CL, pair sentinel, ABI. */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax,g_ecx,g_edx,g_esp,g_esi,g_ebx,g_ebp,g_edi,g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static unsigned checks, failures;
static uint8_t memory[0x810000u];
#define CHECK(e) do {++checks; if(!(e)){++failures;fprintf(stderr,"line%d: %s\n",__LINE__,#e);}}while(0)
void sub_003B5FE0(void);
void sub_000387D0(void);
static void prepare(void) {
 memset(memory,0,sizeof memory);g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
 g_esp=0x1000;guest_write32(g_esp,0x12345678);
 g_eax=0x11111111;g_ecx=0xA5C3E277;g_edx=0x33333333;
 g_esi=0x51515151;g_ebx=0xB0B0B0B0;g_ebp=0xBEBEBEBE;g_edi=0xD1D1D1D1;
}
static void preserved(uint32_t esp) {
 CHECK(g_esp==esp);CHECK(guest_read32(0x1000)==0x12345678);
 CHECK(g_edx==0x33333333);CHECK(g_esi==0x51515151);
 CHECK(g_ebx==0xB0B0B0B0);CHECK(g_ebp==0xBEBEBEBE);CHECK(g_edi==0xD1D1D1D1);
}
int main(void) {
 unsigned before;
 /* Sweep every byte flag, every source bit, and aliases into argument/return slots.
  * Original low24 retention and top flags expressed independently as bit checks. */
 before=checks;
 const uint32_t pointers[]={0x30000,0xFE0,0xFD8,0xFE4};
 for(unsigned p=0;p<4;p++)for(unsigned bit=0;bit<32;bit++)for(unsigned flag=0;flag<256;flag++){
  prepare();guest_write32(0x1004,pointers[p]);guest_write32(0x1008,0xDEADBEEF);
  guest_write32(0x100C,0xCAFE0000|flag);
  guest_write32(pointers[p]+0x28,1u<<bit);
  /* Snapshot after all aliasing setup; original reads word then flag, without writes. */
  uint32_t word=guest_read32(guest_read32(0x1004)+0x28);
  uint32_t byte=guest_read8(0x100C);uint32_t oldret=guest_read32(0x1000);
  sub_003B5FE0();
  CHECK((g_eax&0xFFFFFF)==(word&0xFFFFFF));
  CHECK((g_eax>>24)==(byte?0xB0u:0x80u));
  CHECK(g_ecx==(0xA5C3E200|byte));CHECK(g_esp==0x1010);
  CHECK(guest_read32(0x1000)==oldret);CHECK(guest_read32(pointers[p]+0x28)==word);
  CHECK(g_edx==0x33333333 && g_esi==0x51515151 && g_ebx==0xB0B0B0B0 && g_ebp==0xBEBEBEBE && g_edi==0xD1D1D1D1);
 }
 printf("0x003B5FE0 %u checks %u failures\n",checks-before,failures);
 before=checks;
 const uint32_t values[]={0,1,2,0xFFFFFFFF,0x80000000,0x12345678};
 for(unsigned a=0;a<6;a++)for(unsigned b=0;b<6;b++){
  prepare();guest_write32(0x68DDAC,values[a]);guest_write32(0x68DDB0,values[b]);
  sub_000387D0();CHECK(g_eax==((values[a]&&values[b])?values[b]:0xFFFFFFFF));
  CHECK(g_ecx==0xA5C3E277);preserved(0x1004);
  CHECK(guest_read32(0x68DDAC)==values[a]&&guest_read32(0x68DDB0)==values[b]);
 }
 printf("0x000387D0 %u checks %u failures\n",checks-before,failures);
 printf("%u checks %u failures\n",checks,failures);return failures?1:0;
}
