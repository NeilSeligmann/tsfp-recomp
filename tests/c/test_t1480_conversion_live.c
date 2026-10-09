/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic live ABI controls; original proof remains separate. */
#define _GNU_SOURCE
#include "game_replace.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x1000000];
static unsigned checks, failures, calls, root_index;
static uint32_t entry_sp, character, mask, return_pc, cleanup, restored_esi;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
void sub_003CD157(void); void sub_003CAD32(void); void sub_003CF214(void);

static void leaf_spy(void)
{
    calls++;
    CHECK(g_esp == entry_sp - (root_index == 0u ? 12u : 16u));
    CHECK(guest_read32(g_esp) == return_pc);
    CHECK(game_stack_arg(0u) == character && game_stack_arg(1u) == mask);
    CHECK(g_eax == (root_index == 0u ? character : 0xEA112233u));
    CHECK(g_ecx == 0xEC112233u && g_edx == 0xED112233u);
    CHECK(g_esi == (root_index == 0u ? 6u : character));
    if (root_index != 0u) CHECK(guest_read32(entry_sp - 4u) == 6u);
    g_ecx = character + 1u;
    g_eax = 0u;
    if (g_ecx <= 256u) {
        uint16_t word;
        g_ecx = guest_read32(0x54D7F0u);
        memcpy(&word, game_host_ptr(g_ecx + 2u * character), sizeof word);
        g_eax = word & mask;
    }
    /* These mutations are caller-ABI controls, not claimed original leaf behavior. */
    guest_write32(entry_sp - (root_index == 0u ? 4u : 8u), cleanup);
    if (root_index != 0u) guest_write32(entry_sp - 4u, restored_esi);
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
    int affinity_status = sched_getaffinity(0, sizeof affinity, &affinity);
    printf("T1480-NATIVE-RESOURCE pid=%ld tid=%ld NI=%d CPU=%d affinity_status=%d cores=",
           (long)getpid(), (long)gettid(), getpriority(PRIO_PROCESS, 0), sched_getcpu(),
           affinity_status);
    if (affinity_status == 0) {
        for (unsigned cpu = 0u; cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &affinity)) printf("%u,", cpu);
    }
    printf("\n");
    static const uint32_t locales[] = {0u,1u,2u,0x7FFFFFFFu,0x80000000u,0xFFFFFFFFu};
    static const uint32_t chars[] = {0xFFFFFFFEu,0xFFFFFFFFu,0u,65u,255u,256u,257u,0x80000000u,0x7FFFFFFFu};
    static const uint16_t patterns[] = {0u,1u,2u,3u,8u,0x100u};
    void (*roots[])(void) = {sub_003CD157,sub_003CAD32,sub_003CF214};
    const uint32_t masks[] = {8u,2u,1u};
    const uint32_t pcs[] = {0x3CD176u,0x3CAD48u,0x3CF22Au};
    const uint32_t deltas[] = {0u,0xFFFFFFE0u,0x20u};
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    entry_sp = 0xE80000u;
    for (root_index = 0u; root_index < 3u; root_index++) {
        mask = masks[root_index]; return_pc = pcs[root_index];
        for (unsigned locale = 0u; locale < 6u; locale++) {
            for (unsigned ch = 0u; ch < 9u; ch++) {
                for (unsigned pat = 0u; pat < 6u; pat++) {
                    character = chars[ch]; cleanup = mask ^ 0xA500u;
                    restored_esi = 0x5A112233u;
                    g_esp = entry_sp; g_eax = 0xEA112233u; g_ecx = 0xEC112233u;
                    g_edx = 0xED112233u; g_ebx = 3u; g_ebp = 5u; g_esi = 6u; g_edi = 7u;
                    guest_write32(entry_sp, 0xAB112233u);
                    guest_write32(entry_sp + 4u, character);
                    guest_write32(0x54D7F0u, 0xD50004u);
                    guest_write32(0x54D7F8u, locales[locale]);
                    for (unsigned word = 0u; word < 260u; word++)
                        memcpy(game_host_ptr(0xD50000u + word * 2u), &patterns[pat], 2u);
                    unsigned before = calls;
                    roots[root_index]();
                    int early = root_index == 0u && character > 255u;
                    int live = !early && (int32_t)locales[locale] > 1;
                    uint32_t class_bits = patterns[pat] & mask;
                    if (early || (live && character + 1u > 256u)) class_bits = 0u;
                    uint32_t expected = root_index == 0u ? class_bits :
                        (class_bits != 0u ? character + deltas[root_index] : character);
                    CHECK(calls == before + (unsigned)live && g_eax == expected);
                    uint32_t ecx_expected = early || (!live && root_index != 0u) ?
                        0xEC112233u : (live ? cleanup : 0xD50004u);
                    CHECK(g_ecx == ecx_expected);
                    CHECK(g_edx == 0xED112233u && g_esp == entry_sp + 4u);
                    CHECK(g_esi == (live && root_index != 0u ? restored_esi : 6u));
                    CHECK(g_ebx == 3u && g_ebp == 5u && g_edi == 7u);
                    CHECK(guest_read32(entry_sp) == 0xAB112233u);
                }
            }
        }
    }
    /* Fallback observes the freshly spilled ESI, not the old stack bytes. */
    for (root_index = 1u; root_index < 3u; root_index++) {
        for (unsigned incoming = 0u; incoming < 4u; incoming++) {
            g_esp = entry_sp; g_esi = incoming; g_ecx = 0xEC112233u;
            guest_write32(entry_sp + 4u, 65u);
            guest_write32(entry_sp - 4u, 0xFFFFFFFFu);
            guest_write32(0x54D7F8u, 0x80000000u);
            guest_write32(0x54D7F0u, entry_sp - 4u - 130u);
            roots[root_index]();
            CHECK(g_eax == ((incoming & masks[root_index]) ? 65u + deltas[root_index] : 65u));
            CHECK(g_esi == incoming && g_ecx == 0xEC112233u);
        }
    }
    unsigned char *arena = mmap(NULL, 0x1001000u, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(arena != MAP_FAILED);
    if (arena != MAP_FAILED) {
        g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)arena;
        CHECK(mprotect(arena + 0x1000000u, 0x1000u, PROT_NONE) == 0);
        guest_write32(entry_sp, 0xAB112233u);
        guest_write32(entry_sp + 4u, 0u);
        guest_write32(0x54D7F0u, 0xFFFFFFu);
        guest_write32(0x54D7F8u, 0x80000000u);
        guest_write8(0xFFFFFFu, 0xFFu);
        for (root_index = 0u; root_index < 3u; root_index++) {
            pid_t child = fork();
            CHECK(child >= 0);
            if (child == 0) {
                g_esp = entry_sp; g_ecx = 0xEC112233u; g_esi = 6u;
                roots[root_index]();
                uint32_t expected = root_index == 0u ? 8u : deltas[root_index];
                _exit(g_eax == expected && g_esi == 6u ? 0 : 1);
            }
            if (child > 0) {
                int status;
                CHECK(waitpid(child, &status, 0) == child);
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
        }
        /* The unsigned early guard must not touch either locale/table global. */
        CHECK(mprotect(arena + 0x54D000u, 0x1000u, PROT_NONE) == 0);
        guest_write32(entry_sp + 4u, 0xFFFFFFFFu);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            g_esp = entry_sp; g_ecx = 0xEC112233u; g_esi = 6u;
            sub_003CD157();
            _exit(g_eax == 0u && g_ecx == 0xEC112233u && g_esi == 6u ? 0 : 1);
        }
        if (child > 0) {
            int status;
            CHECK(waitpid(child, &status, 0) == child);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        munmap(arena, 0x1001000u);
    }
    printf("T1480 conversion ABI: %u checks, %u calls, %u failures\n", checks, calls, failures);
    return failures ? 1 : 0;
}
