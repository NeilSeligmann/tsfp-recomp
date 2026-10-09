/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic callback fixture around genuine generated entry/helper bodies. */
#define _GNU_SOURCE
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
int g_force_return;
uint32_t g_xbox_code_lo = 0xA00000u, g_xbox_code_hi = 0xA10000u;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
volatile uint64_t g_icall_count;
void recomp_original_003F22B4(void);

#define OBJECT 0xA07000u
#define ROOT (OBJECT + 0x100u)
#define LEFT (OBJECT + 0x140u)
#define RIGHT (OBJECT + 0x180u)
#define REPLACEMENT (OBJECT + 0x1C0u)
#define TABLE (OBJECT + 0x300u)
#define CALLBACK (OBJECT + 0x400u)
static unsigned shape, count;
static uint32_t calls[8];
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); exit(1); } } while (0)

static void callback(void)
{
    CHECK(count < 8u);
    CHECK(MEM32(g_esp + 4u) == 1u);
    CHECK(MEM32(g_ecx + 12u) == 0u);
    CHECK(MEM32(OBJECT + 8u) == ROOT && MEM32(OBJECT + 12u) == 99u);
    calls[count++] = g_ecx;
    if (shape == 5u && g_ecx == LEFT) MEM32(ROOT + 20u) = REPLACEMENT;
    /* Explicit fixture counterpart of MOV EAX,[ECX+4]; RET4. */
    g_eax = MEM32(g_ecx + 4u);
    g_esp += 8u;
}
recomp_func_t recomp_lookup_manual(uint32_t address) { return address == CALLBACK ? callback : NULL; }
recomp_func_t recomp_lookup(uint32_t address) { (void)address; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t address) { (void)address; return NULL; }
RECOMP_NORETURN void recomp_icall_unresolved_trap(uint32_t address, const char *kind)
{
    printf("FAIL unresolved %08x %s\n", address, kind);
    exit(1);
}
static void bytes(uint32_t address, unsigned size)
{
    for (unsigned i = 0u; i < size; i++) printf("%02x", *(const unsigned char *)(uintptr_t)(address + i));
    putchar('\n');
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    shape = (unsigned)strtoul(argv[1], NULL, 10);
    CHECK(shape <= 5u);
    CHECK(mmap((void *)(uintptr_t)0xA00000u, 0x10000u, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)0xA00000u);
    CHECK(mmap((void *)(uintptr_t)0xB00000u, 0x100000u, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)0xB00000u);
    memset((void *)(uintptr_t)OBJECT, 0xA5, 20u);
    const uint32_t nodes[] = {ROOT, LEFT, RIGHT, REPLACEMENT};
    for (unsigned i = 0u; i < 4u; i++) {
        MEM32(nodes[i]) = TABLE;
        MEM32(nodes[i] + 4u) = 0xABCD0001u + i;
        MEM32(nodes[i] + 8u) = 0xFEED0000u;
        MEM32(nodes[i] + 12u) = 0xFEED0000u;
        MEM32(nodes[i] + 16u) = 0u;
        MEM32(nodes[i] + 20u) = 0u;
    }
    MEM32(TABLE) = CALLBACK;
    const unsigned char callback_bytes[] = {0x8B, 0x41, 0x04, 0xC2, 0x04, 0x00};
    memcpy((void *)(uintptr_t)CALLBACK, callback_bytes, sizeof(callback_bytes));
    MEM32(OBJECT + 8u) = shape == 0u ? 0u : ROOT;
    MEM32(OBJECT + 12u) = 99u;
    if (shape == 2u || shape == 4u || shape == 5u) MEM32(ROOT + 16u) = LEFT;
    if (shape == 3u || shape == 4u || shape == 5u) MEM32(ROOT + 20u) = RIGHT;
    g_eax = 0x13579BDFu; g_ecx = OBJECT; g_edx = 0xD00DFEEDu;
    g_ebx = 0x11223344u; g_esi = 0x2468ACE0u; g_edi = 0x87654321u;
    g_esp = 0xBFEFFCu;
    MEM32(g_esp) = 0x00980000u;
    recomp_original_003F22B4();
    CHECK(g_eax == (shape == 0u ? 0x13579BDFu : 0xABCD0001u));
    CHECK(g_esp == 0xBFF000u && g_esi == 0x2468ACE0u);
    CHECK(MEM32(OBJECT) == 0x004A1C14u && MEM32(OBJECT + 8u) == 0u && MEM32(OBJECT + 12u) == 0u);
    const uint32_t expected[6][3] = {{0u}, {ROOT}, {LEFT, ROOT}, {RIGHT, ROOT},
                                   {LEFT, RIGHT, ROOT}, {LEFT, REPLACEMENT, ROOT}};
    const unsigned lengths[] = {0u, 1u, 2u, 2u, 3u, 3u};
    CHECK(count == lengths[shape]);
    for (unsigned i = 0u; i < count; i++) CHECK(calls[i] == expected[shape][i]);
    printf("%08x %08x %08x %08x %08x %08x %08x %08x\n",
           g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp);
    bytes(OBJECT, 0x500u);
    bytes(0xBFEE00u, 0x200u);
    return 0;
}
