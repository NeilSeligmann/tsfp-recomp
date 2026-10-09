/* SPDX-License-Identifier: GPL-3.0-or-later
 * Boundary and alias controls for the original batch2 state contracts. */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax,g_ecx,g_edx,g_esp,g_esi,g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static unsigned char mem[0x810000];
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while (0)
#ifndef T1787_BATCH2_DRAFT
#define T1787_BATCH2_DRAFT "../../docs/data/t1787-engine/batch2/frozen-draft.c.txt"
#endif
#include T1787_BATCH2_DRAFT
static void setup(uint32_t a,uint32_t b) {
 memset(mem,0,sizeof mem);g_esp=0x800000;g_ecx=0xAABBCCDD;g_edx=0xFEDCBA98;g_esi=0xCAFE1234;
 guest_write32(g_esp,0x12345678);guest_write32(g_esp+4,a);guest_write32(g_esp+8,b);
}
static void preserved(void) { CHECK(g_esp==0x800004);CHECK(g_esi==0xCAFE1234);CHECK(guest_read32(0x800000)==0x12345678); }
int main(void) {
 g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)mem;
 /* SHL masks count to five bits, including high-bit counts. */
 const uint32_t shifts[]={0,31,32,63,0xFFFFFFFF};
 for(unsigned i=0;i<5;i++) {setup(shifts[i],0);guest_write32(0x4CB54C,0x80000001);sub_00045D60();CHECK(g_eax==1);CHECK(g_ecx==shifts[i]);CHECK(g_edx==0xFEDCBA98);preserved();}
 setup(2,0);guest_write32(0x7A2BF4,0x10000);guest_write32(0x10028,0x80);sub_00274380();CHECK(g_eax==0);CHECK(g_ecx==0x10000);preserved();
 setup(2,0);guest_write32(0x7A2BF4,0x10000);guest_write32(0x1007C,0x20000);guest_write32(0x200A0,0xFFFFFFFB);sub_00274330();CHECK(g_eax==0);CHECK(g_ecx==0x10000);CHECK(g_edx==0x20000);preserved();
 setup(0x10000,0x80000000);guest_write32(0x10004,0x20000);guest_write32(0x200D4,0x80000000);sub_00059D70();CHECK(g_eax==1);CHECK(g_ecx==0x80000000);CHECK(g_edx==0x80000000);preserved();
 setup(0,0);sub_000B0F90();CHECK(g_eax==0);CHECK(g_ecx==0xAABBCCDD);CHECK(g_edx==0xFEDCBA98);preserved();
 setup(0x10000,0);guest_write32(0x1007C,0x20000);sub_000B0F90();CHECK(g_eax==1);CHECK(g_ecx==1);CHECK(g_edx==0);preserved();
 setup(1,0);guest_write32(0x7A2BF4,0x10000);guest_write32(0x1007C,0x20000);guest_write32(0x2003C,0x12345678);sub_002744C0();CHECK(g_eax==0x12345678);CHECK(g_ecx==0x10000);CHECK(g_edx==0x20000);preserved();
 /* Forward overlap propagates the first word; a memcpy-style snapshot is wrong. */
 setup(0x10000,0x1001C);guest_write32(0x1001C,0x11223344);guest_write32(0x10020,0x55667788);guest_write32(0x10024,0x99AABBCC);sub_000FF490();CHECK(guest_read32(0x10020)==0x11223344);CHECK(guest_read32(0x10024)==0x11223344);CHECK(guest_read32(0x10028)==0x11223344);CHECK(g_ecx==0x11223344);CHECK(g_edx==0x11223344);preserved();
 const uint32_t modes[]={4,6,0xFFFFFFFE};const uint32_t values[]={0x1B26,0x10E8,0};
 for(unsigned i=0;i<3;i++){setup(0,0);guest_write32(0x79094C,modes[i]);sub_00164900();CHECK(g_eax==values[i]);CHECK(g_ecx==0xAABBCCDD);CHECK(g_edx==0xFEDCBA98);preserved();}
 setup(0x10000,0);guest_write32(0x10114,0x40000);sub_000D1C90();CHECK(g_eax==1);preserved();
 setup(0x10000,0);guest_write32(0x1007C,0x20000);guest_write32(0x20010,2);sub_000B0F60();CHECK(g_eax==1);CHECK(g_ecx==0xAABBCCDD);CHECK(g_edx==0xFEDCBA98);preserved();
 setup(1,0);guest_write32(0x7A2BF4,0x10000);guest_write32(0x1007C,0x20000);guest_write32(0x200A0,0xFFFFFFFF);sub_00274350();CHECK(g_eax==4);CHECK(g_ecx==0x10000);CHECK(g_edx==0x20000);preserved();
 /* Wrapped difference +4 reaches INT32_MIN and must compare signed. */
 setup(0,0);guest_write32(0x7B4FAC,0x7FFFFFFC);sub_000A4C70();CHECK(g_eax==1);CHECK(g_edx==0x80000000);CHECK(g_ecx==0);preserved();
 printf("T1787 batch2 native: %u checks, %u failures\n",checks,failures);return failures!=0;
}
