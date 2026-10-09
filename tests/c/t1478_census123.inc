/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent boundary/alias controls for T1478 census passing roots. */
static void census123_invoke(uint32_t va, uint32_t pop)
{
    g_ebx = 0x1234abcd; g_esi = 0x2345bcde;
    g_edi = 0x3456cdef; g_ebp = 0x4567def0; g_df = 0;
    invoke(va, pop);
    CHECK(g_ebx == 0x1234abcdu && g_esi == 0x2345bcdeu);
    CHECK(g_edi == 0x3456cdefu && g_ebp == 0x4567def0u);
}

static void test_t1478_census123(void)
{
    /* First object's override, second kind and descriptor flag combine independently. */
    for (uint32_t override = 0; override < 2; ++override) {
        for (uint32_t kind = 0; kind < 3; ++kind) {
            for (uint32_t flag = 0; flag < 2; ++flag) {
                prepare();
                guest_write32(g_esp + 4u, 0x3000u);
                guest_write32(g_esp + 8u, 0x4000u);
                guest_write8(0x300cu, (uint8_t)(override * 0x40u));
                guest_write32(0x4038u, kind);
                guest_write32(0x4030u, 2u);
                guest_write32(0x4f9bacu, 0x6000u);
                guest_write32(0x6544u, flag * 0x20000u);
                census123_invoke(0x001b5840u, 4u);
                CHECK(g_eax == ((override || (kind == 1u && !flag)) ? 1u : 0u));
            }
        }
    }
    /* Deferred links retain their pointer and clear both active fields. */
    for (uint32_t suppressed = 0; suppressed < 2; ++suppressed) {
        prepare();
        guest_write32(g_esp + 4u, 0x4000u);
        guest_write32(0x4008u, 1u);
        guest_write32(0x790950u, 2u);
        guest_write32(0x759ed4u, suppressed);
        guest_write32(0x4094u, 0x55667788u);
        guest_write32(0x4098u, 9u);
        guest_write32(0x409cu, 7u);
        census123_invoke(0x001b6540u, 4u);
        CHECK(g_eax == 0x4000u && g_edx == 0u);
        CHECK(guest_read32(0x4094u) == (suppressed ? 0x55667788u : 0u));
        CHECK(guest_read32(0x4098u) == (suppressed ? 9u : 0u));
        CHECK(guest_read32(0x409cu) == (suppressed ? 7u : 0x55667788u));
    }
    for (uint32_t active = 0; active < 2; ++active) {
        for (uint32_t request = 0; request < 2; ++request) {
            for (uint32_t mode = 0; mode < 2; ++mode) {
                prepare();
                guest_write32(g_esp + 4u, 0x4000u);
                guest_write32(g_esp + 8u, request);
                guest_write32(0x4080u, active);
                guest_write32(0x7de458u, mode * 0x1000u);
                census123_invoke(0x001b6a10u, 4u);
                CHECK(g_eax == ((active && (!request || mode)) ? 0u : 1u));
                CHECK(g_ecx == active);
            }
        }
    }
    /* Signed bounds and untouched ECX on the rejected index. */
    const uint32_t indices[] = {0xffffffffu, 0u, 65u, 66u, 0x80000000u};
    for (unsigned i = 0; i < sizeof indices / sizeof indices[0]; ++i) {
        prepare();
        uint32_t index = indices[i];
        guest_write32(g_esp + 4u, index);
        guest_write32(0x4f9bacu, 0x6000u);
        if (index < 66u) guest_write32(0x6000u + index * 0x29cu + 0x250u, 0x76543210u);
        uint32_t old_ecx = g_ecx;
        census123_invoke(0x001b9920u, 4u);
        CHECK(g_eax == (index < 66u ? 0x76543210u : 0xffffffffu));
        CHECK(g_ecx == (index < 66u ? 0x6000u : old_ecx));
    }
    /* Ring modulo20 includes signed negative indices and wrap19->0. */
    const int32_t ring_indices[] = {-2, -1, 0, 18, 19, 20};
    for (unsigned i = 0; i < sizeof ring_indices / sizeof ring_indices[0]; ++i) {
        prepare();
        int32_t index = ring_indices[i];
        guest_write32(g_esp + 4u, 1u);
        guest_write32(g_esp + 8u, 0xa5a5ffffu);
        guest_write32(0x75aef0u, (uint32_t)index);
        census123_invoke(0x001cce70u, 4u);
        /* Slot20 aliases the cursor and is overwritten by the final cursor store. */
        if (index != 20) {
            CHECK(guest_read8(0x75aec8u + (uint32_t)index * 2u) == 0xffu);
            CHECK(guest_read8(0x75aec9u + (uint32_t)index * 2u) == 0xfcu);
        }
        CHECK(g_ecx == 20u && g_eax == (uint32_t)((index + 1) / 20));
        CHECK(g_edx == (uint32_t)((index + 1) % 20));
        CHECK(guest_read32(0x75aef0u) == g_edx);
    }
    const uint32_t letters[] = {0u, 1u, 25u, 26u, 255u, 256u, 281u};
    for (unsigned i = 0; i < sizeof letters / sizeof letters[0]; ++i) {
        prepare();
        guest_write32(g_esp + 4u, letters[i]);
        census123_invoke(0x002561b0u, 4u);
        uint32_t index = letters[i] & 255u;
        CHECK(g_eax == (index < 26u ? 0x51c160u + index * 24u : 0u));
    }
    for (uint32_t enabled = 0; enabled < 2; ++enabled) {
        for (uint32_t mode = 0; mode < 4; ++mode) {
            prepare();
            guest_write8(0x7de402u, (uint8_t)enabled);
            guest_write8(0x7de455u, (uint8_t)mode);
            uint32_t old_ecx = g_ecx;
            census123_invoke(0x00275620u, 4u);
            CHECK(g_eax == ((enabled && mode == 2u) ? 1u : 0u));
            CHECK(g_ecx == (enabled ? (old_ecx & 0xffffff00u) | mode : old_ecx));
        }
    }
}

static void test_t1478_census123_list2(void)
{
    /* The whole field participates in OR; bits above low nibble are not masked. */
    prepare();
    guest_write32(g_esp + 4u, 0x4000u);
    guest_write32(0x4184u, 0x12345678u);
    guest_write32(0x756e58u, 0xabcdef0fu);
    census123_invoke(0x001957d0u, 4u);
    CHECK(g_eax == ((0xabcdef0fu & 0xfffffff0u) | 0x12345678u));
    CHECK(guest_read32(0x756e58u) == g_eax);
    CHECK(g_ecx == 0x4000u && g_edx == 0x12345678u);
    /* Capture second argument before clearing state; read first after clearing. */
    for (uint32_t alias = 0; alias < 2; ++alias) {
        prepare();
        if (alias) g_esp = 0x756f0cu;
        guest_write32(g_esp + 4u, 0x98765432u);
        guest_write32(g_esp + 8u, 0x12345678u);
        census123_invoke(0x001b2880u, 4u);
        CHECK(guest_read32(0x757080u) == (alias ? 0u : 0x98765432u));
        CHECK(guest_read32(0x757084u) == 0x12345678u);
        CHECK(guest_read32(0x759d18u) == 1u && g_eax == 0u);
        CHECK(g_ecx == 0x12345678u);
    }
    for (uint32_t index = 0; index < 3; ++index) {
        prepare();
        guest_write32(0x756f10u, index);
        guest_write32(0x757080u + index * 0x170u, 0x12340000u + index);
        guest_write32(0x757084u + index * 0x170u, 0x56780000u + index);
        census123_invoke(0x001b2930u, 4u);
        CHECK(g_ecx == index * 0x170u);
        CHECK(g_eax == 0x12340000u + index && g_edx == 0x56780000u + index);
    }
    const uint32_t indices[] = {0xffffffffu, 0u, 65u, 66u, 0x80000000u};
    for (unsigned i = 0; i < sizeof indices / sizeof indices[0]; ++i) {
        prepare();
        uint32_t index = indices[i];
        guest_write32(g_esp + 4u, index);
        guest_write32(0x4f9bacu, 0x6000u);
        if (index < 66u) guest_write32(0x6000u + index * 0x29cu + 0x24cu, 0x76543210u);
        uint32_t old_ecx = g_ecx;
        census123_invoke(0x001b98f0u, 4u);
        CHECK(g_eax == (index < 66u ? 0x76543210u : 0xffffffffu));
        CHECK(g_ecx == (index < 66u ? 0x6000u : old_ecx));
    }
    prepare();
    memset(memory + 0x75a030u, 0x5a, 0xc3cu);
    guest_write32(0x75a02cu, 0x12345678u);
    guest_write32(0x75ac6cu, 0x87654321u);
    census123_invoke(0x001cb240u, 4u);
    for (uint32_t offset = 0; offset < 0xc3cu; ++offset) CHECK(guest_read8(0x75a030u + offset) == 0u);
    CHECK(guest_read32(0x75a02cu) == 0x12345678u);
    CHECK(guest_read32(0x75ac6cu) == 0x87654321u);
    CHECK(g_eax == 0u && g_ecx == 0u);
    const uint32_t levels[] = {100u, 101u, 102u};
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; ++i) {
        prepare();
        guest_write32(0x79094cu, levels[i]);
        guest_write32(0x75ccb8u, 0x12345678u);
        census123_invoke(0x00233f90u, 4u);
        CHECK(g_eax == (levels[i] == 101u ? 0u : 0x12345678u));
        CHECK(g_ecx == 0x12345678u);
    }
    const uint32_t sequences[] = {0u, 1u, 0xffffffffu};
    for (unsigned i = 0; i < sizeof sequences / sizeof sequences[0]; ++i) {
        for (uint32_t alias = 0; alias < 2; ++alias) {
            prepare();
            uint32_t object = alias ? 0x519520u : 0x4000u;
            guest_write32(g_esp + 4u, object);
            guest_write32(0x519750u, sequences[i]);
            census123_invoke(0x00240290u, 4u);
            CHECK(g_eax == object && g_ecx == sequences[i]);
            CHECK(guest_read32(0x519750u) == sequences[i] + 1u);
            CHECK(guest_read32(object + 0x230u) == (alias ? sequences[i] + 1u : sequences[i]));
        }
    }
    prepare(); guest_write32(g_esp + 4u, 0u);
    uint32_t old_ecx = g_ecx;
    census123_invoke(0x00240290u, 4u);
    CHECK(g_eax == 0u && g_ecx == old_ecx);
    for (uint32_t level = 23u; level <= 25u; ++level) {
        prepare(); guest_write32(0x79094cu, level);
        census123_invoke(0x00240580u, 4u);
        CHECK(g_ecx == level && g_eax == (level == 24u ? 1u : 0u));
    }
    for (uint32_t mode = 9u; mode <= 11u; ++mode) {
        prepare(); guest_write32(0x78b594u, mode);
        census123_invoke(0x002533e0u, 4u);
        CHECK(g_ecx == mode && g_eax == (mode == 10u ? 1u : 0u));
    }
    for (uint32_t kind = 2u; kind <= 4u; ++kind) {
        prepare();
        guest_write32(g_esp + 4u, 0x4000u);
        guest_write32(0x407cu, 0x5000u);
        guest_write32(0x5020u, kind);
        guest_write32(0x59d0u, 7u);
        census123_invoke(0x00264720u, 4u);
        CHECK(g_eax == 0x5000u);
        CHECK(guest_read32(0x59d0u) == (kind == 3u ? 1u : 7u));
    }
}

static int census123_registered(uint32_t va)
{
    size_t count; const game_replacement *table = game_replacement_table(&count);
    for (size_t i = 0; i < count; ++i) if (table[i].va == va) return 1;
    return 0;
}

static void test_t1478_census123_list3(void)
{
    if (census123_registered(0x00190d60u)) {
        const uint32_t bounds[] = {0xffffffffu, 0u, 1u, 4u};
        for (unsigned i = 0; i < sizeof bounds / sizeof bounds[0]; ++i) {
            prepare();
            guest_write32(0x5236c4u, bounds[i]);
            guest_write32(0x74e918u, 0xffffffffu);
            guest_write32(0x74e91cu, 0u);
            guest_write32(0x74e920u, 0x7fffffffu);
            uint32_t old_edx = g_edx;
            census123_invoke(0x00190d60u, 4u);
            CHECK(g_eax == (bounds[i] == 4u ? 3u : 1u));
            CHECK(g_ecx == (bounds[i] == 4u ? 4u : 1u));
            CHECK(g_edx == (bounds[i] == 4u ? 1u : old_edx));
        }
    }
    if (census123_registered(0x001b2fd0u)) {
        for (uint32_t position = 0; position <= 27u; ++position) {
            prepare();
            for (uint32_t index = 0; index < 27u; ++index) guest_write32(0x4ea380u + index * 4u, 100u + index);
            guest_write32(g_esp + 4u, 100u + position);
            census123_invoke(0x001b2fd0u, 4u);
            CHECK(g_eax == (position < 27u ? 1u : 0u));
            CHECK(g_ecx == 100u + position);
        }
    }
    if (census123_registered(0x001b52b0u)) {
        prepare();
        memset(memory + 0x4000u, 0xa5, 0xa0u);
        guest_write8(0x3fffu, 0x5a); guest_write8(0x40a0u, 0x5a);
        guest_write32(g_esp + 4u, 0x4000u);
        census123_invoke(0x001b52b0u, 4u);
        for (uint32_t offset = 0; offset < 0xa0u; ++offset) {
            uint8_t expected = 0;
            if (offset == 0u || offset == 1u || offset == 0x57u) expected = 1;
            if (offset == 0x54u) expected = 0x65;
            if (offset == 0x55u) expected = 9;
            if (offset == 0x58u) expected = 0x0a;
            if (offset == 0x59u) expected = 0x84;
            if (offset == 0x5au) expected = 0x0d;
            if ((offset >= 0x90u && offset <= 0x93u) || offset == 0x9bu) expected = 0xff;
            CHECK(guest_read8(0x4000u + offset) == expected);
        }
        CHECK(guest_read8(0x3fffu) == 0x5a && guest_read8(0x40a0u) == 0x5a);
        CHECK(g_eax == 0xffffffffu && g_ecx == 1u && g_edx == 0x4000u);
    }
    if (census123_registered(0x001cd090u)) {
        for (uint32_t mask = 0; mask < 3u; ++mask) {
            prepare();
            uint32_t input = mask == 0u ? 0u : mask == 1u ? 0x1fffu : 0x1555u;
            for (uint32_t i = 0; i < 13u; ++i) guest_write32(0x4fcc84u + i * 0x1cu, 1u << i);
            guest_write32(g_esp + 4u, input);
            census123_invoke(0x001cd090u, 4u);
            CHECK(g_eax == input && g_ecx == 13u && g_edx == 0x4fcdf0u);
        }
    }
    if (census123_registered(0x00227100u)) {
        for (uint32_t alias = 0; alias < 2; ++alias) {
            prepare();
            uint32_t object = alias ? g_esp + 4u : 0x4000u;
            memset(memory + object, 0xa5, 124u);
            guest_write32(g_esp + 4u, object); guest_write32(g_esp + 8u, 0x12345678u);
            census123_invoke(0x00227100u, 4u);
            for (uint32_t i = 0; i < 31u; ++i) CHECK(guest_read32(object + i * 4u) == (i < 11u ? 0x12345678u : 0u));
            CHECK(g_eax == object && g_ecx == 0x12345678u && g_edx == 0u);
        }
    }
    if (census123_registered(0x00227a70u)) {
        prepare(); guest_write32(g_esp + 4u, 1u); guest_write32(g_esp + 8u, 0u);
        guest_write32(0x75c138u, 0x76543210u);
        census123_invoke(0x00227a70u, 4u); CHECK(g_eax == 0x76543210u);
        prepare(); guest_write32(g_esp + 4u, 3u); guest_write32(g_esp + 8u, 1u);
        guest_write32(0x78cec8u, 3u); guest_write32(0x78ceccu, 0x4000u); guest_write32(0x75c14cu, 0x5000u);
        const uint32_t order[] = {0u, 2u, 1u};
        for (uint32_t i = 0; i < 3u; ++i) guest_write32(0x5000u + i * 4u, order[i]);
        guest_write8(0x4008u, 3u); guest_write8(0x4054u, 7u); guest_write8(0x40a0u, 3u);
        census123_invoke(0x00227a70u, 4u); CHECK(g_eax == 2u && g_ecx == 3u && g_edx == 3u);
    }
    if (census123_registered(0x00227e40u)) {
        /* Updating byte+B aliases the dynamic bound low byte:2 becomes5. */
        prepare(); guest_write32(0x78cec8u, 2u); guest_write32(0x78ceccu, 0x78cebdu);
        guest_write32(0x75c14cu, 0x5000u);
        for (uint32_t i = 0; i < 5u; ++i) guest_write32(0x5000u + i * 4u, 0u);
        guest_write8(0x78cec6u, 2u);
        census123_invoke(0x00227e40u, 4u);
        CHECK(guest_read32(0x78cec8u) == 5u && g_eax == 5u && g_edx == 5u);
        CHECK(g_ecx == 0x78ce05u);
    }
    if (census123_registered(0x0022d790u)) {
        for (uint32_t found = 0; found < 4u; ++found) {
            prepare(); guest_write32(g_esp + 4u, 0x4000u);
            guest_write32(0x4020u, 3u); guest_write32(0x401cu, 0x5000u);
            for (uint32_t i = 0; i < 3u; ++i) guest_write8(0x5001u + i * 20u, i == found ? 2u : 0u);
            census123_invoke(0x0022d790u, 4u);
            CHECK(g_eax == (found < 3u ? 1u : 0u));
            CHECK(g_ecx == 0x5000u + (found < 3u ? found : 3u) * 20u && g_edx == 3u);
        }
    }
    if (census123_registered(0x0022dc60u)) {
        for (uint32_t alias = 0; alias < 2u; ++alias) {
            prepare(); uint32_t dest = alias ? 0x7b0c7cu : 0x7000u;
            guest_write32(g_esp + 4u, dest); guest_write32(0x7de470u, 7u);
            guest_write32(0x7b0c7cu, 0x4000u); guest_write32(0x4000u, 1u); guest_write32(0x5000u, 2u);
            guest_write32(0x75cc90u, 0x6000u);
            guest_write32(0x6040u, 0x5000u); guest_write32(0x6048u, 0x11111111u); guest_write32(0x6050u, 0x22222222u);
            guest_write32(0x607cu, 0x33333333u); guest_write32(0x6084u, 0x44444444u);
            census123_invoke(0x0022dc60u, 4u);
            CHECK(guest_read32(dest) == 0x5000u);
            CHECK(guest_read32(dest + 4u) == (alias ? 0x33333333u : 0x11111111u));
            CHECK(guest_read32(dest + 8u) == (alias ? 0x44444444u : 0x22222222u));
            CHECK(g_eax == 1u && g_ecx == dest);
        }
    }
    for (uint32_t kind = 0; kind < 2u; ++kind) {
        uint32_t va = kind ? 0x00230570u : 0x00230530u;
        if (!census123_registered(va)) continue;
        for (uint32_t found = 0; found < 4u; ++found) {
            prepare(); guest_write32(g_esp + 4u, 21u); guest_write32(0x513d58u, 0x4000u);
            guest_write32(0x4020u, 3u); guest_write32(0x401cu, 0x5000u);
            for (uint32_t i = 0; i < 3u; ++i) {
                guest_write8(0x5000u + i * 20u, !kind && i == found ? 0x80u : 0u);
                guest_write8(0x5001u + i * 20u, kind && i == found ? 1u : 0u);
            }
            census123_invoke(va, 4u);
            CHECK(g_eax == (found < 3u ? 0x5000u + found * 20u : 0u));
            CHECK(g_ecx == (found < 3u ? found : 3u) && g_edx == 3u);
        }
    }
    if (census123_registered(0x0023fe50u)) {
        prepare(); memset(memory + 0x75daf0u, 0x5a, 4000u);
        census123_invoke(0x0023fe50u, 4u);
        for (uint32_t i = 0; i < 250u; ++i) {
            CHECK(guest_read32(0x75daf0u + i * 16u) == 0u);
            CHECK(guest_read32(0x75daf4u + i * 16u) == 0u);
            CHECK(guest_read32(0x75daf8u + i * 16u) == 0u);
            CHECK(guest_read32(0x75dafcu + i * 16u) == 0x5a5a5a5au);
        }
        CHECK(g_eax == 0x75ea94u && g_ecx == 0u);
    }
    if (census123_registered(0x00240020u)) {
        for (uint32_t stale = 0; stale < 2u; ++stale) {
            prepare(); guest_write32(g_esp + 4u, 0x12345678u);
            for (uint32_t i = 0; i < 150u; ++i) guest_write32(0x75ce68u + i * 8u, i);
            guest_write32(0x75d310u, 0x12345678u); guest_write32(0x75d314u, 0x5000u);
            guest_write32(0x5000u, 0x800000u); guest_write32(0x72f5e8u, 0x6000u);
            guest_write32(0x65fcu, stale ? 0u : 0x800000u);
            census123_invoke(0x00240020u, 4u);
            CHECK(g_eax == (stale ? 0u : 0x65d0u));
            CHECK(g_ecx == 0x65d0u && g_edx == 0x800000u);
            CHECK(guest_read32(0x5000u) == (stale ? 0u : 0x800000u));
        }
    }
    if (census123_registered(0x00242ee0u)) {
        for (uint32_t kind = 8u; kind <= 12u; ++kind) {
            prepare(); guest_write32(g_esp + 4u, 0x4000u);
            guest_write32(0x407cu, 0x5000u); guest_write32(0x500cu, 0u); guest_write32(0x5010u, kind);
            guest_write32(0x4004u, 0x6000u); guest_write32(0x6030u, 2u); guest_write32(0x5197e8u, 0u);
            census123_invoke(0x00242ee0u, 4u);
            CHECK(g_eax == ((kind == 9u || kind == 12u) ? 1u : 0u));
        }
    }
    if (census123_registered(0x00246dc0u)) {
        const uint32_t counters[] = {0u, 28u, 29u, 0xffffffffu};
        for (unsigned i = 0; i < sizeof counters / sizeof counters[0]; ++i) {
            prepare(); guest_write32(g_esp + 4u, 0x4000u); guest_write32(0x4004u, 0x5000u);
            guest_write32(0x75eae0u, counters[i]); guest_write32(0x75eaecu, 0u); guest_write32(0x75eaf0u, 0u);
            guest_write32(0x51acb4u, 0x10u); guest_write32(0x51ad64u, 0x20u); guest_write32(0x5030u, 7u);
            census123_invoke(0x00246dc0u, 4u);
            if (counters[i] == 29u) {
                CHECK(guest_read32(0x75eae0u) == 0u && guest_read32(0x75eaecu) == 1u && guest_read32(0x75eaf0u) == 1u);
                CHECK(guest_read32(0x5030u) == 0x40030u);
            } else CHECK(guest_read32(0x75eae0u) == counters[i] + 1u && guest_read32(0x5030u) == 7u);
        }
    }
    if (census123_registered(0x0024c6a0u)) {
        prepare(); guest_write32(0x79094cu, 1u); guest_write32(0x78b6fcu, 3u); guest_write32(0x78b718u, 0x4000u);
        guest_write32(0x4000u, 1u); guest_write32(0x4140u, 2u); guest_write32(0x4280u, 3u);
        census123_invoke(0x0024c6a0u, 4u);
        CHECK(g_eax == 1u && g_ecx == 3u && g_edx == 0x43c0u);
        prepare(); guest_write32(0x79094cu, 2u);
        census123_invoke(0x0024c6a0u, 4u);
        CHECK(g_eax == 0u && g_ecx == 0xffffffc6u);
    }
    if (census123_registered(0x00261c70u)) {
        for (uint32_t mode = 9u; mode <= 10u; ++mode) {
            for (uint32_t active = 0; active < 2u; ++active) {
                for (uint32_t kind = 1u; kind <= 3u; kind += 2u) {
                    prepare(); guest_write32(g_esp + 4u, 0x4000u); guest_write32(g_esp + 8u, 7u);
                    guest_write32(0x407cu, 0x5000u); guest_write8(0x7de455u, (uint8_t)mode);
                    guest_write32(0x5010u, active * 0x80000u); guest_write32(0x5018u, active * 0x80000u);
                    guest_write32(0x5014u, 0x12345678u); guest_write32(0x501cu, 0x87654321u);
                    guest_write32(0x5020u, kind); guest_write32(0x59ecu, 5u); guest_write32(0x59d0u, 9u);
                    uint32_t frame = g_esp;
                    census123_invoke(0x00261c70u, 4u);
                    CHECK(guest_read32(frame - 4u) == (mode == 10u ? 0x12345678u : 0x87654321u));
                    CHECK(guest_read32(0x59ecu) == (active ? 7u : 5u));
                    CHECK(guest_read32(0x59d0u) == (active && kind != 1u ? 1u : 9u));
                    CHECK(g_eax == 0x5000u && g_ecx == (active ? 1u : 0u));
                    CHECK(g_edx == (active ? kind : 0u));
                }
            }
        }
    }
    for (uint32_t returns_type = 0; returns_type < 2u; ++returns_type) {
        uint32_t va = returns_type ? 0x0026e100u : 0x0026d690u;
        if (!census123_registered(va)) continue;
        const uint32_t types[] = {7u, 12u, 13u};
        for (unsigned i = 0; i < sizeof types / sizeof types[0]; ++i) {
            prepare(); guest_write32(g_esp + 4u, 0x4000u); guest_write32(g_esp + 8u, 0x6000u);
            guest_write32(0x407cu, 0x5000u); guest_write32(0x5aa0u, 3u);
            guest_write32(0x6030u, types[i]); guest_write32(0x6034u, 2u);
            guest_write32(0x600cu, 77u); guest_write32(0x6018u, 99u);
            census123_invoke(va, 4u);
            uint32_t pointer = types[i] == 12u ? 0x600cu : types[i] == 13u ? 0x6018u : 0x6030u;
            uint32_t expected_type = types[i] == 12u ? 77u : types[i] == 13u ? 99u : 7u;
            CHECK(g_eax == (returns_type ? expected_type : pointer));
            CHECK(g_ecx == 0x6000u && g_edx == (types[i] == 13u ? 9u : 0x5000u));
        }
    }
}
