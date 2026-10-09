#define _GNU_SOURCE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_seh_ebp;
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
void sub_0042811B(void);
void sub_00413640(void){g_eax=0;g_esp+=12;}
void sub_00413677(void){_exit(87);}
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc,uint32_t va){fprintf(stderr,"unresolved %s at %08x\n",cc,va);_exit(86);}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 unsigned char *m=mmap(NULL,0x8000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(m==MAP_FAILED)return 2;
 memset(m,0xa5,0x8000);g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)m;
 MEM32(0x5800)=0x6000;MEM32(0x5804)=strtoul(argv[1],NULL,0);MEM32(0x5818)=strtoul(argv[2],NULL,0);MEM32(0x5820)=0x2080;
 g_esp=0x5800;g_ecx=0x2300;g_ebp=0x77777777;g_ebx=0x44444444;g_esi=0x55555555;g_edi=0x66666666;
 sub_0042811B();
 if(g_esp!=0x5824||g_ebp!=0x77777777||g_ebx!=0x44444444||g_esi!=0x55555555||g_edi!=0x66666666)return 3;
 printf("eax=%08x esp=%08x\n",g_eax,g_esp);return 0;
}
