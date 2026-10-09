/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ordinary-frame unit controls only; the callee below is a declared test double,
 * never an original-code oracle or differential proof. No saved-frame aliases.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
static unsigned checks, failures, calls;
static uint32_t entry_stack, object, returned_group, written_value;
static uint32_t expected_ecx, expected_edx;

#define CHECK(e) do { checks++; if (!(e)) { failures++; \
    fprintf(stderr, "line%d:%s\n", __LINE__, #e); } } while (0)

extern void sub_00272430(void);

static void controlled_search(void)
{
    CHECK(g_esp == entry_stack - 8u);
    CHECK(guest_read32(g_esp) == 0x0027243Au);
    CHECK(guest_read32(g_esp + 4u) == object);
    CHECK(g_eax == object);
    CHECK(g_ecx == expected_ecx && g_edx == expected_edx);
    /* The root must read the returned group's field after this call. Its own
     * original argument is no longer needed, so changing it exposes reloading. */
    guest_write32(entry_stack + 4u, 0x90000000u);
    guest_write32(returned_group + 8u, written_value);
    g_eax = returned_group;
    g_ecx = 0xC0C0C0C0u;
    g_edx = 0xD0D0D0D0u;
    g_esp += 4u; /* retail cdecl callee RET; argument remains live */
    calls++;
}

game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x00271950u);
    return va == 0x00271950u ? controlled_search : NULL;
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    const uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned frame = 0u; frame < 2u; frame++) {
        for (unsigned bank = 0u; bank < 2u; bank++) {
            for (unsigned group = 0u; group < 3u; group++) {
                for (unsigned value = 0u; value < sizeof(values) / sizeof(values[0]); value++) {
                    entry_stack = 0x10000u + frame * 0x400u;
                    object = 0xD90000u;
                    returned_group = 0xDA0000u + bank * 0x4B0u + group * 0x12Cu;
                    written_value = values[value];
                    expected_ecx = 0x12345678u ^ group;
                    expected_edx = 0x87654321u ^ value;
                    g_esp = entry_stack;
                    g_eax = 0xEEEEEEEEu;
                    g_ecx = expected_ecx;
                    g_edx = expected_edx;
                    guest_write32(entry_stack, 0x00ABCDEFu);
                    guest_write32(entry_stack + 4u, object);
                    guest_write32(returned_group + 4u, written_value ^ 0xFFFFFFFFu);
                    guest_write32(returned_group + 8u, 0x55555555u);
                    guest_write32(returned_group + 12u, written_value ^ 0xFFFFFFFFu);
                    unsigned prior_calls = calls;
                    sub_00272430();
                    CHECK(calls == prior_calls + 1u);
                    CHECK(g_eax == written_value);
                    CHECK(g_ecx == 0xC0C0C0C0u && g_edx == 0xD0D0D0D0u);
                    CHECK(g_esp == entry_stack + 4u);
                    CHECK(guest_read32(entry_stack) == 0x00ABCDEFu);
                    CHECK(guest_read32(entry_stack - 4u) == object);
                    CHECK(guest_read32(entry_stack - 8u) == 0x0027243Au);
                    CHECK(guest_read32(entry_stack + 4u) == 0x90000000u);
                }
            }
        }
    }
    printf("T1478 registered nested-group actual body: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
