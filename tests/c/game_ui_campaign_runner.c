/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone original-contract controls for the T1791 qualified leaf bodies. */
#include "game_replace.h"
#include <stdio.h>
#include <string.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_edi;
ptrdiff_t g_xbox_mem_offset;
static unsigned checks, failures;
static unsigned root_checks, root_failures;
static uint8_t memory[0x810000u];
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr); } } while (0)

void sub_00032F10(void);
#ifdef T1791_TEST_NTH_NODE
void sub_000757A0(void);
#endif
void sub_00147200(void);

static void prepare(void)
{
    memset(memory, 0, sizeof memory);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    g_esp = 0x1000u;
    guest_write32(g_esp, 0x12345678u);
    g_eax = 0x11111111u;
    g_ecx = 0x22222222u;
    g_edx = 0x33333333u;
    g_esi = 0x51515151u;
    g_ebx = 0xB0B0B0B0u;
    g_ebp = 0xBEBEBEBEu;
    g_edi = 0xD1D1D1D1u;
}

static void check_return(void)
{
    CHECK(g_esp == 0x1004u && guest_read32(0x1000u) == 0x12345678u);
    CHECK(g_esi == 0x51515151u && g_ebx == 0xB0B0B0B0u &&
          g_ebp == 0xBEBEBEBEu && g_edi == 0xD1D1D1D1u);
}

static void begin_root(void)
{ root_checks = checks; root_failures = failures; }

static void end_root(uint32_t va)
{
    printf("0x%08X %u checks %u failures\n", va, checks - root_checks,
           failures - root_failures);
}

#include "test_game_ui_campaign.inc"

int main(void)
{
    test_t1791_status_result();
#ifdef T1791_TEST_NTH_NODE
    test_t1791_nth_node();
#endif
    test_t1791_notice_append();
    printf("%u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
