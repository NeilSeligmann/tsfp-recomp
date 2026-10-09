/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone environment for the T1477 appendix native controls. It mirrors the
 * prepare/invoke/CHECK interface of tests/c/test_game_replacements.c so that each
 * control file also works embedded in that test. Only for the pre-proof drafts and the
 * per-root runner (tools/t1477_appendix_native.py). */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00810000];
static unsigned checks, failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        checks++;                                                             \
        if (!(expression)) {                                                  \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void prepare(void)
{
    g_eax = 0x11111111u;
    g_ecx = 0x22222222u;
    g_edx = 0x33333333u;
    g_esp = 0x100u;
    guest_write32(g_esp, 0x12345678u);
}

static void invoke(uint32_t va, uint32_t expected_pop)
{
    size_t count;
    const game_replacement *table = game_replacement_table(&count);
    uint32_t stack = g_esp;
    for (size_t i = 0; i < count; i++) {
        if (table[i].va == va) {
            uint64_t calls = table[i].calls;
            table[i].adapter();
            CHECK(table[i].calls == calls + 1u);
            CHECK(g_esp == stack + expected_pop);
            return;
        }
    }
    CHECK(0);
}
