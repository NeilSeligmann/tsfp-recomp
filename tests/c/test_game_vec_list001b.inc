/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1596 native tests for the 3 admitted roots of src/game/game_vec_list001b.c (included by
 * test_game_replacements.c after test_game_vec_list001.inc, reusing its vec_* helpers).
 * Expectations are hand-derived from the retail disassembly, XMM0 is seeded nonzero. */

static void test_t1596_vec_list001b(void)
{
    /* 65230: arg == [7DE30C] -> return, EAX = arg, XMM0 and [7DE300] untouched. */
    vec_begin();
    guest_write32(0x007DE30Cu, 0x77u);
    guest_write32(0x007DE300u, 0x3F800000u);
    guest_write32(g_esp + 4u, 0x77u);
    invoke(0x00065230u, 4u);
    vec_check_registers(0x77u);
    CHECK(g_xmm0.u[0] == 0xA5A5A500u && g_xmm0.u[3] == 0xA5A5A503u);
    CHECK(guest_read32(0x007DE30Cu) == 0x77u && guest_read32(0x007DE300u) == 0x3F800000u);

    /* 65230: arg != [7DE30C] -> xorps, [7DE30C] = arg, [7DE300] = 0, XMM0 all zero. */
    vec_begin();
    guest_write32(0x007DE30Cu, 0x77u);
    guest_write32(0x007DE300u, 0x3F800000u);
    guest_write32(g_esp + 4u, 0x78u);
    invoke(0x00065230u, 4u);
    vec_check_registers(0x78u);
    vec_check_xmm0(0u);
    CHECK(g_xmm0.q[0] == 0u && g_xmm0.q[1] == 0u);
    CHECK(guest_read32(0x007DE30Cu) == 0x78u && guest_read32(0x007DE300u) == 0u);

    /* 24C920: constants and zeros, XMM0 cleared. */
    vec_begin();
    guest_write32(0x0078B600u, 0xCCCCCCCCu);
    guest_write32(0x0078B664u, 0xCCCCCCCCu);
    guest_write32(0x0078B668u, 0xCCCCCCCCu);
    guest_write32(0x0078B66Cu, 0xCCCCCCCCu);
    guest_write32(0x0078B670u, 0xCCCCCCCCu);
    invoke(0x0024C920u, 4u);
    vec_check_registers(0x11111111u);
    CHECK(g_xmm0.q[0] == 0u && g_xmm0.q[1] == 0u);
    CHECK(guest_read32(0x0078B600u) == 4u && guest_read32(0x0078B664u) == 0x0Au);
    CHECK(guest_read32(0x0078B668u) == 0u && guest_read32(0x0078B66Cu) == 0u &&
          guest_read32(0x0078B670u) == 0u);

    /* 10E970: null object -> nothing written, XMM0 untouched. */
    vec_begin();
    guest_write32(0x007AD28Cu, 0u);
    guest_write32(g_esp + 4u, 0x40000000u);
    invoke(0x0010E970u, 4u);
    vec_check_registers(0u);
    CHECK(g_xmm0.u[0] == 0xA5A5A500u && g_xmm0.u[1] == 0xA5A5A501u);

    /* 10E970: object at VEC_BASE: [obj+0xAC] = a, [obj+4] |= 0x20000, EAX = obj, XMM0 = a. */
    vec_begin();
    guest_write32(0x007AD28Cu, VEC_BASE);
    guest_write32(VEC_BASE + 4u, 0x00000401u);
    guest_write32(VEC_BASE + 0xACu, 0u);
    guest_write32(g_esp + 4u, 0x40000000u);
    invoke(0x0010E970u, 4u);
    vec_check_registers(VEC_BASE);
    vec_check_xmm0(0x40000000u);
    CHECK(guest_read32(VEC_BASE + 0xACu) == 0x40000000u);
    CHECK(guest_read32(VEC_BASE + 4u) == 0x00020401u);
}
