/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary mapped option/fallback controls with a cache double.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_ebx, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures, calls;
#define STACK 0x10000u
#define OBJECT 0x20000u
#define DESCRIPTOR 0x30000u
#define ROW 0x53D2D0u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void t1479_candidate_350dc0(void);

static void write_word(uint32_t address, uint16_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}
static void cache(void)
{
    CHECK(g_esp == STACK - 20u && guest_read32(g_esp) == 0x350DEAu);
    CHECK(game_stack_arg(0u) == 101u && g_eax == 101u);
    CHECK(g_ebx == OBJECT && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u);
    g_eax = DESCRIPTOR; g_ecx = 0xEC111111u; g_edx = 0xED111111u;
    calls++; g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x6A850u);
    return va == 0x6A850u ? cache : NULL;
}
static void reset(void)
{
    memset(memory, 0, sizeof(memory)); calls = 0u;
    g_esp = STACK; g_ebx = 0xBBBBBBBBu; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u;
    g_eax = 0xAAAAAAAAu; g_ecx = 0xCCCCCCCCu; g_edx = 0xDDDDDDDDu;
    guest_write32(STACK, 0x10101010u); guest_write32(STACK + 4u, OBJECT);
    guest_write32(0x79094Cu, 101u);
}
static void preserved(void)
{
    CHECK(g_esp == STACK && guest_read32(STACK) == 0x10101010u);
    CHECK(g_ebx == 0xBBBBBBBBu && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u);
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t pointers[] = {0u, OBJECT, 0xFFFFFFFFu};
    for (unsigned i = 0u; i < 3u; i++) {
        reset(); guest_write32(STACK + 4u, pointers[i]); guest_write8(0x7DE458u, 0x40u);
        t1479_candidate_350dc0();
        CHECK(g_eax == 8u && calls == 0u); preserved();
        CHECK(g_ecx == 0xCCCCCCCCu && g_edx == 0xDDDDDDDDu);
        CHECK(guest_read32(STACK - 4u) == 0u);
    }
    const uint32_t counts[] = {0u, 1u, 3u};
    const uint8_t results[] = {0u, 255u};
    for (unsigned count = 0u; count < 3u; count++)
        for (unsigned match = 0u; match < 2u; match++)
            for (unsigned selector = 0u; selector < 2u; selector++)
                for (unsigned result = 0u; result < 2u; result++) {
                    reset(); guest_write8(0x7DE455u, 10u);
                    guest_write8(0x7DE457u, (uint8_t)selector);
                    guest_write32(ROW, counts[count]); write_word(OBJECT + 0x48u, 7u);
                    for (uint32_t i = 0u; i < counts[count]; i++) {
                        write_word(ROW + 4u + 8u * i, (uint16_t)(match && i == 0u ? 7u : 8u));
                        guest_write8(ROW + 6u + 8u * i + selector, results[result]);
                    }
                    guest_write8(0x7DE434u, 5u);
                    t1479_candidate_350dc0();
                    CHECK(g_eax == (counts[count] && match ? (uint32_t)results[result] : 0u));
                    CHECK(calls == 1u); preserved();
                    CHECK(g_ecx == 0u);
                    CHECK(g_edx == (counts[count] && match ? ROW + selector : 5u));
                }
    const uint32_t tags[] = {0xFFFFFFFFu, 0u, 2u};
    const uint32_t indexes[] = {0xFFFFFFFFu, 0u, 3u};
    const uint8_t bytes[] = {0u, 5u, 255u};
    for (unsigned mode = 0u; mode < 2u; mode++)
        for (unsigned table = 0u; table < 2u; table++)
            for (unsigned tag = 0u; tag < 3u; tag++)
                for (unsigned index = 0u; index < 3u; index++) {
                    reset(); guest_write8(0x7DE455u, mode ? 10u : 1u);
                    guest_write32(0x7DE458u, table ? 0x80000u : 0u);
                    write_word(OBJECT + 0x48u, (uint16_t)tags[tag]);
                    guest_write32(OBJECT + 8u, indexes[index]);
                    guest_write8(0x7DE434u + indexes[index], bytes[index]);
                    guest_write32(0x53A3DCu + 48u * tags[tag], 0x12345678u);
                    t1479_candidate_350dc0();
                    CHECK(g_eax == (table ? 0x12345678u : 3u * (5u - bytes[index])));
                    CHECK(calls == mode); preserved();
                    CHECK(g_ecx == (table ? mode ? 0u : 0xCCCCCCCCu : indexes[index]));
                    CHECK(g_edx == (table ? mode ? 0xED111111u : 0xDDDDDDDDu : bytes[index]));
                }
    printf("T1479 cache ordinary controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
