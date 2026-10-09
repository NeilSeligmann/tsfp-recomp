/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic ABI/read-width controls, independent of original equivalence receipts. */
#define _GNU_SOURCE
#include "game_replace.h"
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x1000000];
static unsigned checks, failures, calls;
static uint32_t entry_sp, character, mask, return_pc, cleanup;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
void sub_003C887E(void); void sub_003C88AC(void); void sub_003C88D5(void);
void sub_003C88FE(void); void sub_003C892C(void);

static void leaf_spy(void)
{
    calls++;
    CHECK(g_esp == entry_sp - 12u && guest_read32(g_esp) == return_pc);
    CHECK(game_stack_arg(0u) == character && game_stack_arg(1u) == mask);
    CHECK(g_eax == 0xEA112233u && g_ecx == 0xEC112233u && g_edx == 0xED112233u);
    g_ecx = character + 1u;
    g_eax = 0u;
    if (g_ecx <= 256u) {
        uint16_t value;
        g_ecx = guest_read32(0x54D7F0u);
        memcpy(&value, game_host_ptr(g_ecx + 2u * character), sizeof value);
        g_eax = value & mask;
    }
    /* Synthetic mutation validates real pop cleanup, not a fabricated original leaf. */
    guest_write32(entry_sp - 4u, cleanup);
    g_esp += 4u;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x3C9C4Eu);
    return leaf_spy;
}

int main(void)
{
    static const uint32_t locales[] = {0u,1u,2u,0x7FFFFFFFu,0x80000000u,0xFFFFFFFFu};
    static const uint32_t chars[] = {0xFFFFFFFEu,0xFFFFFFFFu,0u,65u,255u,256u,257u,0x80000000u,0x7FFFFFFFu};
    static const uint16_t patterns[] = {0u,0xFFFFu,0x100u};
    void (*roots[])(void) = {sub_003C887E,sub_003C88AC,sub_003C88D5,sub_003C88FE,sub_003C892C};
    const uint32_t masks[] = {0x103u,4u,8u,0x107u,0x157u};
    const uint32_t pcs[] = {0x3C8895u,0x3C88C0u,0x3C88E9u,0x3C8915u,0x3C8943u};
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    entry_sp = 0xE80000u;
    for (unsigned root = 0u; root < 5u; root++) {
        mask = masks[root]; return_pc = pcs[root];
        for (unsigned locale = 0u; locale < 6u; locale++) {
            for (unsigned ch = 0u; ch < 9u; ch++) {
                for (unsigned pat = 0u; pat < 3u; pat++) {
                    character = chars[ch]; cleanup = mask ^ 0xA500u;
                    g_esp = entry_sp; g_eax = 0xEA112233u; g_ecx = 0xEC112233u;
                    g_edx = 0xED112233u; g_ebx = 3u; g_ebp = 5u; g_esi = 6u; g_edi = 7u;
                    guest_write32(entry_sp, 0xAB112233u);
                    guest_write32(entry_sp + 4u, character);
                    guest_write32(0x54D7F0u, 0xD50004u);
                    guest_write32(0x54D7F8u, locales[locale]);
                    for (unsigned word = 0u; word < 260u; word++)
                        memcpy(game_host_ptr(0xD50000u + word * 2u), &patterns[pat], 2u);
                    unsigned before = calls;
                    roots[root]();
                    int live = (int32_t)locales[locale] > 1;
                    uint32_t expected = patterns[pat] & mask;
                    if (live && character + 1u > 256u) expected = 0u;
                    if (!live && (root == 1u || root == 2u)) expected &= 0xFFu;
                    CHECK(calls == before + (unsigned)live && g_eax == expected);
                    CHECK(g_ecx == (live ? cleanup : 0xD50004u));
                    CHECK(g_edx == 0xED112233u && g_esp == entry_sp + 4u);
                    CHECK(g_ebx == 3u && g_ebp == 5u && g_esi == 6u && g_edi == 7u);
                    CHECK(guest_read32(entry_sp) == 0xAB112233u);
                }
            }
        }
    }
    /* Guard the next page: byte predicates must not widen this valid final-byte read. */
    unsigned char *arena = mmap(NULL, 0x1001000u, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(arena != MAP_FAILED);
    if (arena != MAP_FAILED) {
        CHECK(mprotect(arena + 0x1000000u, 0x1000u, PROT_NONE) == 0);
        g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)arena;
        guest_write32(0x54D7F0u, 0xFFFFFFu);
        guest_write8(0xFFFFFFu, 0xFFu);
        guest_write32(entry_sp, 0xAB112233u);
        guest_write32(entry_sp + 4u, 0u);
        for (unsigned root = 0u; root < 5u; root++) {
            for (unsigned highbit = 0u; highbit < 2u; highbit++) {
                guest_write32(0x54D7F8u, highbit ? 0x80000000u : 0u);
                pid_t child = fork();
                CHECK(child >= 0);
                if (child == 0) {
                    g_esp = entry_sp;
                    roots[root]();
                    _exit(g_eax == (0xFFu & masks[root]) && g_ecx == 0xFFFFFFu ? 0 : 1);
                }
                if (child > 0) {
                    int status;
                    CHECK(waitpid(child, &status, 0) == child);
                    if (root == 1u || root == 2u)
                        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                    else
                        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
                }
            }
        }
        munmap(arena, 0x1001000u);
    }
    printf("T1480 classifier ABI: %u checks, %u calls, %u failures\n", checks, calls, failures);
    return failures ? 1 : 0;
}
