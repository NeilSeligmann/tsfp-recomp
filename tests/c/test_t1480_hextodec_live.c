/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic callee mutations verify caller ABI, not original leaf behavior. */
#define _GNU_SOURCE
#include "game_replace.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x1000000];
static unsigned checks, failures, calls;
static uint32_t character;
static const uint32_t entry_sp = 0xE80000u;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
void sub_003CDA3C(void);

static void leaf_spy(void)
{
    calls++;
    CHECK(g_esp == entry_sp - 16u);
    CHECK(guest_read32(g_esp) == 0x3CDA50u);
    CHECK(game_stack_arg(0u) == character && game_stack_arg(1u) == 4u);
    CHECK(g_eax == character && g_esi == character);
    CHECK(g_ecx == 0xEC112233u && g_edx == 0xED112233u);
    CHECK(guest_read32(entry_sp - 4u) == 6u);
    g_ecx = character + 1u;
    g_eax = 0u;
    if (g_ecx <= 256u) {
        uint16_t word;
        g_ecx = guest_read32(0x54D7F0u);
        memcpy(&word, game_host_ptr(g_ecx + 2u * character), sizeof word);
        g_eax = word & 4u;
    }
    guest_write32(entry_sp - 8u, 0xA5000004u);
    guest_write32(entry_sp - 4u, 0x5A112233u);
    g_esp += 4u;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x3C9C4Eu);
    return leaf_spy;
}

int main(void)
{
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CHECK(sched_getaffinity(0, sizeof affinity, &affinity) == 0);
    printf("T1480-HEX-NATIVE pid=%ld tid=%ld NI=%d CPU=%d cores=",
           (long)getpid(), (long)gettid(), getpriority(PRIO_PROCESS, 0), sched_getcpu());
    for (unsigned cpu = 0u; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &affinity)) printf("%u,", cpu);
    printf("\n");
    static const uint32_t locales[] = {0u,1u,2u,0x7FFFFFFFu,0x80000000u,0xFFFFFFFFu};
    static const uint32_t chars[] = {0xFFFFFFFEu,0xFFFFFFFFu,0u,65u,255u,256u,257u,0x80000000u,0x7FFFFFFFu};
    static const uint16_t patterns[] = {0u,1u,4u,5u,0x100u,0x104u};
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned locale = 0u; locale < 6u; locale++) {
        for (unsigned ch = 0u; ch < 9u; ch++) {
            for (unsigned pat = 0u; pat < 6u; pat++) {
                character = chars[ch];
                g_esp = entry_sp; g_eax = character; g_ecx = 0xEC112233u;
                g_edx = 0xED112233u; g_ebx = 3u; g_ebp = 5u; g_esi = 6u; g_edi = 7u;
                guest_write32(entry_sp, 0xAB112233u);
                guest_write32(entry_sp + 4u, character ^ 0xFFFFFFFFu);
                guest_write32(0x54D7F0u, 0xD50004u);
                guest_write32(0x54D7F8u, locales[locale]);
                for (unsigned word = 0u; word < 260u; word++)
                    memcpy(game_host_ptr(0xD50000u + word * 2u), &patterns[pat], 2u);
                unsigned before = calls;
                sub_003CDA3C();
                int live = (int32_t)locales[locale] > 1;
                uint32_t class_bits = patterns[pat] & 4u;
                if (live && character + 1u > 256u) class_bits = 0u;
                uint32_t expected = class_bits ? character : (character & 0xFFFFFFDFu) - 7u;
                CHECK(calls == before + (unsigned)live && g_eax == expected);
                CHECK(g_ecx == (live ? 0xA5000004u : 0xEC112233u));
                CHECK(g_edx == 0xED112233u && g_esp == entry_sp + 4u);
                CHECK(g_esi == (live ? 0x5A112233u : 6u));
                CHECK(g_ebx == 3u && g_ebp == 5u && g_edi == 7u);
                CHECK(guest_read32(entry_sp) == 0xAB112233u);
                CHECK(guest_read32(entry_sp + 4u) == (character ^ 0xFFFFFFFFu));
            }
        }
    }
    for (unsigned incoming = 0u; incoming <= 5u; incoming++) {
        g_esp = entry_sp; g_esi = incoming; g_eax = 65u; g_ecx = 0xEC112233u;
        guest_write32(entry_sp - 4u, 0xFFFFFFFFu);
        guest_write32(0x54D7F8u, 0x80000000u);
        guest_write32(0x54D7F0u, entry_sp - 4u - 130u);
        sub_003CDA3C();
        CHECK(g_eax == ((incoming & 4u) ? 65u : 58u));
        CHECK(g_esi == incoming && g_ecx == 0xEC112233u);
    }
    unsigned char *arena = mmap(NULL, 0x1001000u, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(arena != MAP_FAILED);
    if (arena != MAP_FAILED) {
        g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)arena;
        CHECK(mprotect(arena + 0x1000000u, 0x1000u, PROT_NONE) == 0);
        guest_write32(entry_sp, 0xAB112233u);
        guest_write32(0x54D7F0u, 0xFFFFFFu);
        guest_write8(0xFFFFFFu, 0xFFu);
        for (unsigned live = 0u; live < 2u; live++) {
            guest_write32(0x54D7F8u, live ? 2u : 0x80000000u);
            pid_t child = fork();
            CHECK(child >= 0);
            if (child == 0) {
                g_esp = entry_sp; g_eax = 0u; g_esi = 6u;
                g_ecx = 0xEC112233u; g_edx = 0xED112233u; character = 0u;
                sub_003CDA3C();
                _exit(g_eax == 0u && g_esi == 6u ? 0 : 1);
            }
            if (child > 0) {
                int status;
                CHECK(waitpid(child, &status, 0) == child);
                CHECK(live ? WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV :
                             WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
        }
        CHECK(munmap(arena, 0x1001000u) == 0);
    }
    printf("T1480 hextodec native: %u checks, %u calls, %u failures\n", checks, calls, failures);
    return failures != 0u;
}
