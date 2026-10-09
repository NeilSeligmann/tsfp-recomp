/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1597 native tests for the 8 admitted roots of src/game/game_vec_list003.c (included by
 * test_game_replacements.c after test_game_vec_list001.inc, which declares VecTestXmm, the
 * g_xmm0 extern, vec_begin, vec_check_registers and vec_check_xmm0).
 * Expectations are hand-derived from the retail disassembly (GPR outputs, memory, raw XMM0
 * lanes), not by running the replacement against itself. XMM0 is seeded with a nonzero
 * pattern by vec_begin so lane zeroing is observable. */

static void vec3_check_gprs(uint32_t eax, uint32_t ecx, uint32_t edx)
{
    CHECK(g_eax == eax && g_ecx == ecx && g_edx == edx);
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
}

static void test_t1597_vec_list003(void)
{
    /* 309080 and 3090D0: four word copies, XMM0 ends with the last word. */
    vec_begin();
    guest_write32(0x00783F2Cu, 0x3F800000u);
    guest_write32(0x00783F28u, 0x40000000u);
    guest_write32(0x00783DD4u, 0x40400000u);
    guest_write32(0x00783F20u, 0x40800000u);
    invoke(0x00309080u, 4u);
    vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
    vec_check_xmm0(0x40800000u);
    CHECK(guest_read32(0x00762D58u) == 0x3F800000u && guest_read32(0x00762D5Cu) == 0x40000000u);
    CHECK(guest_read32(0x00762D60u) == 0x40400000u && guest_read32(0x00762D64u) == 0x40800000u);
    vec_begin();
    guest_write32(0x00762D58u, 0x11u);
    guest_write32(0x00762D5Cu, 0x22u);
    guest_write32(0x00762D60u, 0x33u);
    guest_write32(0x00762D64u, 0x44u);
    invoke(0x003090D0u, 4u);
    vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
    vec_check_xmm0(0x44u);
    CHECK(guest_read32(0x00783F2Cu) == 0x11u && guest_read32(0x00783F28u) == 0x22u);
    CHECK(guest_read32(0x00783DD4u) == 0x33u && guest_read32(0x00783F20u) == 0x44u);

    /* 46210: x = [475C78] stored to 4D183C, 4D1840, 4D1848. */
    vec_begin();
    guest_write32(0x004785D4u, 0xA1u);
    guest_write32(0x004785C8u, 0xA2u);
    guest_write32(0x00475C78u, 0xA3u);
    guest_write32(0x006D5EC0u, 0x77u);
    invoke(0x00046210u, 4u);
    vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
    vec_check_xmm0(0xA3u);
    CHECK(guest_read32(0x004D1838u) == 0xA1u && guest_read32(0x004D1844u) == 0xA2u);
    CHECK(guest_read32(0x004D183Cu) == 0xA3u && guest_read32(0x004D1840u) == 0xA3u);
    CHECK(guest_read32(0x004D1848u) == 0xA3u && guest_read32(0x006D5EC0u) == 0u);

    /* 425E0: bit 23 set leaves everything alone, mode 4 only loads EAX, mode 3 promotes. */
    vec_begin();
    guest_write32(0x007DE458u, 0x00800000u);
    guest_write32(0x006B98A4u, 3u);
    invoke(0x000425E0u, 4u);
    vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
    CHECK(g_xmm0.u[0] == 0xA5A5A500u && guest_read32(0x006B98A4u) == 3u);
    vec_begin();
    guest_write32(0x007DE458u, 0u);
    guest_write32(0x006B98A4u, 4u);
    invoke(0x000425E0u, 4u);
    vec3_check_gprs(4u, 0x22222222u, 0x33333333u);
    CHECK(g_xmm0.u[0] == 0xA5A5A500u && guest_read32(0x006B98A4u) == 4u);
    vec_begin();
    guest_write32(0x007DE458u, 0u);
    guest_write32(0x006B98A4u, 3u);
    guest_write32(0x00475D18u, 0x3E99999Au);
    guest_write32(0x006B98B0u, 0u);
    invoke(0x000425E0u, 4u);
    vec3_check_gprs(3u, 0x22222222u, 0x33333333u);
    vec_check_xmm0(0x3E99999Au);
    CHECK(guest_read32(0x006B98A4u) == 5u && guest_read32(0x006B98B0u) == 0x3E99999Au);

    /* 362CC0: 251 words copied, saved ESI/EDI words below ESP, EAX = ECX = 0. */
    vec_begin();
    for (uint32_t index = 0u; index < 0xFBu; index++)
        guest_write32(0x00546304u + 4u * index, 0x1000u + index);
    guest_write32(0x00773C8Cu, 0xCCCCCCCCu);
    invoke(0x00362CC0u, 4u);
    vec3_check_gprs(0u, 0u, 0x33333333u);
    vec_check_xmm0(0u);
    CHECK(guest_read32(0x007738A0u) == 0x1000u && guest_read32(0x00773C88u) == 0x1000u + 0xFAu);
    CHECK(guest_read32(0x00773C8Cu) == 0xCCCCCCCCu);
    CHECK(guest_read32(0x000000FCu) == 0x51515151u && guest_read32(0x000000F8u) == 0xD1D1D1D1u);
    CHECK(guest_read32(0x0076C0B4u) == 1u && guest_read32(0x0077388Cu) == 0u);
    CHECK(guest_read32(0x007736E0u) == 0u && guest_read32(0x00773700u) == 0u);

    /* 12C130: XMM0 ends with [4E6694], [7ABF60] = 0x4E6678. */
    vec_begin();
    guest_write32(0x00475C78u, 0xB0u);
    guest_write32(0x004E6680u, 0xB1u);
    guest_write32(0x004E6684u, 0xB2u);
    guest_write32(0x004E6688u, 0xB3u);
    guest_write32(0x004E668Cu, 0xB4u);
    guest_write32(0x004E6690u, 0xB5u);
    guest_write32(0x004E6694u, 0xB6u);
    invoke(0x0012C130u, 4u);
    vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
    vec_check_xmm0(0xB6u);
    CHECK(guest_read32(0x007ABF7Cu) == 0xB0u && guest_read32(0x007ABF64u) == 0xB1u);
    CHECK(guest_read32(0x007ABF68u) == 0xB2u && guest_read32(0x007ABF6Cu) == 0xB3u);
    CHECK(guest_read32(0x007ABF70u) == 0xB4u && guest_read32(0x007ABF74u) == 0xB5u);
    CHECK(guest_read32(0x007ABF78u) == 0xB6u && guest_read32(0x007ABF60u) == 0x004E6678u);

    /* 119AB0: a == 0 takes the default (1, 0x4E63F8), else (a, b). */
    vec_begin();
    guest_write32(g_esp + 4u, 0u);
    guest_write32(g_esp + 8u, 0x9999u);
    guest_write32(0x0047D580u, 0xC0u);
    invoke(0x00119AB0u, 4u);
    vec3_check_gprs(0u, 0u, 0x33333333u);
    vec_check_xmm0(0u);
    CHECK(guest_read32(0x007AC800u) == 1u && guest_read32(0x007AC804u) == 0x004E63F8u);
    CHECK(guest_read32(0x007AC814u) == 0xC0u && guest_read32(0x007AC818u) == 0xC0u);
    CHECK(guest_read32(0x007AC81Cu) == 0xC0u && guest_read32(0x007AC820u) == 0u);
    CHECK(guest_read32(0x007AC808u) == 0u && guest_read32(0x007AC80Cu) == 0u);
    CHECK(guest_read32(0x007AC810u) == 0u);
    vec_begin();
    guest_write32(g_esp + 4u, 7u);
    guest_write32(g_esp + 8u, 0x9999u);
    invoke(0x00119AB0u, 4u);
    vec3_check_gprs(0x9999u, 0u, 0x33333333u);
    CHECK(guest_read32(0x007AC800u) == 7u && guest_read32(0x007AC804u) == 0x9999u);

    /* 164960: mode 4 swaps the sources. EAX = -1, ECX = 0. */
    vec_begin();
    guest_write32(0x0079094Cu, 2u);
    guest_write32(0x0047CFF4u, 0xD1u);
    guest_write32(0x00475D28u, 0xD2u);
    guest_write32(0x0047D1F4u, 0xD3u);
    invoke(0x00164960u, 4u);
    vec3_check_gprs(0xFFFFFFFFu, 0u, 0x33333333u);
    vec_check_xmm0(0xD2u);
    CHECK(guest_read32(0x0074C3CCu) == 0xD1u && guest_read32(0x0074C408u) == 0xD2u);
    CHECK(guest_read32(0x0074C400u) == 0xFFFFFFFFu && guest_read32(0x0074C3E4u) == 0xFFFFFFFFu);
    CHECK(guest_read32(0x0074C3E8u) == 0u && guest_read32(0x0074C3F0u) == 0u);
    vec_begin();
    guest_write32(0x0079094Cu, 4u);
    invoke(0x00164960u, 4u);
    vec3_check_gprs(0xFFFFFFFFu, 0u, 0x33333333u);
    vec_check_xmm0(0xD3u);
    CHECK(guest_read32(0x0074C3CCu) == 0xD2u && guest_read32(0x0074C408u) == 0xD3u);
}
