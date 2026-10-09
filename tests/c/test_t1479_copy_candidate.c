/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary mapped DF/call controls with declared double; not original execution.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_ebx, g_esi, g_edi;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures, calls;
static uint32_t clamped;
#define STACK 0x10000u
#define OWNER 0x20000u
#define NODE 0x30000u
#define SOURCE 0x40000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void t1479_candidate_3081e0(void);

static void append(void)
{
    CHECK(g_esp == STACK - 16u && guest_read32(g_esp) == 0x308254u);
    CHECK(g_eax == 0x22000u && g_esi == NODE && g_edi == OWNER && g_ebx == NODE);
    CHECK(g_ecx == clamped && g_edx == 0xED001234u);
    CHECK(guest_read32(NODE + 8u) == clamped);
    CHECK(guest_read32(NODE + 0xCu) == 0xABCDEF01u);
    uint16_t word;
    memcpy(&word, game_host_ptr(NODE + 0x10u), sizeof word);
    CHECK(word == 0x1234u);
    CHECK(guest_read32(NODE) == 0xA5000000u);
    CHECK(guest_read32(NODE + (g_df != 0 ? 0xFFFFFFC8u : 56u)) == 0xA500000Eu);
    g_eax = 0x23000u; g_ecx = 0xEC222222u; g_edx = 0xED222222u;
    calls++; g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x308070u);
    return va == 0x308070u ? append : NULL;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t values[] = {0xFFFFFFFFu, 0u, 7u};
    for (unsigned free_node = 0u; free_node < 2u; free_node++)
        for (unsigned source = 0u; source < 2u; source++)
            for (unsigned df = 0u; df < 2u; df++)
                for (unsigned value = 0u; value < 3u; value++) {
                    memset(memory, 0, sizeof(memory)); calls = 0u;
                    g_esp = STACK; g_df = (int)df; g_ebx = 0xBBBBBBBBu;
                    g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u;
                    g_ecx = 0xEC000000u; g_edx = 0xED000000u;
                    guest_write32(STACK, 0x10101010u);
                    guest_write32(STACK + 4u, OWNER);
                    guest_write32(STACK + 8u, 0xABCDEF01u);
                    guest_write32(STACK + 12u, 0xFFFF1234u);
                    guest_write32(STACK + 16u, source ? SOURCE : 0u);
                    guest_write32(0x762CC8u, free_node ? NODE : 0u);
                    guest_write32(0x762CC0u, 5u);
                    guest_write32(NODE + 0x14u, 0x31000u);
                    guest_write32(OWNER + 0x3Cu, 0x22000u);
                    for (uint32_t i = 0u; i < 15u; i++)
                        guest_write32(SOURCE + (df ? 0u - 4u * i : 4u * i), 0xA5000000u + i);
                    guest_write32(SOURCE + 8u, values[value]);
                    clamped = value == 2u ? 7u : 0u;
                    t1479_candidate_3081e0();
                    const unsigned active = free_node && source;
                    CHECK(g_eax == (active ? NODE : 0u) && calls == active);
                    CHECK(g_ebx == 0xBBBBBBBBu && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u);
                    CHECK(g_esp == STACK && g_df == (int)df && guest_read32(STACK) == 0x10101010u);
                    CHECK(guest_read32(0x762CC0u) == (free_node ? 4u : 5u));
                    CHECK(guest_read32(0x762CC8u) == (free_node ? 0x31000u : 0u));
                    CHECK(guest_read32(OWNER + 0x3Cu) == (active ? 0x23000u : 0x22000u));
                    CHECK(g_ecx == (active ? 0xEC222222u : 0xEC000000u));
                    CHECK(g_edx == (active ? 0xED222222u : 0xED000000u));
                }
    printf("T1479 copy ordinary controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
