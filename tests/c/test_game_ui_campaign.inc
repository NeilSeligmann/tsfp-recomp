/* SPDX-License-Identifier: GPL-3.0-or-later
 * Expectations are derived from the original instructions, not replacement outputs.
 * These pin cdecl cleanup, untouched registers and guest argument/store alias order. */
static void test_t1791_status_result(void)
{
    static const uint32_t statuses[] = {0u, 1u, 2u, 3u, 0xFFFFFFFFu, 0x80000000u};
    begin_root();
    for (unsigned i = 0u; i < sizeof statuses / sizeof statuses[0]; ++i) {
        const uint32_t status = statuses[i];
        prepare();
        guest_write32(0x1004u, 0x30000u);
        guest_write32(0x1008u, 0x30004u);
        guest_write32(0x30000u, 0xAAAAu);
        guest_write32(0x30004u, 0xBBBBu);
        guest_write32(0x689178u, status);
        guest_write32(0x689160u, 0xDEADBEEFu);
        guest_write32(0x6891ACu, 0xC001D00Du);
        sub_00032F10();
        CHECK(g_eax == status);
        CHECK(guest_read32(0x30000u) == (status == 2u ? 0xDEADBEEFu : 0xAAAAu));
        CHECK(guest_read32(0x30004u) ==
              (status == 0xFFFFFFFFu ? 0xC001D00Du : 0xBBBBu));
        CHECK(g_ecx == (status == 2u ? 0x30000u :
                       status == 0xFFFFFFFFu ? 0x30004u : 0x22222222u));
        CHECK(g_edx == (status == 2u ? 0xDEADBEEFu :
                       status == 0xFFFFFFFFu ? 0xC001D00Du : 0x33333333u));
        check_return();
    }
    /* The chosen destination aliases the status itself: the original doesn't reread it. */
    prepare();
    guest_write32(0x1004u, 0x30000u);
    guest_write32(0x1008u, 0x689178u);
    guest_write32(0x689178u, 0xFFFFFFFFu);
    guest_write32(0x6891ACu, 2u);
    sub_00032F10();
    CHECK(g_eax == 0xFFFFFFFFu && g_ecx == 0x689178u && g_edx == 2u);
    CHECK(guest_read32(0x689178u) == 2u && guest_read32(0x30000u) == 0u);
    check_return();
    /* Status 2 may overwrite the other stack argument after reading its destination. */
    prepare();
    guest_write32(0x1004u, 0x1008u);
    guest_write32(0x1008u, 0x30000u);
    guest_write32(0x689178u, 2u);
    guest_write32(0x689160u, 0xFFFFFFFFu);
    sub_00032F10();
    CHECK(g_eax == 2u && g_ecx == 0x1008u && g_edx == 0xFFFFFFFFu);
    CHECK(guest_read32(0x1008u) == 0xFFFFFFFFu);
    check_return();
    end_root(0x00032F10u);
}

/* This rejected body is exercised only by the archived-candidate control build. */
#ifdef T1791_TEST_NTH_NODE
static void test_t1791_nth_node(void)
{
    static const uint32_t counts[] = {0u, 1u, 2u, 3u, 4u, 0xFFFFFFFFu, 0x80000000u};
    static const uint32_t expected[] = {0x31000u, 0x32000u, 0x33000u, 0u, 0u,
                                       0x31000u, 0x31000u};
    begin_root();
    for (unsigned i = 0u; i < sizeof counts / sizeof counts[0]; ++i) {
        prepare();
        guest_write32(0x1004u, 0x30000u);
        guest_write32(0x300C8u, counts[i]);
        guest_write32(0x300D8u, 0x31000u);
        guest_write32(0x31010u, 0x32000u);
        guest_write32(0x32010u, 0x33000u);
        sub_000757A0();
        CHECK(g_eax == expected[i]);
        CHECK(g_ecx == ((int32_t)counts[i] > 0 ? 0u : counts[i]));
        CHECK(g_edx == 0x33333333u);
        CHECK(guest_read32(0x31010u) == 0x32000u);
        check_return();
    }
    /* Null argument and flag 0x200000 exit before touching the count/head. */
    prepare();
    sub_000757A0();
    CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    check_return();
    prepare();
    guest_write32(0x1004u, 0x30000u);
    guest_write32(0x300B8u, 0x200000u);
    guest_write32(0x300C8u, 0x7FFFFFFFu);
    guest_write32(0x300D8u, 0xFFFFFFFFu);
    sub_000757A0();
    CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    check_return();
    end_root(0x000757A0u);
}

#endif

static void test_t1791_notice_append(void)
{
    static const uint32_t indices[] = {0u, 1u, 7u, 0xFFFFFFFFu};
    begin_root();
    for (unsigned i = 0u; i < sizeof indices / sizeof indices[0]; ++i) {
        const uint32_t base = 0x30000u;
        const uint32_t record = base + indices[i] * 0x1Cu;
        prepare();
        guest_write32(0x1004u, base);
        guest_write32(0x1008u, 0xA1u);
        guest_write32(0x100Cu, 0xA2u);
        guest_write32(0x1010u, 0xA3u);
        guest_write32(base + 0xE0u, indices[i]);
        sub_00147200();
        CHECK(guest_read32(record) == 0xA1u && guest_read32(record + 4u) == 0xA3u);
        CHECK(guest_read32(record + 8u) == 0u && guest_read32(record + 0xCu) == 0xA2u);
        CHECK(guest_read32(record + 0x18u) == 0xFFFFFFFFu);
        CHECK(guest_read32(base + 0xE0u) == indices[i] + 1u);
        CHECK(g_eax == record && g_ecx == base && g_edx == 0xA2u);
        check_return();
    }
    /* Index 8 puts the first store over +E0: the second index read must see zero. */
    prepare();
    guest_write32(0x1004u, 0x30000u);
    guest_write32(0x1008u, 0u);
    guest_write32(0x100Cu, 0xA2u);
    guest_write32(0x1010u, 0xA3u);
    guest_write32(0x300E0u, 8u);
    sub_00147200();
    CHECK(g_eax == 0x30000u && g_ecx == 0x30000u && g_edx == 0xA2u);
    CHECK(guest_read32(0x300E0u) == 1u && guest_read32(0x300E4u) == 0xA3u);
    CHECK(guest_read32(0x300E8u) == 0u);
    CHECK(guest_read32(0x3000Cu) == 0xA2u && guest_read32(0x30018u) == 0xFFFFFFFFu);
    check_return();
    /* Base aliases arg1; the +4 store replaces arg2 BEFORE the original reads it. */
    prepare();
    guest_write32(0x1004u, 0x1008u);
    guest_write32(0x1008u, 0xA1u);
    guest_write32(0x100Cu, 0xA2u);
    guest_write32(0x1010u, 0xA3u);
    sub_00147200();
    CHECK(g_eax == 0x1008u && g_ecx == 0x1008u && g_edx == 0xA3u);
    CHECK(guest_read32(0x100Cu) == 0xA3u && guest_read32(0x1010u) == 0u);
    CHECK(guest_read32(0x1014u) == 0xA3u && guest_read32(0x1020u) == 0xFFFFFFFFu);
    CHECK(guest_read32(0x10E8u) == 1u);
    check_return();
    end_root(0x00147200u);
}
