/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1602 native tests for the admitted root (00042F90) of src/game/game_ui_list001.c (included by
 * test_game_replacements.c). Expectations are hand-derived from the retail disassembly
 * (register outputs, memory, stack balance), not by running the replacement against itself. */

static void ui_begin(void)
{
    prepare();
    g_ebx = 0xB0B0B0B0u;
    g_ebp = 0xBEBEBE01u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
}

static void ui_check_saved_registers(void)
{
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
}

static uint32_t ui_prompt(uint32_t flags, uint32_t level, uint32_t player, uint32_t field)
{
    ui_begin();
    guest_write32(0x7DE458u, flags);
    guest_write32(0x79094Cu, level);
    guest_write32(0x7B0C48u, player);
    if (player != 0u)
        guest_write32(player + 0x114u, field);
    invoke(0x00042F90u, 4u);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
    ui_check_saved_registers();
    return g_eax;
}

static void test_ui_prompt_allowed(void)
{
    /* 42F90: flag bits 0xE0000200 -> 0; level > 0x64 signed -> 1; null player -> 1;
     * field +0x114 == 0x100 -> 0, else 1. */
    CHECK(ui_prompt(0x00000200u, 0u, 0x30000u, 0u) == 0u);
    CHECK(ui_prompt(0x80000000u, 1000u, 0u, 0u) == 0u);
    CHECK(ui_prompt(0x20000000u, 0u, 0u, 0u) == 0u);
    CHECK(ui_prompt(0x40000000u, 0u, 0u, 0u) == 0u);
    CHECK(ui_prompt(0x1FFFFDFFu, 0x65u, 0x30000u, 0x100u) == 1u);
    CHECK(ui_prompt(0u, 0x64u, 0x30000u, 0x100u) == 0u);
    CHECK(ui_prompt(0u, 0xFFFFFFFFu, 0x30000u, 0x100u) == 0u);
    CHECK(ui_prompt(0u, 0x80000000u, 0x30000u, 0x100u) == 0u);
    CHECK(ui_prompt(0u, 0u, 0u, 0u) == 1u);
    CHECK(ui_prompt(0u, 0u, 0x30000u, 0x101u) == 1u);
    CHECK(ui_prompt(0u, 0u, 0x30000u, 0x100u) == 0u);
}

static void test_t1602_ui_list001(void)
{
    test_ui_prompt_allowed();
}
