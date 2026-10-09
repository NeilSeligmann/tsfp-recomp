/* SPDX-License-Identifier: GPL-3.0-or-later */
static void test_t1476_fresh_reset(void)
{
    const uint32_t objects[] = {0x2000u, 0x2001u, 0x201fu};
    const uint8_t fills[] = {0xa5u, 0xffu, 0x5au};
    for (unsigned i = 0u; i < 3u; i++) {
        prepare();
        uint8_t expected[0x202];
        memset(expected, fills[i], sizeof expected);
        memset(memory + objects[i], fills[i], sizeof expected);
        const uint32_t zeros32[] = {0x1b8u, 0x1ecu, 0x1f0u};
        for (unsigned j = 0u; j < 3u; j++) {
            memset(expected + zeros32[j], 0, 4u);
        }
        expected[0x1f6] = expected[0x1f7] = 0u;
        expected[0x1f8] = expected[0x1f9] = 0xffu;
        expected[0x1fa] = expected[0x87] = 0u;
        expected[0x1fb] = expected[0x1fc] = expected[0x1fd] = 0u;
        expected[0x7c] = expected[0x7d] = 0xffu;
        guest_write32(g_esp + 4u, objects[i]);
        guest_write32(g_esp + 8u, 0x1234abcdu);
        g_ebx = 0x44444444u; g_esi = 0x55555555u;
        g_edi = 0x66666666u; g_ebp = 0x77777777u;
        invoke(0x0008E970u, 4u);
        CHECK(g_eax == objects[i] && g_ecx == 0u && g_edx == 0xffffffffu);
        CHECK(g_ebx == 0x44444444u && g_esi == 0x55555555u);
        CHECK(g_edi == 0x66666666u && g_ebp == 0x77777777u);
        CHECK(guest_read32(0x100u) == 0x12345678u);
        CHECK(guest_read32(0x104u) == objects[i]);
        CHECK(guest_read32(0x108u) == 0x1234abcdu);
        for (unsigned j = 0u; j < sizeof expected; j++) {
            CHECK(guest_read8(objects[i] + j) == expected[j]);
        }
    }
}
