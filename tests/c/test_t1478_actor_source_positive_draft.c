/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary frame/source controls with explicit1B67E0 double, never an oracle.
 * No saved-frame aliases, original execution or fault probes.
 */
#include "game_replace.h"
#include <stdio.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls;
static uint32_t entry_stack, object, child, source_sum;
static int16_t final_word;
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)
extern void t1478_draft_actor_source_positive(void);

static void controlled_sources(void)
{
    CHECK(g_esp == entry_stack - 20u);
    CHECK(guest_read32(g_esp) == 0x001B68C9u);
    CHECK(guest_read32(g_esp + 4u) == object && guest_read32(g_esp + 8u) == child);
    CHECK(g_esi == child && g_edi == object);
    CHECK(g_eax == 2u * 0x29Cu && g_ecx == 0xDA0000u);
    /* Supply the final signed word only during the call: root must read late. */
    memcpy(game_host_ptr(object + 0x61Eu + child * 2u), &final_word, sizeof(final_word));
    guest_write32(entry_stack + 4u, 0x90000000u);
    g_eax = source_sum;
    g_ecx = 0xCCCCCCCCu;
    g_edx = 0xDDDDDDDDu;
    g_esp += 4u;
    calls++;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x001B67E0u);
    return va == 0x001B67E0u ? controlled_sources : NULL;
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const int32_t sums[] = {-65536, -1, 0, 1, 65534, 0};
    const int16_t words[] = {-32768, 1, 0, -1, 32767, 1};
    for (unsigned frame = 0u; frame < 2u; frame++) {
        for (unsigned slot = 0u; slot <= 2u; slot += 2u) {
            for (unsigned active = 0u; active < 2u; active++) {
                for (child = 0u; child < 3u; child++) {
                    for (unsigned pattern = 0u; pattern < 6u; pattern++) {
                        entry_stack = 0x10000u + frame * 0x400u;
                        object = 0xD90000u;
                        source_sum = (uint32_t)sums[pattern];
                        final_word = words[pattern];
                        g_esp = entry_stack;
                        g_eax = 0x11111111u;
                        g_ecx = 0x22334455u;
                        g_edx = 0x33333333u;
                        g_esi = 0xEEEEEEEEu;
                        g_edi = 0xFFFFFFFFu;
                        guest_write32(entry_stack, 0x00ABCDEFu);
                        guest_write32(entry_stack + 4u, object);
                        guest_write32(entry_stack + 8u, slot);
                        guest_write8(object + 0x54Cu + slot, (uint8_t)active);
                        guest_write32(0x004F9BACu, 0xDA0000u);
                        guest_write32(0x004FA138u + slot * 0x3Cu, 2u);
                        guest_write32(0xDA0000u + 2u * 0x29Cu + 0x14u, child);
                        int16_t before = (int16_t)~final_word;
                        memcpy(game_host_ptr(object + 0x61Eu + child * 2u), &before, sizeof(before));
                        unsigned prior_calls = calls;
                        t1478_draft_actor_source_positive();
                        unsigned called = active != 0u && child != 0u;
                        uint32_t expected = called && sums[pattern] + (int32_t)words[pattern] > 0 ? 1u : 0u;
                        CHECK(calls == prior_calls + called);
                        CHECK(g_eax == expected);
                        CHECK(g_ecx == (called ? 0xCCCCCCCCu : active ? 0xDA0000u : 0x22334400u));
                        CHECK(g_edx == (called ? (uint32_t)(int32_t)final_word : 0x33333333u));
                        CHECK(g_esi == 0xEEEEEEEEu && g_edi == 0xFFFFFFFFu);
                        CHECK(g_esp == entry_stack + 4u);
                        CHECK(guest_read32(entry_stack) == 0x00ABCDEFu);
                        CHECK(guest_read32(entry_stack - 4u) == 0xEEEEEEEEu);
                        CHECK(guest_read32(entry_stack - 8u) == 0xFFFFFFFFu);
                        if (called) {
                            CHECK(guest_read32(entry_stack - 12u) == child);
                            CHECK(guest_read32(entry_stack - 16u) == object);
                            CHECK(guest_read32(entry_stack - 20u) == 0x001B68C9u);
                        }
                    }
                }
            }
        }
    }
    printf("T1478 unregistered actor-source draft: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
