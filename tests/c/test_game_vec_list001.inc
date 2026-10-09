/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1595 native tests for the 2 admitted roots of src/game/game_vec_list001.c (included by
 * test_game_replacements.c).
 * Expectations are hand-derived from the retail disassembly (GPR outputs, memory, raw XMM0
 * lanes), not by running the replacement against itself. XMM0 is seeded with a nonzero
 * pattern so the zeroing of lanes 1..3 by `movss xmm, m32` (and the full clear by `xorps`)
 * is observable. */

typedef union VecTestXmm {
    uint32_t u[4];
    uint64_t q[2];
} VecTestXmm;
/* The slot is defined (weak) by src/game/game_vec_list001.c, strong in the runtime. */
extern __thread VecTestXmm g_xmm0;

#define VEC_BASE 0x10000u

static void vec_begin(void)
{
    prepare();
    memset(&memory[VEC_BASE], 0xCC, 0x800);
    g_ebx = 0xB0B0B0B0u;
    g_ebp = 0xBEBEBE01u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    for (unsigned lane = 0u; lane < 4u; lane++)
        g_xmm0.u[lane] = 0xA5A5A500u + lane;
}

static void vec_check_registers(uint32_t eax)
{
    CHECK(g_eax == eax && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
}

static void vec_check_xmm0(uint32_t low)
{
    CHECK(g_xmm0.u[0] == low && g_xmm0.u[1] == 0u && g_xmm0.u[2] == 0u && g_xmm0.u[3] == 0u);
}

static void test_t1595_vec_list001(void)
{
    /* 196170: EAX = [4E8CEC]; xorps xmm0; [756E7C] = EAX; [74FE24] = xmm0 low lane (0). */
    vec_begin();
    guest_write32(0x004E8CECu, 0x1234ABCDu);
    guest_write32(0x00756E7Cu, 0u);
    guest_write32(0x0074FE24u, 0xCCCCCCCCu);
    invoke(0x00196170u, 4u);
    vec_check_registers(0x1234ABCDu);
    vec_check_xmm0(0u);
    CHECK(guest_read32(0x00756E7Cu) == 0x1234ABCDu && guest_read32(0x0074FE24u) == 0u);
    CHECK(guest_read32(0x004E8CECu) == 0x1234ABCDu);

    /* C9F60: [7330AC] = a, [7330B0] = b; XMM0 ends as b zero-extended. */
    vec_begin();
    guest_write32(0x007330ACu, 0u);
    guest_write32(0x007330B0u, 0u);
    guest_write32(0x007330B4u, 0xCCCCCCCCu);
    guest_write32(g_esp + 4u, 0x3F000000u);
    guest_write32(g_esp + 8u, 0xBF800000u);
    invoke(0x000C9F60u, 4u);
    vec_check_registers(0x11111111u);
    vec_check_xmm0(0xBF800000u);
    CHECK(guest_read32(0x007330ACu) == 0x3F000000u && guest_read32(0x007330B0u) == 0xBF800000u);
    CHECK(guest_read32(0x007330B4u) == 0xCCCCCCCCu);
}
