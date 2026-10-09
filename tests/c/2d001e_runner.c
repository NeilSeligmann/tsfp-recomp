#define _GNU_SOURCE
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top,g_fp_cmp;
RECOMP_TLS uint16_t g_fp_control_word=0x37f;
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc,uint32_t va){fprintf(stderr,"flags %s %08x\n",cc,va);_exit(86);}
recomp_func_t recomp_lookup(uint32_t va){fprintf(stderr,"dispatch %08x\n",va);_exit(88);}
recomp_func_t recomp_lookup_manual(uint32_t va){return recomp_lookup(va);}
recomp_func_t recomp_lookup_kernel(uint32_t va){return recomp_lookup(va);}
RECOMP_NORETURN void recomp_icall_unresolved_trap(uint32_t va,const char *kind){(void)kind;recomp_lookup(va);_exit(88);}
#include "selected_bodies.inc"
#include "fixture_sections.inc"
int main(int argc,char **argv){
 if(argc!=8)return 2;
 size_t span=0x21000000;unsigned char *base=mmap(NULL,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(base==MAP_FAILED)return 3;
 FILE *f=fopen(argv[1],"rb");if(!f)return 4;
 for(size_t i=0;i<sizeof(sections)/sizeof(sections[0]);i++){uint32_t va=sections[i][0],len=sections[i][1],raw=sections[i][2],bytes=sections[i][3];size_t lo=va&~4095u,hi=(va+len+4095u)&~4095u;if(mprotect(base+lo,hi-lo,PROT_READ|PROT_WRITE))return 5;if(fseek(f,raw,SEEK_SET)||fread(base+va,1,bytes,f)!=bytes)return 6;}
 fclose(f);if(mprotect(base+0x10000000,0x50000,PROT_READ|PROT_WRITE))return 7;if(mprotect(base+0x20000000,8192,PROT_READ|PROT_WRITE))return 8;
 memset(base+0x20000000,0xa5,8192);g_xbox_mem_offset=(ptrdiff_t)base;
 g_eax=0x55667788;g_ebx=0x11223344;g_ecx=0x66778899;g_edx=0x778899aa;g_esi=0x33445566;g_edi=0x44556677;g_ebp=g_seh_ebp=(uint32_t)strtoul(argv[7],NULL,0);g_esp=0x20001000;
 MEM32(g_esp)=0x7000;MEM32(g_esp+4)=0x10000100;MEM32(g_esp+8)=0;MEM32(g_esp+12)=0x10001100;MEM32(0x100001f4)=0x3f800000;
 MEM32(0x78aeec)=(uint32_t)strtoul(argv[3],NULL,0);MEM32(0x789824)=0x10010000;MEM32(0x1004b6d0)=strtoul(argv[4],NULL,0);
 unsigned mxcsr=0x1f80;__asm__ volatile("ldmxcsr %0"::"m"(mxcsr));
 if(strcmp(argv[5],"outer")){FILE *a=fopen(argv[6],"rb");if(!a)return 10;uint32_t regs[8];if(fread(regs,4,8,a)!=8||fread(base+0x20000000,1,8192,a)!=8192)return 11;RecompXmm xs[8];if(fread(xs,16,8,a)!=8)return 12;fclose(a);g_eax=regs[0];g_ebx=regs[1];g_ecx=regs[2];g_edx=regs[3];g_esi=regs[4];g_edi=regs[5];g_ebp=g_seh_ebp=regs[6];g_esp=regs[7];g_xmm0=xs[0];g_xmm1=xs[1];g_xmm2=xs[2];g_xmm3=xs[3];g_xmm4=xs[4];g_xmm5=xs[5];g_xmm6=xs[6];g_xmm7=xs[7];}
 for(size_t i=0;i<sizeof(sections)/sizeof(sections[0]);i++){uint32_t va=sections[i][0],len=sections[i][1];size_t lo=va&~4095u,hi=(va+len+4095u)&~4095u;if(mprotect(base+lo,hi-lo,PROT_READ))return 13;}
 if(mprotect(base+0x78a000,4096,PROT_READ|PROT_WRITE))return 14;unsigned char before[4096];memcpy(before,base+0x78a000,4096);
 if(!strcmp(argv[5],"outer"))sub_002CFFD0();else sub_002D001E();
 for(size_t i=0;i<4096;i++){if(i>=0xeec&&i<0xef0)continue;if(before[i]!=base[0x78a000+i])return 15;}
 uint32_t regs[]={g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi,g_ebp,g_esp};FILE *o=fopen(argv[2],"wb");if(!o)return 16;fwrite(regs,4,8,o);fwrite(base+0x10000000,1,0x50000,o);fwrite(base+0x20000000,1,8192,o);fwrite(base+0x78aeec,1,4,o);fclose(o);puts("RETURN");return 0;
}
