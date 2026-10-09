/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary declared callee double; no original execution.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures, calls, first_null;
#define STACK 0x10000u
#define ACTOR 0x20000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void t1479_candidate_3158c0(void);

static void actor_callee(void)
{
    const uint32_t returns[] = {0x3158D7u, 0x315919u, 0x315942u};
    CHECK(calls < 3u && g_esp == STACK - 8u);
    CHECK(guest_read32(g_esp) == returns[calls]);
    CHECK(game_stack_arg(0u) == 0x30000u + calls * 0x100u);
    if (calls == 0u) CHECK(g_eax == 0u && g_ecx == 0x30000u);
    if (calls == 1u) CHECK(g_eax == 0x30100u && g_edx == 0x1E0u && g_ecx == 0u);
    if (calls == 2u) CHECK(g_eax == ACTOR && g_ecx == 0x3C0u && g_edx == 0x30200u);
    g_eax = calls == 0u && first_null ? 0u : ACTOR;
    g_ecx = 0xEC123400u + calls;
    g_edx = 0xED123400u + calls;
    calls++;
    guest_write32(0x7BA9E0u, calls);
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x77470u);
    return va == 0x77470u ? actor_callee : NULL;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned mode = 0u; mode < 2u; mode++)
        for (unsigned status = 0u; status < 2u; status++)
            for (unsigned flags = 0u; flags < 4u; flags++)
                for (unsigned null_actor = 0u; null_actor < 2u; null_actor++) {
                    memset(memory, 0, sizeof(memory)); calls = 0u; first_null = null_actor;
                    g_esp = STACK;
                    guest_write32(STACK, 0x10101010u);
                    guest_write32(0x783B94u, mode);
                    for (uint32_t i = 0u; i < 3u; i++)
                        guest_write32(0x7BA26Cu + 0x1E0u * i, 0x30000u + 0x100u * i);
                    guest_write32(ACTOR + 0x1B0u, status ? 4u : 0u);
                    guest_write8(ACTOR + 0xC4u, (uint8_t)(flags * 0x10u));
                    t1479_candidate_3158c0();
                    const unsigned expected_calls = mode ? 1u : flags & 1u ? 2u : 3u;
                    const uint32_t result = mode ? (!null_actor && status ? 2u : 0xFFFFFFFEu)
                        : flags & 1u ? 0u : flags & 2u ? 1u : 0xFFFFFFFFu;
                    CHECK(g_eax == result && calls == expected_calls);
                    CHECK(g_esp == STACK && guest_read32(STACK) == 0x10101010u);
                    CHECK(g_edx == 0xED123400u + expected_calls - 1u);
                    CHECK(g_ecx == (mode ? 1u : expected_calls == 2u
                        ? 0xEC123400u | (flags * 0x10u) : 0xEC123402u));
                }
    printf("T1479 actor ordinary controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
