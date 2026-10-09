/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent synthetic callee spy around the actual root adapter.
 * Authentic417932 runs only in the separate ordinary proof campaign. */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x20000];
static unsigned checks, failures, calls, scenario;
static uint32_t input_ecx, expected_status, expected_edx;
static const uint32_t entry = 0x10000u;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr, "%d: %s\n", __LINE__, #x); } } while (0)
void sub_00417ADA(void);

static void service_spy(void)
{
    ++calls;
    CHECK(g_esp == entry - 28u);
    CHECK(guest_read32(g_esp) == 0x00417AECu);
    CHECK(guest_read32(g_esp + 4u) == 0x98765432u);
    CHECK(guest_read32(g_esp + 8u) == entry - 16u);
    CHECK(guest_read32(entry - 4u) == 0x12345678u);
    CHECK(g_eax == entry - 16u && g_ecx == input_ecx);
    CHECK(g_edx == 0xABCDEF01u && g_ebx == 0x55555555u);
    CHECK(g_ebp == entry - 4u && g_esi == 0x66666666u && g_edi == 0x77777777u);
    if (scenario >= 3u) {
        guest_write32(entry - 16u, 0x98765432u);
        guest_write32(entry - 12u, 0x10203040u + scenario);
        guest_write32(entry - 8u, 0x88776655u);
    }
    g_eax = expected_status;
    g_edx = expected_edx;
    g_esp += 12u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    CHECK(va == 0x00417932u);
    return va == 0x00417932u ? service_spy : NULL;
}
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    for (scenario = 0u; scenario < 7u; ++scenario) {
        memset(memory, 0, sizeof memory);
        g_eax = 0x11111111u; input_ecx = scenario == 0u ? 0u : 0x4000u;
        g_ecx = input_ecx; g_edx = 0xABCDEF01u; g_ebx = 0x55555555u;
        g_esp = entry; g_ebp = 0x12345678u; g_esi = 0x66666666u; g_edi = 0x77777777u;
        calls = 0u;
        guest_write32(entry, 0xDEADBEEFu);
        guest_write32(entry + 4u, 0x98765432u);
        switch (scenario) {
        case 0u: expected_status = 0x80150005u; expected_edx = 0xABCDEF01u; break;
        case 1u: expected_status = 0x80150002u; expected_edx = 0xABCDEF01u; break;
        case 2u: expected_status = 0x80151100u; expected_edx = input_ecx + 0x214u; break;
        case 3u: expected_status = 0u; expected_edx = 0u; break;
        case 4u: expected_status = 0x7FFFFFFFu; expected_edx = 75u; break;
        case 5u: expected_status = 0x80000000u; expected_edx = 75u; break;
        default: expected_status = 0xFFFFFFFFu; expected_edx = 0u; break;
        }
        sub_00417ADA();
        CHECK(calls == 1u);
        CHECK(g_eax == ((int32_t)expected_status < 0 ? 0u : 0x10203040u + scenario));
        CHECK(g_ecx == input_ecx && g_edx == expected_edx);
        CHECK(g_ebx == 0x55555555u && g_esp == entry + 8u);
        CHECK(g_ebp == 0x12345678u && g_esi == 0x66666666u && g_edi == 0x77777777u);
        CHECK(guest_read32(entry) == 0xDEADBEEFu && guest_read32(entry + 4u) == 0x98765432u);
    }
    printf("T1480 ECX service spy: %u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
