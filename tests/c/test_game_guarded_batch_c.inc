/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 batch C native tests for src/game/game_guarded_batch_c.c (included by
 * test_game_replacements.c after test_game_guarded_batch_a.inc).
 *
 * Expectations are hand-derived from the retail disassembly (docs/evidence/t1576/batch-c.md), not
 * by running the replacement against itself. The two live callees are recorded mocks reached
 * through recomp_lookup while gc_active.
 *
 * 0x002D6490 (cdecl, no arguments):
 *   call 0x35E960 (ret 0x2D6495); test eax,eax; je table
 *   call 0x35E970 (ret 0x2D649E); cmp eax,1; jne next; eax = 0x4E9A14; ret
 *   call 0x35E970 (ret 0x2D64AE); cmp eax,2; je close
 *   call 0x35E970 (ret 0x2D64B8); cmp eax,3; jne table; close: eax = 0x4E9A24; ret
 *   table: eax = [0x78AD20] - 4; cmp eax,0xC; ja zero; jmp [eax*4 + 0x2D6514]
 *   table slots (retail dump): 0 -> 0x5238E0, 1 -> 0x523800, 2 and 3 -> 0x523860, 4 -> 0x523830,
 *   5 -> ([0x78ADAE] == 2 ? 0x523890 : 0x523800), 6 and 12 -> 0x524578, 7 to 9 and the JA default
 *   -> 0, 10 -> 0x523920, 11 -> 0x524598. ECX and EDX are never written.
 * 0x00338BC0 (cdecl src, dst): saves ESI and EDI, edi = src, ecx = [edi+4], edx = eax = 0;
 *   cmp ecx,3; ja join; jmp [ecx*4 + 0x338C28]: kinds 0, 1, 2, 3 set eax to 0, 3, 2, 1; join:
 *   esi = dst; [esi] = eax; [esi+4] = [edi+0xC]; ecx = [edi+0x10]; [esi+8] = ecx; [esi+0xC] = 0;
 *   [esi+0x10] = 0; edx = byte [edi]; call 0x3194C0(edx) (ret 0x338C11); [esi+0x14] = eax;
 *   eax = byte [edi+1]; call 0x3194C0(eax) (ret 0x338C1E); [esi+0x18] = eax; restore EDI, ESI. */

static int gc_active_net_live;
static uint32_t gc_reason;
static unsigned gc_live_calls;
static unsigned gc_reason_calls;
static uint32_t gc_reason_returns[3];
static unsigned gc_palette_calls;
static uint32_t gc_palette_returns[2];
static uint32_t gc_palette_args[2];

static void gc_mock_net_live(void)
{
    CHECK(guest_read32(g_esp) == 0x002D6495u);
    gc_live_calls++;
    g_eax = (uint32_t)gc_active_net_live;
    g_esp += 4u;
}

static void gc_mock_reason(void)
{
    if (gc_reason_calls < 3u) gc_reason_returns[gc_reason_calls] = guest_read32(g_esp);
    gc_reason_calls++;
    g_eax = gc_reason;
    g_esp += 4u;
}

static void gc_mock_palette(void)
{
    if (gc_palette_calls < 2u) {
        gc_palette_returns[gc_palette_calls] = guest_read32(g_esp);
        gc_palette_args[gc_palette_calls] = game_stack_arg(0u);
    }
    g_eax = 0xC0000000u + game_stack_arg(0u) * 3u;
    gc_palette_calls++;
    g_esp += 4u;
}

static game_guest_function gc_lookup(uint32_t xbox_va)
{
    if (xbox_va == 0x0035E960u) return gc_mock_net_live;
    if (xbox_va == 0x0035E970u) return gc_mock_reason;
    if (xbox_va == 0x003194C0u) return gc_mock_palette;
    return NULL;
}

static void gc_2d6490(int live, uint32_t reason, uint32_t state, uint32_t flag, uint32_t expect,
                      unsigned live_calls, unsigned reason_calls)
{
    prepare();
    guest_write32(0x0078AD20u, state);
    guest_write8(0x0078ADAEu, (uint8_t)flag);
    gc_active_net_live = live;
    gc_reason = reason;
    gc_live_calls = gc_reason_calls = 0u;
    gc_reason_returns[0] = gc_reason_returns[1] = gc_reason_returns[2] = 0u;
    gc_active = 1;
    invoke(0x002D6490u, 4u);
    gc_active = 0;
    CHECK(g_eax == expect);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
    CHECK(gc_live_calls == live_calls && gc_reason_calls == reason_calls);
    if (reason_calls >= 1u) CHECK(gc_reason_returns[0] == 0x002D649Eu);
    if (reason_calls >= 2u) CHECK(gc_reason_returns[1] == 0x002D64AEu);
    if (reason_calls >= 3u) CHECK(gc_reason_returns[2] == 0x002D64B8u);
}

static void gc_338bc0(uint32_t kind, uint32_t expect_first)
{
    const uint32_t src = 0x2000u;
    const uint32_t dst = 0x3000u;
    prepare();
    g_esi = 0x51515151u;
    g_edi = 0x71717171u;
    guest_write32(g_esp + 4u, src);
    guest_write32(g_esp + 8u, dst);
    guest_write8(src, 0x12u);
    guest_write8(src + 1u, 0x34u);
    guest_write8(src + 2u, 0x56u);
    guest_write8(src + 3u, 0x78u);
    guest_write32(src + 4u, kind);
    guest_write32(src + 0xCu, 0xAABBCCDDu);
    guest_write32(src + 0x10u, 0x01020304u);
    for (uint32_t offset = 0u; offset < 0x20u; offset += 4u) guest_write32(dst + offset, 0xEEEEEEEEu);
    gc_palette_calls = 0u;
    gc_active = 1;
    invoke(0x00338BC0u, 4u);
    gc_active = 0;
    CHECK(gc_palette_calls == 2u);
    CHECK(gc_palette_returns[0] == 0x00338C11u && gc_palette_returns[1] == 0x00338C1Eu);
    CHECK(gc_palette_args[0] == 0x12u && gc_palette_args[1] == 0x34u);
    CHECK(guest_read32(dst) == expect_first);
    CHECK(guest_read32(dst + 4u) == 0xAABBCCDDu && guest_read32(dst + 8u) == 0x01020304u);
    CHECK(guest_read32(dst + 0xCu) == 0u && guest_read32(dst + 0x10u) == 0u);
    CHECK(guest_read32(dst + 0x14u) == 0xC0000000u + 0x12u * 3u);
    CHECK(guest_read32(dst + 0x18u) == 0xC0000000u + 0x34u * 3u);
    CHECK(guest_read32(dst + 0x1Cu) == 0xEEEEEEEEu);
    CHECK(g_eax == 0xC0000000u + 0x34u * 3u);
    CHECK(g_ecx == 0x01020304u && g_edx == 0x12u);
    CHECK(g_esi == 0x51515151u && g_edi == 0x71717171u);
}

static void test_t1576_batch_c(void)
{
    /* net mode off: one probe, then the table */
    gc_2d6490(0, 9u, 4u, 0u, 0x005238E0u, 1u, 0u);
    /* net mode live: reason 1 answers at once, 2 on the second probe, 3 on the third */
    gc_2d6490(1, 1u, 4u, 0u, 0x004E9A14u, 1u, 1u);
    gc_2d6490(1, 2u, 4u, 0u, 0x004E9A24u, 1u, 2u);
    gc_2d6490(1, 3u, 4u, 0u, 0x004E9A24u, 1u, 3u);
    gc_2d6490(1, 0u, 5u, 0u, 0x00523800u, 1u, 3u); /* other reason: falls to the table */
    gc_2d6490(1, 4u, 4u, 0u, 0x005238E0u, 1u, 3u);
    gc_2d6490(1, 0xFFFFFFFFu, 17u, 0u, 0u, 1u, 3u); /* above the bound */
    /* every table slot and the unsigned bound (state - 4) */
    gc_2d6490(0, 0u, 4u, 0u, 0x005238E0u, 1u, 0u);
    gc_2d6490(0, 0u, 5u, 0u, 0x00523800u, 1u, 0u);
    gc_2d6490(0, 0u, 6u, 0u, 0x00523860u, 1u, 0u);
    gc_2d6490(0, 0u, 7u, 0u, 0x00523860u, 1u, 0u);
    gc_2d6490(0, 0u, 8u, 0u, 0x00523830u, 1u, 0u);
    gc_2d6490(0, 0u, 9u, 2u, 0x00523890u, 1u, 0u); /* slot 5, flag byte 2 */
    gc_2d6490(0, 0u, 9u, 0u, 0x00523800u, 1u, 0u);
    gc_2d6490(0, 0u, 9u, 3u, 0x00523800u, 1u, 0u);
    gc_2d6490(0, 0u, 10u, 0u, 0x00524578u, 1u, 0u);
    gc_2d6490(0, 0u, 11u, 0u, 0u, 1u, 0u);
    gc_2d6490(0, 0u, 12u, 0u, 0u, 1u, 0u);
    gc_2d6490(0, 0u, 13u, 0u, 0u, 1u, 0u);
    gc_2d6490(0, 0u, 14u, 0u, 0x00523920u, 1u, 0u);
    gc_2d6490(0, 0u, 15u, 0u, 0x00524598u, 1u, 0u);
    gc_2d6490(0, 0u, 16u, 0u, 0x00524578u, 1u, 0u);
    gc_2d6490(0, 0u, 17u, 0u, 0u, 1u, 0u);
    gc_2d6490(0, 0u, 3u, 0u, 0u, 1u, 0u); /* 3 - 4 wraps above the bound */
    gc_2d6490(0, 0u, 0u, 0u, 0u, 1u, 0u);
    gc_2d6490(0, 0u, 0x80000004u, 0u, 0u, 1u, 0u); /* unsigned compare, not signed */

    gc_338bc0(0u, 0u);
    gc_338bc0(1u, 3u);
    gc_338bc0(2u, 2u);
    gc_338bc0(3u, 1u);
    gc_338bc0(4u, 0u);
    gc_338bc0(0xFFFFFFFFu, 0u); /* unsigned compare, not signed */
    gc_338bc0(0x80000000u, 0u);
}
