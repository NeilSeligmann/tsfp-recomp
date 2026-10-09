/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hand-derived original memory/register/lane contracts, including aliased source. */
static void test_t1478_vector_census(void)
{
    const int32_t counts[] = {-1, 0, 17, 18, 19};
    for (unsigned k = 0; k < 5; ++k) for (unsigned alias = 0; alias < 2; ++alias) {
        prepare();
        const uint32_t slot = 0x78B2E0u + (uint32_t)counts[k] * 32u;
        const uint32_t input = alias ? slot : 0x20000u;
        for (unsigned i = 0; i < 8; ++i) guest_write32(slot + 4u*i, 0xCCCCCCCCu);
        for (unsigned i = 0; i < 6; ++i) guest_write32(input + 4u*i, 0xAAA00000u + i);
        guest_write32(0x78B520u, (uint32_t)counts[k]);
        guest_write32(g_esp + 4u, input);
        g_esi = 0x51515151u;
        for (unsigned i = 0; i < 4; ++i) g_xmm0.u[i] = 0xDEADBEEFu;
        invoke(0x0025F9C0u, 4u);
        CHECK(g_esi == 0x51515151u);
        if (counts[k] >= 18) {
            CHECK(g_eax == 0x11111111u && g_ecx == 0x22222222u);
            CHECK(g_edx == (uint32_t)counts[k]);
            for (unsigned i = 0; i < 4; ++i) CHECK(g_xmm0.u[i] == 0xDEADBEEFu);
            CHECK(guest_read32(0x78B520u) == (uint32_t)counts[k]);
        } else {
            const uint32_t expected[] = {0xAAA00000u, 0xAAA00003u, 0xAAA00004u,
                                        alias ? 0xAAA00004u : 0xAAA00002u,
                                        0u, 0u, 0u, 0xAAA00005u};
            CHECK(g_eax == slot && g_ecx == 0u && g_edx == (uint32_t)counts[k] + 1u);
            CHECK(guest_read32(0x78B520u) == (uint32_t)counts[k] + 1u);
            CHECK(guest_read32(0xFCu) == 0x51515151u);
            for (unsigned i = 0; i < 8; ++i) CHECK(guest_read32(slot + 4u*i) == expected[i]);
            for (unsigned i = 0; i < 4; ++i) CHECK(g_xmm0.u[i] == 0u);
        }
    }
    const uint32_t flags[] = {0u, 1u, 0x10000u, 0x10001u};
    const uint32_t states[] = {0u, 4u, 5u};
    for (unsigned f = 0; f < 4; ++f) for (unsigned s = 0; s < 3; ++s) {
        prepare();
        guest_write32(g_esp + 4u, 0x20000u);
        guest_write32(0x2007Cu, 0x21000u);
        guest_write32(0x21014u, flags[f]);
        guest_write32(0x21020u, states[s]);
        guest_write32(0x219D0u, 0xCCCCCCCCu);
        guest_write32(0x219E0u, 0xDDDDDDDDu);
        guest_write32(0x475C78u, 0x7FC12345u);
        for (unsigned i = 0; i < 4; ++i) g_xmm0.u[i] = 0xDEADBEEFu;
        invoke(0x002649D0u, 4u);
        CHECK(g_eax == 0x21000u);
        const unsigned writes = !(flags[f] & 0x10000u) && states[s] != 4u;
        CHECK(g_ecx == ((flags[f] & 0x10000u) ? 0x10000u : 4u));
        CHECK(g_edx == ((flags[f] & 0x10000u) ? 0x10000u : states[s]));
        CHECK(guest_read32(0x219D0u) == (writes ? 4u : 0xCCCCCCCCu));
        CHECK(guest_read32(0x219E0u) == (writes ? 0x7FC12345u : 0xDDDDDDDDu));
        for (unsigned i = 0; i < 4; ++i)
            CHECK(g_xmm0.u[i] == (writes ? (i ? 0u : 0x7FC12345u) : 0xDEADBEEFu));
    }
}
