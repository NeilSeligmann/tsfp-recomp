/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1789 registry-linked semantic and saved-slot alias controls. */
static void test_t1789_mapmaker(void)
{
    for (uint32_t i = 0; i < 100u; ++i) {
        prepare();
        guest_write32(0x765C30u, i);
        invoke(0x00336850u, 4u);
        CHECK(g_eax == (i != 0u) && g_ecx == i && g_edx == 0x33333333u);
        prepare();
        guest_write32(g_esp + 4u, 0x20000u);
        guest_write32(0x20084u, i);
        guest_write32(0x20080u, i % 12u);
        invoke(0x002FE450u, 4u);
        CHECK(g_eax == ((i >= 0x35u && i <= 0x37u) || i == 0x46u || i % 12u == 11u));
        CHECK(g_edx == ((i >= 0x35u && i <= 0x37u) || i == 0x46u ? 0x33333333u : i % 12u));
    }
    prepare();
    g_edi = 0xDEADBEEFu;
    memset(memory + 0x783F40u, 0xA5, 0xB8u);
    invoke(0x00308690u, 4u);
    CHECK(g_eax == 0u && g_ecx == 0u && g_edi == 0xDEADBEEFu && g_edx == 0x33333333u);
    for (uint32_t i = 0; i < 46u; ++i) CHECK(guest_read32(0x783F40u + i * 4u) == 0u);
    prepare();
    g_edi = 0xDEADBEEFu;
    g_esp = 0x783F44u;
    invoke(0x00308690u, 4u);
    CHECK(g_edi == 0u); /* bulk clear overwrites original guest saved-EDI slot */
    prepare();
    guest_write32(g_esp + 4u, 0x20000u);
    guest_write32(0x20084u, 0xFFFFFFFFu);
    guest_write32(0x20080u, 11u);
    invoke(0x002FE450u, 4u);
    CHECK(g_eax == 1u && g_edx == 11u); /* signed type comparison */
}
