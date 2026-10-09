/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary paired-table/frame tests with explicit lookup/relation doubles.
 * Keys are authenticated static data; no original code is executed here.
 */
#include "game_replace.h"
#include <stdio.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls, pair, stage, length, mode;
static uint32_t entry_stack, table, right_pc, left_pc, relation_pc;
static const uint32_t (*pairs)[2];
static const uint32_t first[][2] = {{1172,1305},{1172,1307},{1171,1307},{1140,1307},
    {1140,1308},{1175,1308},{1175,1309},{1174,1310},{1062,1310},{1062,1311},{1211,1212}};
static const uint32_t second[][2] = {{1034,1217},{1046,1158},{1005,1006},{1024,1291},{1186,1187}};
#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)
extern void t1478_draft_relation_flag_clear_220900(void);
extern void t1478_draft_relation_flag_clear_220960(void);
static uint32_t index_of(uint32_t key)
{
    /* Number of distinct authenticated keys below key, independently derived. */
    uint32_t result = 0u;
    for (unsigned i = 0u; i < length; i++) {
        for (unsigned endpoint = 0u; endpoint < 2u; endpoint++) {
            uint32_t value = pairs[i][endpoint];
            if (value >= key) continue;
            unsigned seen = 0u;
            for (unsigned j = 0u; j <= i; j++) {
                unsigned count = j == i ? endpoint : 2u;
                for (unsigned k = 0u; k < count; k++)
                    if (pairs[j][k] == value) seen = 1u;
            }
            if (!seen) result++;
        }
    }
    return result;
}
static unsigned found(unsigned item)
{
    return mode == 1u || (mode == 2u && (item & 1u) == 0u);
}
static void controlled_lookup(void)
{
    CHECK(stage < 2u && pair < length);
    CHECK(g_esi == table + pair * 8u);
    CHECK(g_esp == entry_stack - (stage == 0u ? 12u : 16u));
    CHECK(guest_read32(g_esp) == (stage == 0u ? right_pc : left_pc));
    uint32_t key = pairs[pair][stage == 0u ? 1u : 0u];
    CHECK(guest_read32(g_esp + 4u) == key);
    if (stage == 0u) CHECK(g_eax == key);
    else CHECK(g_ecx == key);
    uint32_t index = index_of(key);
    if ((mode == 3u && stage == 0u) || (mode == 4u && stage == 1u)) index = 0xFFFFFFFFu;
    guest_write32(0x00715F2Cu, stage == 0u ? 0xDB0000u : 0xDC0000u);
    g_eax = index;
    g_ecx = 0xAAAAAAAAu;
    g_edx = 0xBBBBBBBBu;
    g_esp += 4u;
    stage++;
    calls++;
}
static void controlled_relation(void)
{
    CHECK(stage == 2u && pair < length);
    CHECK(g_esp == entry_stack - 16u);
    CHECK(guest_read32(g_esp) == relation_pc);
    uint32_t left = mode == 4u ? 0xFFFFFFFFu : index_of(pairs[pair][0]);
    uint32_t right = mode == 3u ? 0xFFFFFFFFu : index_of(pairs[pair][1]);
    CHECK(guest_read32(g_esp + 4u) == 0xDC0000u + left * 32u);
    CHECK(guest_read32(g_esp + 8u) == 0xDB0000u + right * 32u);
    CHECK(g_eax == 0xDC0000u + left * 32u && g_edx == 0xDC0000u);
    g_eax = found(pair) ? 0xDE0000u + pair * 20u : 0u;
    g_ecx = 0xCCCC0000u + pair;
    g_edx = 0xDDDD0000u + pair;
    g_esp += 4u;
    stage = 0u;
    pair++;
    calls++;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == (stage < 2u ? 0x0009CC40u : 0x0009D740u));
    if (va == 0x0009CC40u) return controlled_lookup;
    if (va == 0x0009D740u) return controlled_relation;
    return NULL;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t patterns[] = {0x10000u, 0x10001u, 0xFFFFFFFFu};
    for (unsigned root = 0u; root < 2u; root++) {
        pairs = root ? second : first;
        length = root ? 5u : 11u;
        table = root ? 0x00509778u : 0x00509720u;
        right_pc = root ? 0x0022096Fu : 0x0022090Fu;
        left_pc = root ? 0x00220986u : 0x00220926u;
        relation_pc = root ? 0x0022099Au : 0x0022093Au;
        for (unsigned frame = 0u; frame < 2u; frame++) {
            for (mode = 0u; mode < 5u; mode++) {
                for (unsigned pattern = 0u; pattern < 3u; pattern++) {
                    entry_stack = 0x10000u + frame * 0x400u;
                    g_esp = entry_stack;
                    g_eax = 0x11111111u; g_ecx = 0x22222222u; g_edx = 0x33333333u;
                    g_esi = 0xEEEEEEEEu;
                    guest_write32(entry_stack, 0x00ABCDEFu);
                    guest_write32(0x00715F2Cu, 0xDA0000u);
                    for (unsigned i = 0u; i < length; i++) {
                        guest_write32(table + i * 8u, pairs[i][0]);
                        guest_write32(table + i * 8u + 4u, pairs[i][1]);
                        guest_write32(0xDE0000u + i * 20u, patterns[pattern]);
                    }
                    pair = stage = calls = 0u;
                    if (root) t1478_draft_relation_flag_clear_220960();
                    else t1478_draft_relation_flag_clear_220900();
                    CHECK(pair == length && stage == 0u && calls == length * 3u);
                    CHECK(g_eax == (found(length - 1u) ? 0xDE0000u + (length - 1u) * 20u : 0u));
                    CHECK(g_ecx == 0xCCCC0000u + length - 1u && g_edx == 0xDDDD0000u + length - 1u);
                    CHECK(g_esi == 0xEEEEEEEEu && g_esp == entry_stack + 4u);
                    CHECK(guest_read32(entry_stack) == 0x00ABCDEFu);
                    CHECK(guest_read32(entry_stack - 4u) == 0xEEEEEEEEu);
                    CHECK(guest_read32(entry_stack - 16u) == relation_pc);
                    for (unsigned i = 0u; i < length; i++)
                        CHECK(guest_read32(0xDE0000u + i * 20u) ==
                              (found(i) ? patterns[pattern] & 0xFFFEFFFFu : patterns[pattern]));
                }
            }
        }
    }
    printf("T1478 unregistered relation drafts: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
