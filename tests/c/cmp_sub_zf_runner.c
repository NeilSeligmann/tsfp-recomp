/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
void sub_002561D0(void);
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc,uint32_t va){(void)cc;(void)va;_exit(86);}
recomp_func_t recomp_lookup(uint32_t va){(void)va;_exit(87);}
recomp_func_t recomp_lookup_manual(uint32_t va){(void)va;_exit(87);}
recomp_func_t recomp_lookup_kernel(uint32_t va){(void)va;_exit(87);}
RECOMP_NORETURN void recomp_icall_unresolved_trap(uint32_t va,const char *kind){(void)va;(void)kind;_exit(87);}
int main(int argc,char **argv){
 if(argc!=3)return 2;FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out)return 2;
 uint32_t input[3];unsigned cases=0;
 while(fread(input,sizeof input,1,in)==1){
  if(++cases>64)return 2;
  unsigned char *mem=mmap(NULL,0x480000,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(mem==MAP_FAILED)return 2;
  if(mprotect(mem+0x1000,4096,PROT_READ|PROT_WRITE)||mprotect(mem+0x5000,4096,PROT_READ|PROT_WRITE)||mprotect(mem+0x475000,4096,PROT_READ|PROT_WRITE))return 2;
  memset(mem+0x1000,0xa5,4096);memset(mem+0x5000,0xb6,4096);g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)mem;
  uint32_t first=0x1100,second=0x1104,last=0x1108;
  if(input[2]==1)first=second=last=0;
  else if(input[2]==2)second=first;
  else if(input[2]==3)first=0x9000;
  MEM32(0x5800)=0x7000;MEM32(0x5804)=input[0];MEM32(0x5808)=input[1];MEM32(0x580c)=first;MEM32(0x5810)=second;MEM32(0x5814)=last;
  MEM32(0x475db4)=0x40800000;
  if(mprotect(mem+0x5000,4096,PROT_READ)||mprotect(mem+0x475000,4096,PROT_READ))return 2;
  g_eax=0x11111111;g_ecx=0x22222222;g_edx=0x33333333;g_ebx=0x44444444;g_esi=0x55555555;g_edi=0x66666666;g_ebp=0x77777777;g_esp=0x5800;
  RecompXmm *vectors[]={&g_xmm0,&g_xmm1,&g_xmm2,&g_xmm3,&g_xmm4,&g_xmm5,&g_xmm6,&g_xmm7};
  for(unsigned n=0;n<8;n++)for(unsigned lane=0;lane<4;lane++)vectors[n]->u[lane]=0x3f800000u+n*0x100u+lane;
  _mm_setcsr(0x1f80);sub_002561D0();
  uint32_t regs[]={g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi};if(fwrite(regs,sizeof regs,1,out)!=1)return 2;
  for(unsigned n=0;n<8;n++)if(fwrite(vectors[n]->u,16,1,out)!=1)return 2;
  uint32_t mxcsr=_mm_getcsr();if(fwrite(&mxcsr,4,1,out)!=1)return 2;
  if(fwrite(mem+0x1000,4096,1,out)!=1||fwrite(mem+0x5000,4096,1,out)!=1)return 2;
  if(munmap(mem,0x480000))return 2;
 }
 if(!feof(in)||ferror(in)||fclose(in)||fclose(out))return 2;return 0;
}
