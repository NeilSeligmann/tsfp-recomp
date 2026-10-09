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
static uint32_t allocation;
static unsigned calls;
void sub_0042811B(void);
/* Chosen two-argument seams, not real allocator/free implementations. */
void sub_00380C53(void) { g_eax = ++calls == 1 ? allocation : 0; g_esp += 12; }
void sub_00380CF3(void) { g_eax = 0; g_esp += 12; }
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc,uint32_t va) {
    fprintf(stderr,"unresolved %s at %08x\n",cc,va);_exit(86);
}
int main(int argc,char **argv) {
    if(argc != 5) return 2;
    FILE *out=fopen(argv[4],"wb"); if(!out) return 2;
    unsigned char *mem=mmap(NULL,0x8000,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(mem==MAP_FAILED) return 2;
    for(unsigned n=0;n<3;n++) {
        unsigned page=(unsigned[]){2,3,5}[n];
        if(mprotect(mem+page*4096,4096,PROT_READ|PROT_WRITE))return 2;
        memset(mem+page*4096,0xa5,4096);
    }
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)mem;
    uint32_t args[9]={0x6000,strtoul(argv[1],NULL,0),3,4,0x12345678,0x87654321,strtoul(argv[2],NULL,0),9,0x2080};
    memcpy(mem+0x5800,args,sizeof(args)); allocation=strtoul(argv[3],NULL,0);
    g_eax=0x11111111;g_ecx=0x2300;g_edx=0x33333333;g_esp=0x5800;
    g_ebp=0x77777777;g_ebx=0x44444444;g_esi=0x55555555;g_edi=0x66666666;
    RecompXmm *xmms[8]={&g_xmm0,&g_xmm1,&g_xmm2,&g_xmm3,&g_xmm4,&g_xmm5,&g_xmm6,&g_xmm7};
    for(unsigned n=0;n<8;n++){memset(xmms[n],0,16);xmms[n]->u[0]=n+123;}
    _mm_setcsr(0x1f80);sub_0042811B();
    uint32_t regs[8]={g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi};
    if(fwrite(regs,sizeof(regs),1,out)!=1)return 2;
    for(unsigned n=0;n<8;n++)if(fwrite(xmms[n],16,1,out)!=1)return 2;
    uint32_t mxcsr=_mm_getcsr();if(fwrite(&mxcsr,4,1,out)!=1)return 2;
    for(unsigned n=0;n<3;n++){unsigned page=(unsigned[]){2,3,5}[n];if(fwrite(mem+page*4096,4096,1,out)!=1)return 2;}
    if(fclose(out)||munmap(mem,0x8000))return 2;return 0;
}
