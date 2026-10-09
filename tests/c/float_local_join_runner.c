/* T1164 patch 36 runtime half: argv = ecx x0 x1 x2 x3, prints eax (1 fall-through, 2 jb taken). */
#define _GNU_SOURCE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
void sub_00012000(void){g_esp+=4;}
void sub_00011000(void);
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc,uint32_t va){(void)cc;(void)va;_exit(86);}
int main(int argc,char **argv){
    if(argc!=6)return 2;
    unsigned char memory[4096]={0};g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;g_esp=2048;
    g_ecx=(uint32_t)atoi(argv[1]);g_xmm0.f[0]=(float)atof(argv[2]);g_xmm1.f[0]=(float)atof(argv[3]);
    g_xmm2.f[0]=(float)atof(argv[4]);g_xmm3.f[0]=(float)atof(argv[5]);
    sub_00011000();printf("%u\n",g_eax);return 0;
}
