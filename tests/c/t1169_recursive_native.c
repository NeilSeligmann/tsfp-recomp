/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual generated cleanup plus explicit synthetic callback observations. */
#define _GNU_SOURCE
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
int g_force_return;
uint32_t g_xbox_code_lo=0xA00000u,g_xbox_code_hi=0xA10000u;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_trace_idx;
volatile uint64_t g_icall_count;
void recomp_original_003F17AE(void);
#define OBJECT 0xA07000u
#define ROOT (OBJECT+0x100u)
#define LEFT (OBJECT+0x140u)
#define REPLACEMENT (OBJECT+0x1C0u)
#define CALLBACK (OBJECT+0x400u)
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n",__LINE__,#c); exit(1); } } while(0)
static unsigned mode,count;
static uint32_t observed[8][5];
static void callback(void)
{
    CHECK(count<8u && MEM32(g_esp+4u)==1u);
    uint32_t *record=observed[count++];
    record[0]=g_ecx; record[1]=MEM32(g_esp+4u); record[2]=MEM32(g_ecx+8u);
    record[3]=MEM32(OBJECT+8u); record[4]=MEM32(OBJECT+12u);
    if(g_ecx==LEFT) {
        if(mode==1u) MEM32(ROOT+16u)=REPLACEMENT;
        if(mode==2u) MEM32(ROOT+8u)=0x11223344u;
        if(mode==3u) { MEM32(OBJECT+8u)=REPLACEMENT; MEM32(OBJECT+12u)=123u; }
    }
    g_eax=MEM32(g_ecx+4u); g_esp+=8u;
}
recomp_func_t recomp_lookup_manual(uint32_t address) { return address==CALLBACK ? callback : NULL; }
recomp_func_t recomp_lookup(uint32_t address) { (void)address; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t address) { (void)address; return NULL; }
RECOMP_NORETURN void recomp_icall_unresolved_trap(uint32_t address,const char *kind)
{ printf("FAIL unresolved %08x %s\n",address,kind); exit(1); }
static void bytes(uint32_t address,unsigned size)
{
    for(unsigned i=0;i<size;i++) printf("%02x",*(const unsigned char *)(uintptr_t)(address+i));
    putchar('\n');
}
static void fault(int signal_number)
{
    static char output[0xA01u];
    const char hex[]="0123456789abcdef";
    (void)signal_number;
    for(unsigned i=0;i<0x500u;i++) {
        unsigned byte=*(const unsigned char *)(uintptr_t)(OBJECT+i);
        output[2u*i]=hex[byte>>4]; output[2u*i+1u]=hex[byte&15u];
    }
    output[0xA00u]='\n';
    (void)write(STDOUT_FILENO,output,sizeof(output));
    _exit(0); /* evidence only: no recovery or fabricated guest completion */
}
int main(int argc,char **argv)
{
    CHECK(argc==2 || argc==3);
    mode=(unsigned)strtoul(argv[1],NULL,10);
    CHECK(mode<=3u);
    CHECK(mmap((void *)(uintptr_t)0xA00000u,0x10000u,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void *)(uintptr_t)0xA00000u);
    CHECK(mmap((void *)(uintptr_t)0xB00000u,0x100000u,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void *)(uintptr_t)0xB00000u);
    CHECK(fread((void *)(uintptr_t)OBJECT,1u,0x500u,stdin)==0x500u);
    g_eax=0x13579BDFu; g_ecx=OBJECT; g_edx=0xD00DFEEDu;
    g_ebx=0x11223344u; g_esi=0x2468ACE0u; g_edi=0x87654321u; g_esp=0xBFEFFCu;
    MEM32(g_esp)=0x00980000u;
    if(argc==3) {
        unsigned fault_mode=(unsigned)strtoul(argv[2],NULL,10);
        struct sigaction action={0}; action.sa_handler=fault;
        CHECK(sigemptyset(&action.sa_mask)==0 && sigaction(SIGSEGV,&action,NULL)==0);
        if(fault_mode==1u) CHECK(mprotect((void *)(uintptr_t)OBJECT,4096u,PROT_READ)==0);
        else if(fault_mode==2u) MEM32(ROOT+12u)=0xDEAD0000u;
        else if(fault_mode==3u) MEM32(ROOT)=0xDEAD0000u;
        else CHECK(0);
    }
    recomp_original_003F17AE();
    CHECK(argc==2); /* fault cases must genuinely fault */
    printf("%08x %08x %08x %08x %08x %08x %08x %08x\n",
        g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp);
    bytes(OBJECT,0x500u); bytes(0xBFEE00u,0x200u);
    printf("%u\n",count);
    for(unsigned i=0;i<count;i++) printf("%08x %08x %08x %08x %08x\n",
        observed[i][0],observed[i][1],observed[i][2],observed[i][3],observed[i][4]);
    return 0;
}
