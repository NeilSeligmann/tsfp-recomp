/* SPDX-License-Identifier: GPL-3.0-or-later
 * Mocked ordinary frames only; original helpers are not executed.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures, calls;
static uint32_t key, first_result, second_result;
#define STACK 0x10000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void t1479_candidate_2fe520(void);

static void first_callee(void)
{
    CHECK(calls == 0u && g_esp == STACK - 16u);
    CHECK(guest_read32(g_esp) == 0x2FE52Du);
    CHECK(game_stack_arg(0u) == 0u && game_stack_arg(1u) == key);
    CHECK(g_eax == 0xABCDEF01u && g_esi == key);
    g_eax = first_result;
    g_ecx = 0xEC000001u;
    g_edx = 0xED000001u;
    g_esp += 4u;
    calls++;
}
static void second_callee(void)
{
    CHECK(calls == 1u && g_esp == STACK - 12u);
    CHECK(guest_read32(g_esp) == 0x2FE53Au);
    CHECK(game_stack_arg(0u) == key && g_esi == key && g_eax == 0u);
    CHECK(g_ecx == 0xEC000001u && g_edx == 0xED000001u);
    g_eax = second_result;
    g_ecx = 0xEC000002u;
    g_edx = 0xED000002u;
    g_esp += 4u;
    calls++;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == (calls == 0u ? 0x2FE480u : 0x2FE4E0u));
    return va == 0x2FE480u ? first_callee : va == 0x2FE4E0u ? second_callee : NULL;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t keys[] = {0xFFFF8000u, 0xFFFFFFFFu, 0u, 32767u, 32768u};
    const uint32_t seconds[] = {0x80000000u, 0xFFFFFFFFu, 0u, 0x7FFFFFFFu};
    for (unsigned k = 0u; k < 5u; k++)
        for (unsigned hit = 0u; hit < 2u; hit++)
            for (unsigned result = 0u; result < 4u; result++)
                for (unsigned mode = 0u; mode < 2u; mode++)
                    for (unsigned match = 0u; match < 2u; match++) {
                        memset(memory, 0, sizeof(memory)); calls = 0u;
                        g_esp = STACK; g_eax = 0xABCDEF01u; g_esi = 0x12345678u;
                        key = keys[k]; first_result = hit ? 0x20000u : 0u;
                        second_result = seconds[result];
                        guest_write32(STACK, 0x10101010u);
                        guest_write32(STACK + 4u, key);
                        guest_write32(0x7844A8u, 0x30000u);
                        guest_write32(0x32F38u, match ? key : key ^ 1u);
                        guest_write32(0x32F34u, mode ? 0x46u : 0xFFFFFFFFu);
                        t1479_candidate_2fe520();
                        CHECK(g_eax == (hit || result >= 2u || (mode && match) ? 1u : 0u));
                        CHECK(calls == (hit ? 1u : 2u));
                        CHECK(g_esp == STACK && g_esi == 0x12345678u);
                        CHECK(g_ecx == (hit ? 0xEC000001u : 0xEC000002u));
                        CHECK(g_edx == (hit ? 0xED000001u : 0xED000002u));
                        CHECK(guest_read32(STACK) == 0x10101010u);
                    }
    printf("T1479 profile ordinary controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
