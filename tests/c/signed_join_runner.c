/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
void sub_003A98A0(void);
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc, uint32_t va) {
    fprintf(stderr, "unresolved %s at %08x\n", cc, va);
    _exit(86);
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) return 2;
    uint32_t input[2];
    unsigned cases = 0;
    while (fread(input, sizeof(input), 1, in) == 1) {
        if (++cases > 1024) return 2;
        unsigned char *mem = mmap(NULL, 0x8000u, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return 2;
        for (unsigned n = 0; n < 3; ++n) {
            unsigned p = (unsigned[]){2, 4, 5}[n];
            if (mprotect(mem + p * 4096u, 4096u, PROT_READ | PROT_WRITE)) return 2;
            memset(mem + p * 4096u, 0xa5, 4096u);
        }
        g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;
        for (unsigned i = 0; i < 128; i++) MEMF(0x2000 + input[0] + i * 4) = (float)((int)((i * 13) % 31) - 15);
        MEM32(0x5800) = 0x7000;
        MEM32(0x5804) = 0x2000 + input[0];
        MEM32(0x5808) = input[1];
        MEM32(0x580c) = 0x4080;
        if (mprotect(mem + 0x2000, 4096, PROT_READ)) return 2;
        _mm_setcsr(0x1f80);
        g_eax = 0x11111111; g_ecx = 0x22222222; g_edx = 0x33333333;
        g_ebx = 0x44444444; g_esi = 0x55555555; g_edi = 0x66666666;
        g_ebp = 0x77777777; g_esp = 0x5800;
        RecompXmm *xmms[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3,
                             &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
        for (unsigned n = 0; n < 8; ++n)
            for (unsigned lane = 0; lane < 4; ++lane)
                xmms[n]->u[lane] = 0x3f800000u + n * 0x100u + lane;
        sub_003A98A0();
        uint32_t regs[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
        if (fwrite(regs, sizeof(regs), 1, out) != 1) return 2;
        for (unsigned n = 0; n < 8; ++n)
            if (fwrite(xmms[n]->u, 16, 1, out) != 1) return 2;
        for (unsigned n = 0; n < 3; ++n) {
            unsigned p = (unsigned[]){2, 4, 5}[n];
            if (fwrite(mem + p * 4096u, 4096u, 1, out) != 1) return 2;
        }
        if (munmap(mem, 0x8000u)) return 2;
    }
    if (!feof(in) || ferror(in) || fclose(in) || fclose(out)) return 2;
    return 0;
}
