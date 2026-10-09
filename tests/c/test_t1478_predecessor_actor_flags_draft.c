/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary separate-memory frame tests with an explicit callee double.
 * Never an original oracle; no saved-frame alias or fault probes.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls;
static uint32_t entry_stack, object, selected_index, actor, flags_after_call;
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)
extern void t1478_draft_predecessor_actor_flags(void);

static void controlled_predecessor(void)
{
    CHECK(g_esp == entry_stack - 16u);
    CHECK(guest_read32(g_esp) == 0x001BFA81u);
    CHECK(guest_read32(g_esp + 4u) == object);
    CHECK(guest_read32(g_esp + 8u) == selected_index);
    CHECK(g_eax == object && g_ecx == selected_index);
    CHECK(g_esi == actor);
    guest_write32(object + 0x94u, selected_index);
    guest_write32(object + 8u, 7u);
    guest_write32(0x007B0C48u, 0xDC0000u);
    guest_write32(actor + 12u, flags_after_call);
    g_eax = 0xAAAAAAAAu;
    g_ecx = 0xCCCCCCCCu;
    g_edx = 0xDDDDDDDDu;
    g_esp += 4u;
    calls++;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x001BA130u);
    return va == 0x001BA130u ? controlled_predecessor : NULL;
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t indexes[] = {0xFFFFFFFFu, 0u, 1u, 2u};
    const uint32_t selections[] = {0u, 6u, 0xFFFFFFFFu};
    const uint32_t flags[] = {0u, 0x4000u, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned frame = 0u; frame < 2u; frame++) {
        for (unsigned index = 0u; index < 4u; index++) {
            for (unsigned selection = 0u; selection < 3u; selection++) {
                for (unsigned pattern = 0u; pattern < 4u; pattern++) {
                    entry_stack = 0x10000u + frame * 0x400u;
                    object = 0xD90000u;
                    selected_index = selections[selection];
                    actor = 0xDA0000u + indexes[index] * 0x1584u;
                    flags_after_call = flags[pattern];
                    g_esp = entry_stack;
                    g_eax = 0x11111111u;
                    g_ecx = 0x22222222u;
                    g_edx = 0x33333333u;
                    g_esi = 0xEEEEEEEEu;
                    guest_write32(entry_stack, 0x00ABCDEFu);
                    guest_write32(entry_stack + 4u, object);
                    guest_write32(entry_stack + 8u, selected_index);
                    guest_write32(object + 8u, indexes[index]);
                    guest_write32(object + 0x94u, 0x12345678u);
                    guest_write32(0x007B0C48u, 0xDA0000u);
                    guest_write32(actor + 12u, flags_after_call ^ 0xFFFFFFFFu);
                    guest_write32(0xDC0000u + 7u * 0x1584u + 12u, 0x55555555u);
                    unsigned prior_calls = calls;
                    t1478_draft_predecessor_actor_flags();
                    CHECK(calls == prior_calls + 1u);
                    CHECK(g_eax == (flags_after_call | 0x4000u));
                    CHECK(guest_read32(actor + 12u) == g_eax);
                    CHECK(guest_read32(0xDC0000u + 7u * 0x1584u + 12u) == 0x55555555u);
                    CHECK(g_ecx == 0xCCCCCCCCu && g_edx == 0xDDDDDDDDu);
                    CHECK(g_esi == 0xEEEEEEEEu && g_esp == entry_stack + 4u);
                    CHECK(guest_read32(object + 0x94u) == selected_index);
                    CHECK(guest_read32(entry_stack) == 0x00ABCDEFu);
                    CHECK(guest_read32(entry_stack - 4u) == 0xEEEEEEEEu);
                    CHECK(guest_read32(entry_stack - 8u) == selected_index);
                    CHECK(guest_read32(entry_stack - 12u) == object);
                    CHECK(guest_read32(entry_stack - 16u) == 0x001BFA81u);
                }
            }
        }
    }
    printf("T1478 unregistered predecessor actor draft: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
