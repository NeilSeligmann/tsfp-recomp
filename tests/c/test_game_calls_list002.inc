/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1541 native tests for src/game/game_calls_list002.c (included by test_game_replacements.c).
 * Expectations are hand-derived from the retail disassembly (register outputs, saved stack
 * slots, call arguments and return VAs), not by running the replacement against itself.
 * Nested guest callees are recorded stubs reached through recomp_lookup while l2_active. */

typedef struct {
    uint32_t va, pops, eax, ecx, edx, eax_sel8;
    unsigned split8;
} l2_stub_def;

typedef struct {
    uint32_t va, ret_va, esp_seen, ecx_in, args[3];
} l2_log_entry;

#define L2_STUBS 18u
static l2_stub_def l2_stubs[L2_STUBS] = {
    {0x003BB890u, 0u}, {0x0009E300u, 0u}, {0x00153BB0u, 0u}, {0x0037E89Eu, 1u}, {0x000B7630u, 0u},
    {0x000DB450u, 0u}, {0x00302A20u, 0u}, {0x0009E2C0u, 0u}, {0x0009E2D0u, 0u}, {0x0009E380u, 0u},
    {0x003BB650u, 0u}, {0x00240560u, 0u}, {0x0032A5B0u, 0u}, {0x003C8960u, 0u}, {0x0006BB90u, 0u},
    {0x0009CC40u, 0u}, {0x001BAE40u, 0u}, {0x001BADD0u, 0u}};
static l2_log_entry l2_log[32];
static unsigned l2_log_count;

static void l2_hit(unsigned index)
{
    l2_stub_def *stub = &l2_stubs[index];
    l2_log_entry *entry = &l2_log[l2_log_count < 32u ? l2_log_count : 31u];
    l2_log_count++;
    entry->va = stub->va;
    entry->ret_va = guest_read32(g_esp);
    entry->esp_seen = g_esp;
    entry->ecx_in = g_ecx;
    for (unsigned i = 0; i < 3u; i++) entry->args[i] = game_stack_arg(i);
    g_eax = (stub->split8 && entry->args[0] == 8u) ? stub->eax_sel8 : stub->eax;
    g_ecx = stub->ecx;
    g_edx = stub->edx;
    g_esp += 4u + 4u * stub->pops;
}

#define L2_STUB_FN(N) static void l2_stub_##N(void) { l2_hit(N); }
L2_STUB_FN(0) L2_STUB_FN(1) L2_STUB_FN(2) L2_STUB_FN(3) L2_STUB_FN(4) L2_STUB_FN(5)
L2_STUB_FN(6) L2_STUB_FN(7) L2_STUB_FN(8) L2_STUB_FN(9) L2_STUB_FN(10) L2_STUB_FN(11)
L2_STUB_FN(12) L2_STUB_FN(13) L2_STUB_FN(14) L2_STUB_FN(15) L2_STUB_FN(16) L2_STUB_FN(17)
static game_guest_function const l2_stub_fns[L2_STUBS] = {
    l2_stub_0, l2_stub_1, l2_stub_2, l2_stub_3, l2_stub_4, l2_stub_5, l2_stub_6, l2_stub_7,
    l2_stub_8, l2_stub_9, l2_stub_10, l2_stub_11, l2_stub_12, l2_stub_13, l2_stub_14,
    l2_stub_15, l2_stub_16, l2_stub_17};

static game_guest_function l2_lookup(uint32_t xbox_va)
{
    for (unsigned i = 0; i < L2_STUBS; i++)
        if (l2_stubs[i].va == xbox_va) return l2_stub_fns[i];
    return NULL;
}

static l2_stub_def *l2_stub(uint32_t va)
{
    for (unsigned i = 0; i < L2_STUBS; i++)
        if (l2_stubs[i].va == va) return &l2_stubs[i];
    CHECK(0);
    return &l2_stubs[0];
}

static void l2_set(uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx)
{
    l2_stub_def *stub = l2_stub(va);
    stub->eax = eax;
    stub->ecx = ecx;
    stub->edx = edx;
    stub->split8 = 0u;
}

static void l2_write16(uint32_t address, uint16_t value)
{
    guest_write8(address, (uint8_t)(value & 0xFFu));
    guest_write8(address + 1u, (uint8_t)(value >> 8));
}

static void l2_begin(void)
{
    prepare();
    l2_active = 1;
    l2_log_count = 0u;
    memset(l2_log, 0, sizeof l2_log);
    for (unsigned i = 0; i < L2_STUBS; i++) {
        l2_stubs[i].eax = 0xE0000000u + i;
        l2_stubs[i].ecx = 0xE1000000u + i;
        l2_stubs[i].edx = 0xE2000000u + i;
        l2_stubs[i].split8 = 0u;
    }
    g_ebx = 0xB0B0B0B0u;
    g_ebp = 0xBEBEBE01u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
}

static void l2_check_saved(void)
{
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
}

#define L2_REGS(A, C, D) CHECK(g_eax == (A) && g_ecx == (C) && g_edx == (D))
#define L2_ENTRY_ESP 0x100u

static void test_l2_object_constructors(void)
{
    /* 38864E: fixed field set zeroed, 0xFFFF at +0x30, 1 at +0x98, rest untouched. */
    l2_begin();
    memset(&memory[0x2000], 0xFF, 0xB0);
    g_ecx = 0x2000u;
    invoke(0x0038864Eu, 4u);
    static const uint32_t cleared[] = {0u, 4u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x28u, 0x2Cu, 0x54u,
                                       0x58u, 0x5Cu, 0x90u, 0x94u, 0x9Cu, 0xA0u, 0xA4u, 0xA8u};
    for (uint32_t offset = 0u; offset < 0xB0u; offset += 4u) {
        uint32_t expect = 0xFFFFFFFFu;
        for (unsigned i = 0; i < sizeof cleared / sizeof cleared[0]; i++)
            if (cleared[i] == offset) expect = 0u;
        if (offset == 0x30u) expect = 0xFFFFu;
        if (offset == 0x98u) expect = 1u;
        CHECK(guest_read32(0x2000u + offset) == expect);
    }
    L2_REGS(0x2000u, 0u, 0x209Cu);
    l2_check_saved();
}

static void test_l2_table_lookups(void)
{
    /* Every one of the fifteen entries is found when it alone holds the key. */
    for (uint32_t entry = 0u; entry < 15u; entry++) {
        l2_begin();
        for (uint32_t k = 0u; k < 16u; k++) guest_write32(0x004DAA50u + 0x2Cu * k, 0u);
        guest_write32(0x004DAA50u + 0x2Cu * entry, 7u);
        g_esi = 7u;
        invoke(0x000B3C10u, 4u);
        CHECK(g_eax == 0x004DAA50u + 0x2Cu * entry);
    }
    /* 0x4DAA50 table, stride 0x2C, ESI key. */
    for (unsigned scenario = 0u; scenario < 4u; scenario++) {
        l2_begin();
        for (uint32_t k = 0u; k < 16u; k++) guest_write32(0x004DAA50u + 0x2Cu * k, 0u);
        if (scenario == 1u) {
            guest_write32(0x004DAA50u + 0x2Cu * 3u, 7u);
            guest_write32(0x004DAA50u + 0x2Cu * 9u, 7u);
        }
        if (scenario == 2u) guest_write32(0x004DAA50u + 0x2Cu * 14u, 7u);
        if (scenario == 3u) guest_write32(0x004DAA50u, 7u);
        g_esi = 7u;
        invoke(0x000B3C10u, 4u);
        const uint32_t expect = scenario == 1u ? 0x004DAA50u + 0x2Cu * 9u
                                : scenario == 2u ? 0x004DAA50u + 0x2Cu * 14u
                                : scenario == 3u ? 0x004DAA50u : 0u;
        CHECK(g_eax == expect && g_ecx == 0x004DAD10u && g_esi == 7u && g_edi == 0xD1D1D1D1u);
        CHECK(g_edx == 0x004DAD10u - 0xDCu + 0x84u);
    }
}

static void test_l2_call_wrappers(void)
{
    /* 25240 */
    static const uint32_t flags[] = {8u, 0x40000008u, 4u, 0u, 0x40000004u, 12u};
    static const uint32_t selector[] = {1u, 5u, 8u, 0u, 4u, 9u};
    for (unsigned i = 0u; i < 6u; i++) {
        l2_begin();
        l2_set(0x0037E89Eu, 0x77u, 0x11u, 0x22u);
        guest_write32(g_esp + 4u, flags[i]);
        invoke(0x00025240u, 4u);
        L2_REGS(0x77u, 0x11u, 0x22u);
        CHECK(l2_log_count == 1u && l2_log[0].args[0] == selector[i] &&
              l2_log[0].ret_va == ((flags[i] & 0x40000000u) ? 0x00025261u : 0x0002526Bu) &&
              l2_log[0].esp_seen == L2_ENTRY_ESP - 8u);
    }

    /* 1B5260: five calls, arguments stay live on the original stack. */
    l2_begin();
    invoke(0x001B5260u, 4u);
    static const uint32_t order[] = {0x00302A20u, 0x0009E2C0u, 0x0009E2C0u, 0x0009E2C0u, 0x0009E2D0u, 0x0009E380u};
    static const uint32_t returns[] = {0x1B5265u, 0x1B5271u, 0x1B527Du, 0x1B5289u, 0x1B529Au, 0x1B52A4u};
    static const uint32_t depth[] = {4u, 12u, 20u, 28u, 40u, 44u};
    CHECK(l2_log_count == 6u);
    for (unsigned i = 0u; i < 6u; i++)
        CHECK(l2_log[i].va == order[i] && l2_log[i].ret_va == returns[i] &&
              l2_log[i].esp_seen == L2_ENTRY_ESP - depth[i]);
    CHECK(l2_log[1].args[0] == 0u && l2_log[1].args[1] == 0x242680u);
    CHECK(l2_log[2].args[0] == 1u && l2_log[2].args[1] == 0x242BA0u);
    CHECK(l2_log[3].args[0] == 2u && l2_log[3].args[1] == 0x2426B0u);
    CHECK(l2_log[4].args[0] == 0x2000u && l2_log[4].args[1] == 0u && l2_log[4].args[2] == 0x2426D0u);
    CHECK(l2_log[5].args[0] == 0x35DE60u);
    static const uint32_t finals[] = {0x242680u, 0u, 0x242BA0u, 1u, 0x2426B0u, 2u,
                                      0x2426D0u, 0u, 0x2000u, 0x35DE60u, 0x1B52A4u};
    for (unsigned i = 0u; i < 11u; i++) CHECK(guest_read32(L2_ENTRY_ESP - 4u - 4u * i) == finals[i]);
    L2_REGS(0xE0000000u + 9u, 0xE1000000u + 9u, 0xE2000000u + 9u);

    /* 195770 */
    l2_begin();
    guest_write32(g_esp + 4u, 0x3000u);
    guest_write32(g_esp + 8u, 0x4444u);
    for (uint32_t i = 0u; i < 6u; i++) guest_write32(0x3000u + 4u * i, 0x100u + i);
    l2_set(0x003C8960u, 0x66u, 0x11u, 0x22u);
    invoke(0x00195770u, 4u);
    for (uint32_t i = 0u; i < 6u; i++) CHECK(guest_read32(0x0074FA78u + 4u * i) == 0x100u + i);
    L2_REGS(0x66u, 0x11u, 0x22u);
    CHECK(l2_log[0].args[0] == 0x00752344u && l2_log[0].args[1] == 0x4444u && l2_log[0].args[2] == 0x37u &&
          l2_log[0].ret_va == 0x001957BAu && l2_log[0].esp_seen == L2_ENTRY_ESP - 16u);

    /* 25BE0 */
    static const uint32_t sizes[] = {0u, 0x3F88u, 0x8000u};
    static const uint32_t counts[] = {8u, 3u, 5u};
    for (unsigned i = 0u; i < 3u; i++) {
        l2_begin();
        l2_stub(0x0037E89Eu)->split8 = 1u;
        l2_stub(0x0037E89Eu)->eax_sel8 = 0x64u;
        l2_stub(0x0037E89Eu)->eax = 0x14u;
        l2_stub(0x0037E89Eu)->ecx = 0x11u;
        l2_stub(0x0037E89Eu)->edx = 0x22u;
        guest_write32(g_esp + 4u, sizes[i]);
        invoke(0x00025BE0u, 4u);
        CHECK(g_eax == counts[i] && g_edx == sizes[i] && g_esi == 0x51515151u);
        CHECK(g_ecx == (sizes[i] == 0u ? 0xA044u : sizes[i] + 0x78u));
        CHECK(l2_log_count == 2u && l2_log[0].args[0] == 8u && l2_log[0].ret_va == 0x00025BE8u &&
              l2_log[1].args[0] == 0u && l2_log[1].ret_va == 0x00025BF1u &&
              l2_log[0].esp_seen == L2_ENTRY_ESP - 12u);
    }
}

static unsigned l2_leap_reference(int32_t year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1u : 0u;
}

static void test_l2_scalar_and_search(void)
{
    /* 3BA560 over many years against an independent calendar rule. */
    for (int32_t year = -800; year <= 4000; year++) {
        l2_begin();
        g_ecx = (uint32_t)year;
        invoke(0x003BA560u, 4u);
        CHECK((g_eax & 0xFFu) == l2_leap_reference(year));
        l2_check_saved();
    }
    l2_begin();
    g_ecx = 2004u;
    invoke(0x003BA560u, 4u);
    L2_REGS(1u, 2004u, 4u);
    l2_begin();
    g_ecx = 2000u;
    invoke(0x003BA560u, 4u);
    L2_REGS(1u, 0x190u, 0u);
    l2_begin();
    g_ecx = 1900u;
    invoke(0x003BA560u, 4u);
    L2_REGS(0u, 0x190u, 300u);
    l2_begin();
    g_ecx = 0x01000000u;
    invoke(0x003BA560u, 4u);
    L2_REGS(0x00028F01u, 0x01000000u, 16u);
}

static void test_t1541_list002(void)
{
    test_l2_object_constructors();
    test_l2_table_lookups();
    test_l2_call_wrappers();
    test_l2_scalar_and_search();
    l2_active = 0;
}
