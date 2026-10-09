#define _GNU_SOURCE
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo=0x10000,g_xbox_code_hi=0x450000;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_trace_idx;
volatile uint64_t g_icall_count,g_icall_guard_hits,g_icall_guard_misses;
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top,g_fp_cmp;
RECOMP_TLS uint16_t g_fp_control_word=0x37f;
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc,uint32_t va){fprintf(stderr,"unresolved %s at %08x\n",cc,va);_exit(va==0x2fb575?86:87);}
recomp_func_t recomp_lookup(uint32_t va){fprintf(stderr,"dispatch %08x\n",va);_exit(88);}
recomp_func_t recomp_lookup_manual(uint32_t va){return recomp_lookup(va);}
recomp_func_t recomp_lookup_kernel(uint32_t va){return recomp_lookup(va);}
RECOMP_NORETURN void recomp_icall_unresolved_trap(uint32_t va,const char *kind){(void)kind;recomp_lookup(va);_exit(88);}

#include <sys/mman.h>
#include <string.h>

#include "selected_bodies.inc"
#include "fixture_sections.inc"
int main(int argc,char **argv){
 if(argc!=5)return 2;
 size_t span=0x21000000u;uint8_t *base=mmap(NULL,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(base==MAP_FAILED)return 3;
 FILE *f=fopen(argv[2],"rb");if(!f)return 4;
 for(size_t i=0;i<sizeof(sections)/sizeof(sections[0]);i++){
  uint32_t va=sections[i][0],len=sections[i][1],raw=sections[i][2],bytes=sections[i][3];
  size_t lo=va&~4095u,hi=(va+len+4095u)&~4095u;
  if(mprotect(base+lo,hi-lo,PROT_READ|PROT_WRITE))return 5;
  if(fseek(f,raw,SEEK_SET)||fread(base+va,1,bytes,f)!=bytes)return 6;
 }
 fclose(f);if(mprotect(base+0x10000000,0x1000,PROT_READ|PROT_WRITE))return 7;
 if(mprotect(base+0x20000000,0x2000,PROT_READ|PROT_WRITE))return 8;
 memset(base+0x20000000,0xa5,0x2000);g_xbox_mem_offset=(ptrdiff_t)base;
 g_esp=0x20001000;MEM32(g_esp)=0x7000;MEM32(0x778d34)=0x10000800;MEM32(0x7842a8)=0x10000000;MEM32(0x1000001c)=0x10000400;MEM32(0x4e8cec)=60;MEM32(0x7844ac)=(uint32_t)strtoul(argv[1],NULL,0);
 g_eax=0x10203040;g_ebx=0x11223344;g_ecx=0x50607080;g_edx=0x60708090;g_ebp=g_seh_ebp=0x22334455;g_esi=0x33445566;g_edi=0x44556677;
 unsigned mxcsr=0x1f80;__asm__ volatile("ldmxcsr %0"::"m"(mxcsr));g_fp_top=0;g_fp_control_word=0x37f;
 if(atoi(argv[4])){MEM32(0x7ba26c)=0x10000600;MEM32(0x10000600)=1;MEM32(0x7844a8)=0x10000400;}
 for(size_t i=0;i<sizeof(sections)/sizeof(sections[0]);i++){
  uint32_t va=sections[i][0],len=sections[i][1];size_t lo=va&~4095u,hi=(va+len+4095u)&~4095u;
  if(mprotect(base+lo,hi-lo,PROT_READ))return 10;
 }
 uint32_t writable_pages[]={0x701000,0x778000,0x784000,0x7ba000};
 for(size_t i=0;i<4;i++)if(mprotect(base+writable_pages[i],4096,PROT_READ|PROT_WRITE))return 11;
 sub_002FB4C0();
 uint32_t regs[]={g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi,g_ebp,g_esp};
 FILE *out=fopen(argv[3],"wb");if(!out)return 9;
 fwrite(regs,1,sizeof(regs),out);fwrite(base+0x10000000,1,0x1000,out);fwrite(base+0x20000000,1,0x2000,out);uint32_t pages[]={0x701000,0x778000,0x784000,0x7ba000};for(size_t i=0;i<4;i++)fwrite(base+pages[i],1,4096,out);fclose(out);
 printf("RETURN eax=%08x ecx=%08x edx=%08x esp=%08x fp_top=%d\n",g_eax,g_ecx,g_edx,g_esp,g_fp_top);
 return 0;
}
