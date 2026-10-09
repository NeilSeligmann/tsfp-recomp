/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic dispatch ABI controls; not retail equivalence evidence.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebp, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x800000];
static unsigned checks, failures, calls;
static uint32_t entry_sp, pointer, length, index_value, leaf_result;
static int equality, change_expected;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
void sub_00150560(void);
void sub_00115F10(void);

static void leaf_spy(void)
{
    calls++;
    CHECK(g_esp == entry_sp - (equality ? 12u : 8u));
    CHECK(guest_read32(g_esp) == (equality ? 0x15056Fu : 0x115F2Du));
    if (equality) {
        CHECK(game_stack_arg(0u) == pointer && game_stack_arg(1u) == length);
        CHECK(g_eax == length && g_ecx == pointer && g_edx == 0xED112233u);
        if (change_expected) guest_write32(entry_sp + 12u, leaf_result);
    } else {
        CHECK(game_stack_arg(0u) == index_value && g_eax == index_value);
        CHECK(g_ecx == 0xEC112233u && g_edx == 0xED112233u);
    }
    g_eax = leaf_result;
    g_ecx = 0xEC445566u;
    g_edx = 0xED445566u;
    g_esp += 4u;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == (equality ? 0x1504F0u : 0x356800u));
    return leaf_spy;
}

static void prepare(void)
{
    memset(memory, 0, sizeof memory);
    entry_sp = g_esp = 0x8000u;
    g_eax = 0xEA112233u; g_ecx = 0xEC112233u; g_edx = 0xED112233u;
    g_ebx = 0xEB112233u; g_esi = 0xE5112233u; g_edi = 0xED998877u;
    g_ebp = 0xEB998877u;
    guest_write32(entry_sp, 0xAB123456u);
}

static void preserved(void)
{
    CHECK(g_esp == entry_sp + 4u && guest_read32(entry_sp) == 0xAB123456u);
    CHECK(g_ebx == 0xEB112233u && g_esi == 0xE5112233u);
    CHECK(g_edi == 0xED998877u && g_ebp == 0xEB998877u);
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    equality = 1;
    for (unsigned match = 0; match < 2; match++) {
        for (change_expected = 0; change_expected < 2; change_expected++) {
            prepare(); pointer = 0x1000u; length = 7u; leaf_result = 0x12345678u;
            guest_write32(entry_sp + 4u, pointer);
            guest_write32(entry_sp + 8u, length);
            guest_write32(entry_sp + 12u, leaf_result ^ match);
            unsigned before = calls;
            sub_00150560();
            CHECK(calls == before + 1u);
            CHECK(g_eax == (change_expected || !match));
            CHECK(g_ecx == 0xEC445566u && g_edx == 0xED445566u);
            preserved();
        }
    }
    equality = 0;
    for (unsigned row = 0; row < 4; row++) {
        for (unsigned remapped = 0; remapped < 2; remapped++) {
            prepare(); index_value = row; leaf_result = remapped ? 3u - row : row;
            guest_write32(entry_sp + 4u, 202u + row);
            guest_write32(0x7B0C48u, 0x10000u);
            for (unsigned neighbor = 0; neighbor < 4; neighbor++) {
                guest_write32(0x10000u + neighbor * 0x1584u + 0x14u, 0x20000u + neighbor * 0x80u);
                guest_write32(0x20000u + neighbor * 0x80u + 0x34u, 11u + 7u * neighbor);
            }
            unsigned before = calls;
            sub_00115F10();
            CHECK(calls == before + 1u && g_eax == 11u + 7u * leaf_result);
            CHECK(g_ecx == 0x10000u && g_edx == 0xED445566u);
            preserved();
        }
    }
    const uint32_t outside[] = {0u, 201u, 206u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu};
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        prepare(); guest_write32(entry_sp + 4u, outside[i]);
        unsigned before = calls;
        sub_00115F10();
        CHECK(calls == before && g_eax == outside[i]);
        CHECK(g_ecx == 0xEC112233u && g_edx == 0xED112233u);
        preserved();
    }
    printf("T1477 follow-up ABI: %u checks, %u calls, %u failures\n", checks, calls, failures);
    return failures ? 1 : 0;
}
