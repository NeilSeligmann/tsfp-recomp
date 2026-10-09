/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1619 native tests for the 9 admitted leftover pilot leaves of src/game/game_pilot_left.c
 * (included by test_game_replacements.c). Expectations are hand-derived from the retail
 * disassembly (docs/t-pilot-leftover-draft.md), not by running the replacement against itself:
 * result in EAX, guest memory, ECX/EDX untouched and the cdecl stack pop of 4. The tests of the
 * 16 rejected drafts are kept with them in docs/data/t-pilot-leftover/. */

static uint32_t left_call(uint32_t va, uint32_t first, uint32_t second, uint32_t third,
                          uint32_t fourth, uint32_t fifth)
{
    prepare();
    guest_write32(g_esp + 4u, first);
    guest_write32(g_esp + 8u, second);
    guest_write32(g_esp + 12u, third);
    guest_write32(g_esp + 16u, fourth);
    guest_write32(g_esp + 20u, fifth);
    invoke_ecx_edx_scratch(va, 4u);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
    return g_eax;
}

/* A function registered as `void` reports EAX as scratch too. */
static void left_call_void(uint32_t va, uint32_t first, uint32_t second, uint32_t third)
{
    prepare();
    guest_write32(g_esp + 4u, first);
    guest_write32(g_esp + 8u, second);
    guest_write32(g_esp + 12u, third);
    invoke_with_scratch(va, 4u, GAME_SCRATCH_EAX | GAME_SCRATCH_ECX | GAME_SCRATCH_EDX);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
}

static void test_left_record_bytes(void)
{
    /* 0x0016B790: record (index & 0xFFFF) * 512 of the table at 0x74C458, bytes +0x1C9/+0x1CA. */
    guest_write32(0x0074C458u, 0x4000u);
    guest_write8(0x4600u + 0x1C8u, 0x77u);
    guest_write8(0x4600u + 0x1CBu, 0x88u);
    CHECK(left_call(0x0016B790u, 0x10003u, 0x1ABu, 0x2CDu, 0u, 0u) == 0x4600u);
    CHECK(guest_read8(0x4600u + 0x1C9u) == 0xABu && guest_read8(0x4600u + 0x1CAu) == 0xCDu);
    CHECK(guest_read8(0x4600u + 0x1C8u) == 0x77u && guest_read8(0x4600u + 0x1CBu) == 0x88u);

    /* 0x00359E30: record `slot` (0x30C bytes) at 0x774180, selector 0 -> +0x2FA, 1 -> +0x2FB,
     * other selectors store nothing, EAX = record. */
    const uint32_t rec = 0x00774180u + 2u * 0x30Cu;
    guest_write8(rec + 0x2FAu, 0x11u);
    guest_write8(rec + 0x2FBu, 0x22u);
    CHECK(left_call(0x00359E30u, 0u, 0x1A5u, 2u, 0u, 0u) == rec);
    CHECK(guest_read8(rec + 0x2FAu) == 0xA5u && guest_read8(rec + 0x2FBu) == 0x22u);
    CHECK(left_call(0x00359E30u, 1u, 0x2B6u, 2u, 0u, 0u) == rec);
    CHECK(guest_read8(rec + 0x2FAu) == 0xA5u && guest_read8(rec + 0x2FBu) == 0xB6u);
    CHECK(left_call(0x00359E30u, 2u, 0x77u, 2u, 0u, 0u) == rec);
    CHECK(guest_read8(rec + 0x2FAu) == 0xA5u && guest_read8(rec + 0x2FBu) == 0xB6u);
}

static void test_left_stores(void)
{
    /* 0x00246D20: [75EAF8] = arg, [75EB00] = 1, [75EAE4] = 1 only when it was 0, EAX = 1. */
    guest_write32(0x0075EAE4u, 0u);
    CHECK(left_call(0x00246D20u, 0x1234u, 0u, 0u, 0u, 0u) == 1u);
    CHECK(guest_read32(0x0075EAF8u) == 0x1234u && guest_read32(0x0075EB00u) == 1u);
    CHECK(guest_read32(0x0075EAE4u) == 1u);
    guest_write32(0x0075EAE4u, 7u);
    guest_write32(0x0075EB00u, 0u);
    CHECK(left_call(0x00246D20u, 0x4321u, 0u, 0u, 0u, 0u) == 1u);
    CHECK(guest_read32(0x0075EAF8u) == 0x4321u && guest_read32(0x0075EB00u) == 1u);
    CHECK(guest_read32(0x0075EAE4u) == 7u);

    /* 0x0025B770: a,c,d,e,b to 0x78B540.., EAX = e, pop 4 (cdecl, five arguments). */
    CHECK(left_call(0x0025B770u, 1u, 2u, 3u, 4u, 5u) == 5u);
    CHECK(guest_read32(0x0078B540u) == 1u && guest_read32(0x0078B544u) == 3u);
    CHECK(guest_read32(0x0078B548u) == 4u && guest_read32(0x0078B54Cu) == 5u);
    CHECK(guest_read32(0x0078B550u) == 2u);

    /* 0x00352750: [76B10C] non-zero -> three dwords cleared, EAX = 0. */
    guest_write32(0x0076B108u, 7u);
    guest_write32(0x0076B10Cu, 0u);
    guest_write32(0x0076B0FCu, 8u);
    CHECK(left_call(0x00352750u, 0u, 0u, 0u, 0u, 0u) == 0u);
    CHECK(guest_read32(0x0076B108u) == 7u && guest_read32(0x0076B0FCu) == 8u);
    guest_write32(0x0076B10Cu, 5u);
    CHECK(left_call(0x00352750u, 0u, 0u, 0u, 0u, 0u) == 0u);
    CHECK(guest_read32(0x0076B108u) == 0u && guest_read32(0x0076B10Cu) == 0u);
    CHECK(guest_read32(0x0076B0FCu) == 0u);

    /* 0x003561B0: both non-zero -> [dest] = value and [774168]++, else nothing. */
    guest_write32(0x2000u, 0x11u);
    guest_write32(0x00774168u, 40u);
    left_call_void(0x003561B0u, 0x2000u, 9u, 0u);
    CHECK(guest_read32(0x2000u) == 9u && guest_read32(0x00774168u) == 41u);
    left_call_void(0x003561B0u, 0x2000u, 0u, 0u);
    CHECK(guest_read32(0x2000u) == 9u && guest_read32(0x00774168u) == 41u);
    left_call_void(0x003561B0u, 0u, 5u, 0u);
    CHECK(guest_read32(0x00774168u) == 41u);

    /* 0x0035E5F0: non-zero -> [76BBBC]++, zero -> [76BBC4]++, always [76BBC0]--, EAX = arg. */
    guest_write32(0x0076BBBCu, 1u);
    guest_write32(0x0076BBC0u, 10u);
    guest_write32(0x0076BBC4u, 100u);
    CHECK(left_call(0x0035E5F0u, 3u, 0u, 0u, 0u, 0u) == 3u);
    CHECK(guest_read32(0x0076BBBCu) == 2u && guest_read32(0x0076BBC0u) == 9u);
    CHECK(guest_read32(0x0076BBC4u) == 100u);
    CHECK(left_call(0x0035E5F0u, 0u, 0u, 0u, 0u, 0u) == 0u);
    CHECK(guest_read32(0x0076BBBCu) == 2u && guest_read32(0x0076BBC0u) == 8u);
    CHECK(guest_read32(0x0076BBC4u) == 101u);
}

static void test_left_block_copy_and_fill(void)
{
    /* 0x00358E60: block byte 0 = number 2, the 0x1F dwords after it go to the pointer at
     * table + 0x1584 * 2 + 0x1500, dword of record 2 (0x124 bytes at 0x77728C) gets bit 4. */
    guest_write32(0x007B0C48u, 0x10000u);
    guest_write32(0x10000u + 2u * 0x1584u + 0x1500u, 0x20000u);
    guest_write8(0x2000u, 2u);
    for (uint32_t i = 0u; i < 0x1Fu; i++) {
        guest_write32(0x2001u + i * 4u, 0x100u + i);
        guest_write32(0x20000u + i * 4u, 0u);
    }
    guest_write32(0x20000u + 0x1Fu * 4u, 0xDEADu);
    guest_write32(0x0077728Cu + 2u * 0x124u, 0x3u);
    CHECK(left_call(0x00358E60u, 0x2000u, 0u, 0u, 0u, 0u) == 0x0077728Cu + 2u * 0x124u);
    CHECK(guest_read32(0x20000u) == 0x100u && guest_read32(0x20000u + 0x1Eu * 4u) == 0x11Eu);
    CHECK(guest_read32(0x20000u + 0x1Fu * 4u) == 0xDEADu);
    CHECK(guest_read32(0x0077728Cu + 2u * 0x124u) == 0x13u);

    /* 0x0038F4B5: (count & 0xFFFF) / 2 dword stores of the doubled low word of the value, plus
     * one word store for an odd count; a zero count writes nothing. */
    for (uint32_t i = 0u; i < 16u; i++) guest_write8(0x2000u + i, 0x55u);
    left_call_void(0x0038F4B5u, 0x2000u, 0xABCD1234u, 0x10003u);
    CHECK(guest_read32(0x2000u) == 0x12341234u);
    CHECK(guest_read8(0x2004u) == 0x34u && guest_read8(0x2005u) == 0x12u);
    CHECK(guest_read8(0x2006u) == 0x55u);
    for (uint32_t i = 0u; i < 16u; i++) guest_write8(0x2000u + i, 0x55u);
    left_call_void(0x0038F4B5u, 0x2000u, 0x1111u, 0u);
    left_call_void(0x0038F4B5u, 0x2000u, 0x1111u, 0x10000u);
    CHECK(guest_read32(0x2000u) == 0x55555555u);
    left_call_void(0x0038F4B5u, 0x2000u, 0xFFFF0A0Bu, 4u);
    CHECK(guest_read32(0x2000u) == 0x0A0B0A0Bu && guest_read32(0x2004u) == 0x0A0B0A0Bu);
    CHECK(guest_read32(0x2008u) == 0x55555555u);
}

static void test_t1619_pilot_left(void)
{
    test_left_record_bytes();
    test_left_stores();
    test_left_block_copy_and_fill();
}
