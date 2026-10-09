/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minimal guest mapping for the real replacement adapter; no guest runtime admission.
 */
#include "game_replace.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
void sub_003C94F0(void);

int main(int argc, char **argv)
{
    if (argc != 3 || sysconf(_SC_PAGESIZE) != 4096) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    uint32_t header[2];
    if (!in || !out || fread(header, sizeof(header), 1, in) != 1 ||
        header[0] != 0x54434D50u || header[1] == 0 || header[1] > 1024) return 2;
    for (uint32_t i = 0; i < header[1]; i++) {
        uint32_t args[3];
        unsigned char first[4096], second[4096], stack[4096];
        if (fread(args, sizeof(args), 1, in) != 1 || args[0] > 64 ||
            fread(first, sizeof(first), 1, in) != 1 ||
            fread(second, sizeof(second), 1, in) != 1 ||
            fread(stack, sizeof(stack), 1, in) != 1) return 2;
        uint32_t frame[4];
        memcpy(frame, stack + 0x800, sizeof(frame));
        if (frame[0] != 0x7000 || frame[1] != args[1] ||
            frame[2] != args[2] || frame[3] != args[0]) return 2;
        if (args[0] == 0) {
            if (args[1] != 0x6000 || args[2] != 0x6000) return 2;
        } else {
            if (args[1] < 0x1000 || args[1] >= 0x2000 ||
                !((args[2] >= 0x1000 && args[2] < 0x2000) ||
                  (args[2] >= 0x3000 && args[2] < 0x4000))) return 2;
        }
        unsigned char *base = mmap(NULL, 7u * 4096u, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) return 2;
        g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)base;
        for (unsigned p = 1; p <= 5; p += 2) {
            if (mprotect(base + p * 4096u, 4096, PROT_READ | PROT_WRITE)) return 2;
        }
        memcpy(base + 0x1000, first, 4096);
        memcpy(base + 0x3000, second, 4096);
        memcpy(base + 0x5000, stack, 4096);
        if (mprotect(base + 0x1000, 4096, PROT_READ) ||
            mprotect(base + 0x3000, 4096, PROT_READ)) return 2;
        g_eax = 0x11111111u; g_ecx = 0x22222222u;
        g_edx = 0x33333333u; g_esp = 0x5800;
        sub_003C94F0();
        uint32_t record[4] = {g_eax, g_ecx, g_edx, g_esp};
        if (memcmp(base + 0x1000, first, 4096) ||
            memcmp(base + 0x3000, second, 4096) ||
            memcmp(base + 0x5000, stack, 4096)) return 3;
        if (fwrite(record, sizeof(record), 1, out) != 1 || munmap(base, 7u * 4096u)) return 2;
    }
    if (fgetc(in) != EOF || ferror(in) || fclose(in) || fclose(out)) return 2;
    return 0;
}
