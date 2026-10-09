/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit callback fixture; traversal, rotations and erase are genuine generated bodies. */
#define _GNU_SOURCE
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
int g_force_return;
uint32_t g_xbox_code_lo = 0xA00000u, g_xbox_code_hi = 0xA10000u;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
volatile uint64_t g_icall_count;
void recomp_original_003F36F1(void);
#define MAP 0x405B08u
#define CALLBACK 0xA0B100u
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); exit(1); } } while (0)
static unsigned called, mutate;
static uint32_t observed[4];
static void callback(void)
{
    CHECK(called++ == 0u);
    observed[0] = g_ecx; observed[1] = MEM32(g_esp + 4u);
    observed[2] = MEM32(MAP + 8u); observed[3] = MEM32(MAP + 12u);
    CHECK(observed[1] == 1u);
    if (mutate) MEM32(MAP + 12u) = 13u;
    g_eax = MEM32(g_ecx + 4u); g_esp += 8u;
}
recomp_func_t recomp_lookup_manual(uint32_t address) { return address == CALLBACK ? callback : NULL; }
recomp_func_t recomp_lookup(uint32_t address) { (void)address; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t address) { (void)address; return NULL; }
RECOMP_NORETURN void recomp_icall_unresolved_trap(uint32_t address, const char *kind)
{ printf("FAIL unresolved %08x %s\n", address, kind); exit(1); }
static void bytes(uint32_t address, unsigned size)
{
    for (unsigned i = 0; i < size; i++) printf("%02x", *(const unsigned char *)(uintptr_t)(address + i));
    putchar('\n');
}
static void fault(int signal_number)
{
    /* Async-signal-safe fixed-size evidence; no recovery or forced guest success. */
    char output[36];
    const char hex[] = "0123456789abcdef";
    uint32_t values[4] = {(uint32_t)signal_number, MEM32(0xA07000u), MEM32(MAP + 8u), MEM32(MAP + 12u)};
    for (unsigned i=0;i<4u;i++) {
        for (unsigned j=0;j<8u;j++) output[i*9u+j]=hex[(values[i] >> (28u-4u*j)) & 15u];
        output[i*9u+8u]=(i == 3u) ? '\n' : ' ';
    }
    (void)write(STDOUT_FILENO,output,sizeof(output));
    _exit(0);
}
int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    g_ecx = (uint32_t)strtoul(argv[1], NULL, 16);
    mutate = (unsigned)strtoul(argv[2], NULL, 10);
    const uint32_t bases[] = {0x405000u, 0xA00000u, 0xB00000u};
    const unsigned sizes[] = {4096u, 0x10000u, 0x100000u};
    for (unsigned i = 0; i < 3u; i++)
        CHECK(mmap((void *)(uintptr_t)bases[i], sizes[i], PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)bases[i]);
    const uint32_t addresses[] = {0xA07000u,0xA08000u,MAP,0xA0B000u};
    const unsigned lengths[] = {512u,1024u,64u,512u};
    for (unsigned i = 0; i < 4u; i++)
        CHECK(fread((void *)(uintptr_t)addresses[i],1u,lengths[i],stdin) == lengths[i]);
    g_eax=0x13579BDFu; g_edx=0xD00DFEEDu; g_ebx=0x11223344u;
    g_esi=0x2468ACE0u; g_edi=0x87654321u; g_esp=0xBFEFFCu;
    MEM32(g_esp)=0x00980000u;
    if (argc == 4) {
        unsigned mode=(unsigned)strtoul(argv[3],NULL,10);
        struct sigaction action = {0};
        action.sa_handler=fault;
        CHECK(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGSEGV,&action,NULL) == 0);
        if (mode == 1u) CHECK(mprotect((void *)(uintptr_t)0xA07000u,4096u,PROT_READ) == 0);
        else if (mode == 2u) MEM32(MAP + 8u)=0xDEAD0000u;
        else if (mode == 3u) MEM32(0xA08000u)=0xDEAD0000u;
        else CHECK(0);
    }
    recomp_original_003F36F1();
    CHECK(argc == 3); /* fault controls must genuinely fault */
    printf("%08x %08x %08x %08x %08x %08x %08x %08x\n",
        g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp);
    for (unsigned i=0;i<4u;i++) bytes(addresses[i],lengths[i]);
    bytes(0xBFEE00u,0x200u);
    printf("%u %08x %08x %08x %08x\n",called,observed[0],observed[1],observed[2],observed[3]);
    return 0;
}
