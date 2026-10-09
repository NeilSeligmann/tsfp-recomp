/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary mapped memory controls; no original execution.
 */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x800000];
static unsigned checks, failures;
#define BASE 0x20000u
#define STACK 0x10000u
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
void t1479_candidate_32a5b0(void);

static void reset(uint32_t key)
{
    memset(memory, 0, sizeof(memory));
    g_esp = STACK; g_eax = 0xAAAAAAAAu; g_ecx = 0xCCCCCCCCu;
    g_edx = 0xDDDDDDDDu; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u;
    guest_write32(STACK, 0x10101010u);
    guest_write32(STACK + 4u, BASE);
    guest_write32(STACK + 8u, key);
    guest_write32(BASE, 0xC77667u);
    guest_write32(BASE + 4u, 0x12Du);
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (unsigned bad = 0u; bad < 2u; bad++) {
        reset(9u);
        guest_write32(BASE + 4u * bad, 0u);
        t1479_candidate_32a5b0();
        CHECK(g_eax == 0u && g_ecx == BASE && g_edx == 0xDDDDDDDDu);
        CHECK(g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u);
        CHECK(guest_read32(STACK - 4u) == 0u && guest_read32(STACK - 8u) == 0u);
    }
    const uint32_t sizes[] = {0u, 1u, 4u, 32u};
    for (unsigned length = 0u; length < 5u; length++)
        for (unsigned target = 0u; target <= length; target++)
            for (unsigned size = 0u; size < 4u; size++) {
                reset(9u);
                uint32_t cursor = BASE + 8u, expected = 0u, final_cursor = cursor;
                for (unsigned i = 0u; i < length; i++) {
                    guest_write32(cursor, i == target ? 9u : 10u);
                    guest_write32(cursor + 4u, sizes[size]);
                    if (i == target) { expected = cursor + 8u; final_cursor = cursor; }
                    cursor += 8u + sizes[size];
                }
                guest_write32(cursor, 0xC7766Au);
                if (expected == 0u) final_cursor = cursor;
                t1479_candidate_32a5b0();
                CHECK(g_eax == expected && g_ecx == final_cursor);
                CHECK(g_edx == (expected == 0u ? 0xC7766Au : 9u));
                CHECK(g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u && g_esp == STACK);
                CHECK(guest_read32(STACK - 4u) == 0x51515151u);
                CHECK(guest_read32(STACK - 8u) == 0xD1D1D1D1u);
                CHECK(guest_read32(STACK) == 0x10101010u);
            }
    reset(0xC7766Au);
    guest_write32(BASE + 8u, 0xC7766Au);
    t1479_candidate_32a5b0();
    CHECK(g_eax == BASE + 16u); /* Key comparison precedes sentinel test. */
    reset(9u);
    guest_write32(BASE + 8u, 10u);
    guest_write32(BASE + 12u, 0x7FD0u);
    t1479_candidate_32a5b0();
    CHECK(g_eax == 0u && g_ecx == BASE + 8u && g_edx == 10u);
    printf("T1479 chunk ordinary controls: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
