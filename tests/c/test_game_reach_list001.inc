/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1583 native tests for the 3 admitted roots of src/game/game_reach_list001.c (included by test_game_replacements.c).
 * Expectations are hand-derived from the retail disassembly (register outputs, memory, call
 * arguments, return VAs and the modelled stack depth), not by running the replacement against
 * itself. Nested guest callees are recorded stubs reached through recomp_lookup while rl_active
 * (entry ESP 0x2000). */

typedef struct {
    uint32_t va, pops, eax, ecx, edx;
    void (*hook)(void);
} rl_stub_def;

typedef struct {
    uint32_t va, ret_va, esp_seen, ecx_in, edx_in, args[3];
} rl_log_entry;

#define RL_STUBS 9u
#define RL_ENTRY 0x2000u
static rl_stub_def rl_stubs[RL_STUBS] = {
    {0x00312A60u, 0u}, {0x00094310u, 0u}, {0x00069620u, 0u}, {0x0003B310u, 0u}, {0x003530F0u, 0u},
    {0x0038608Au, 1u}, {0x0032A5B0u, 0u}, {0x00045590u, 0u}, {0x0003E970u, 0u}};
static rl_log_entry rl_log[32];
static unsigned rl_log_count;
static uint32_t rl_3b310_count;

static void rl_hook_3b310(void)
{
    guest_write32(game_stack_arg(0u), rl_3b310_count);
    guest_write32(g_edx, 0xAAAAu);
    guest_write32(g_edx + 4u, 0xBBBBu);
}

static void rl_hit(unsigned index)
{
    rl_stub_def *stub = &rl_stubs[index];
    rl_log_entry *entry = &rl_log[rl_log_count < 32u ? rl_log_count : 31u];
    rl_log_count++;
    entry->va = stub->va;
    entry->ret_va = guest_read32(g_esp);
    entry->esp_seen = g_esp;
    entry->ecx_in = g_ecx;
    entry->edx_in = g_edx;
    for (unsigned i = 0; i < 3u; i++) entry->args[i] = game_stack_arg(i);
    if (stub->hook != NULL) stub->hook();
    g_eax = stub->eax;
    g_ecx = stub->ecx;
    g_edx = stub->edx;
    g_esp += 4u + 4u * stub->pops;
}

#define RL_STUB_FN(N) static void rl_stub_##N(void) { rl_hit(N); }
RL_STUB_FN(0) RL_STUB_FN(1) RL_STUB_FN(2) RL_STUB_FN(3) RL_STUB_FN(4) RL_STUB_FN(5)
RL_STUB_FN(6) RL_STUB_FN(7) RL_STUB_FN(8)
static game_guest_function const rl_stub_fns[RL_STUBS] = {
    rl_stub_0, rl_stub_1, rl_stub_2, rl_stub_3, rl_stub_4, rl_stub_5, rl_stub_6, rl_stub_7,
    rl_stub_8};


static game_guest_function rl_lookup(uint32_t xbox_va)
{
    for (unsigned i = 0; i < RL_STUBS; i++)
        if (rl_stubs[i].va == xbox_va) return rl_stub_fns[i];
    return NULL;
}

static rl_stub_def *rl_stub(uint32_t va)
{
    for (unsigned i = 0; i < RL_STUBS; i++)
        if (rl_stubs[i].va == va) return &rl_stubs[i];
    CHECK(0);
    return &rl_stubs[0];
}

static void rl_set(uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx)
{
    rl_stub_def *stub = rl_stub(va);
    stub->eax = eax;
    stub->ecx = ecx;
    stub->edx = edx;
}

static void rl_begin(void)
{
    prepare();
    g_esp = RL_ENTRY;
    guest_write32(g_esp, 0x12345678u);
    rl_active = 1;
    rl_log_count = 0u;
    memset(rl_log, 0, sizeof rl_log);
    for (unsigned i = 0; i < RL_STUBS; i++) {
        rl_stubs[i].eax = 0xE0000000u + i;
        rl_stubs[i].ecx = 0xE1000000u + i;
        rl_stubs[i].edx = 0xE2000000u + i;
        rl_stubs[i].hook = NULL;
    }
    rl_stubs[3].hook = rl_hook_3b310;
    g_ebx = 0xB0B0B0B0u;
    g_ebp = 0xBEBEBE01u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
}

static void rl_check_saved(void)
{
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
    CHECK(guest_read32(RL_ENTRY) == 0x12345678u);
}

static int rl_bytes_equal(uint32_t address, uint32_t length, uint8_t value)
{
    for (uint32_t i = 0u; i < length; i++)
        if (guest_read8(address + i) != value) return 0;
    return 1;
}

#define RL_REGS(A, C, D) CHECK(g_eax == (A) && g_ecx == (C) && g_edx == (D))

static void test_rl_compare(void)
{
    /* 2CBCE0: 31 dwords of 69620(7) against 0x7BA364 + 2 * 0x1E0. */
    static const uint32_t differ_at[] = {99u, 30u, 5u, 0u};
    static const uint32_t ecx_after[] = {0u, 0u, 25u, 30u};
    for (unsigned i = 0u; i < 4u; i++) {
        rl_begin();
        for (uint32_t k = 0u; k < 0x1Fu; k++) {
            guest_write32(0x10000u + 4u * k, 0x1000u + k);
            guest_write32(0x007BA724u + 4u * k, 0x1000u + k);
        }
        guest_write32(0x10000u + 4u * 0x1Fu, 1u);
        guest_write32(0x007BA724u + 4u * 0x1Fu, 2u);
        if (differ_at[i] < 0x1Fu) guest_write32(0x007BA724u + 4u * differ_at[i], 0xFFFFFFFFu);
        guest_write32(0x0078AD34u, 7u);
        guest_write32(0x007BA9E0u, 2u);
        rl_set(0x00069620u, 0x10000u, 0x11u, 0x22u);
        invoke(0x002CBCE0u, 4u);
        RL_REGS(differ_at[i] < 0x1Fu ? 1u : 0u, ecx_after[i], 0u);
        CHECK(rl_log_count == 1u && rl_log[0].args[0] == 7u && rl_log[0].ret_va == 0x002CBCEDu &&
              rl_log[0].esp_seen == RL_ENTRY - 16u);
        rl_check_saved();
    }
}

static void test_rl_stream_release(void)
{
    /* 386155: release active entries while the budget lasts, zero the others. */
    for (unsigned scenario = 0u; scenario < 5u; scenario++) {
        static const uint32_t budgets[] = {2u, 0u, 10u, 10u, 3u};
        static const uint32_t counts[] = {4u, 4u, 4u, 4u, 0u};
        static const uint32_t flags[5][4] = {
            {1u, 0u, 1u, 1u}, {1u, 1u, 1u, 1u}, {0u, 0u, 0u, 0u}, {1u, 1u, 1u, 1u}, {1u, 1u, 1u, 1u}};
        rl_begin();
        memset(&memory[0x10000], 0, 0x1000);
        memset(&memory[0x20000], 0xFF, 0x2000);
        guest_write32(0x10000u + 0x14u, counts[scenario]);
        guest_write32(0x10000u + 0x1Cu, budgets[scenario]);
        guest_write32(0x10000u + 0x20u, 0x10100u);
        for (uint32_t k = 0u; k < 4u; k++) {
            guest_write32(0x10100u + 4u * k, 0x10200u + 0x100u * k);
            guest_write32(0x10200u + 0x100u * k + 0x14u, flags[scenario][k]);
            guest_write32(0x10200u + 0x100u * k + 0x24u, 0x20000u + 0x400u * k);
            guest_write32(0x10200u + 0x100u * k + 0x4Cu, 0xCCCCCCCCu);
        }
        g_ecx = 0x10000u;
        invoke(0x00386155u, 4u);
        /* Entries released through the call: scenario 0 -> 0 and 2, 1 -> none, 2 -> none,
         * 3 -> 0..3, 4 -> none (no entries). */
        static const unsigned expected_calls[] = {2u, 0u, 0u, 4u, 0u};
        CHECK(rl_log_count == expected_calls[scenario]);
        for (unsigned c = 0u; c < rl_log_count; c++)
            CHECK(rl_log[c].va == 0x0038608Au && rl_log[c].ecx_in == 0x10000u &&
                  rl_log[c].ret_va == 0x0038617Du && rl_log[c].esp_seen == RL_ENTRY - 24u);
        if (scenario == 0u) {
            CHECK(rl_log[0].args[0] == 0x10200u && rl_log[1].args[0] == 0x10400u);
            CHECK(rl_bytes_equal(0x20000u, 0x140u, 0xFFu) && rl_bytes_equal(0x20800u, 0x140u, 0xFFu));
            CHECK(rl_bytes_equal(0x20400u, 0x140u, 0u) && rl_bytes_equal(0x20C00u, 0x140u, 0u));
            CHECK(rl_bytes_equal(0x20400u + 0x140u, 0x10u, 0xFFu));
            CHECK(guest_read32(0x10300u + 0x4Cu) == 0u && guest_read32(0x10500u + 0x4Cu) == 0u &&
                  guest_read32(0x10200u + 0x4Cu) == 0xCCCCCCCCu);
            RL_REGS(0u, 0u, 0x10500u);
        } else if (scenario == 1u || scenario == 2u) {
            for (uint32_t k = 0u; k < 4u; k++) {
                CHECK(rl_bytes_equal(0x20000u + 0x400u * k, 0x140u, 0u));
                CHECK(guest_read32(0x10200u + 0x100u * k + 0x4Cu) == 0u);
            }
            RL_REGS(0u, 0u, 0x10500u);
        } else if (scenario == 3u) {
            CHECK(rl_bytes_equal(0x20000u, 0x2000u, 0xFFu));
            RL_REGS(0xE0000000u + 5u, 0xE1000000u + 5u, 0xE2000000u + 5u);
        } else {
            RL_REGS(0x11111111u, 0x10000u, 0x33333333u);
        }
        rl_check_saved();
    }
}

static void rl_ranges_case(uint32_t length_a, uint32_t length_b, const uint32_t *diffs, unsigned diff_count,
                           const uint32_t *expect, unsigned expect_pairs, uint32_t edx_expect)
{
    rl_begin();
    memset(&memory[0x11000], 0x31, 0x400);
    memset(&memory[0x12000], 0x31, 0x400);
    for (uint32_t k = 0u; k < 16u; k++) {
        guest_write32(0x10000u + 8u * k, 0xFFFFFFFFu);
        guest_write32(0x10000u + 8u * k + 4u, 0u);
    }
    for (unsigned d = 0u; d < diff_count; d++) guest_write8(0x11000u + diffs[d], 0x77u);
    g_esi = 0x10000u;
    g_ecx = 0x4321u;
    guest_write32(g_esp + 4u, 0x11000u);
    guest_write32(g_esp + 8u, 0x12000u);
    guest_write32(g_esp + 12u, length_a);
    guest_write32(g_esp + 16u, length_b);
    invoke(0x003367A0u, 4u);
    CHECK(g_eax == expect_pairs);
    for (unsigned k = 0u; k < 16u; k++) {
        const uint32_t start = k < expect_pairs ? expect[2u * k] : 0xFFFFFFFFu;
        const uint32_t size = k < expect_pairs ? expect[2u * k + 1u] : 0u;
        CHECK(guest_read32(0x10000u + 8u * k) == start && guest_read32(0x10000u + 8u * k + 4u) == size);
    }
    CHECK(g_edx == edx_expect && g_esi == 0x10000u);
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_edi == 0xD1D1D1D1u);
    CHECK(guest_read32(RL_ENTRY) == 0x12345678u);
}

static void test_rl_ranges(void)
{
    static const uint32_t three[] = {0u, 3u};
    rl_ranges_case(3u, 3u, NULL, 0u, three, 1u, 3u);
    CHECK(g_ecx == 0xFFFFF000u);

    static const uint32_t forty[] = {0u, 32u, 33u, 7u};
    rl_ranges_case(40u, 40u, NULL, 0u, forty, 2u, 40u);

    static const uint32_t tail_b[] = {0u, 5u, 5u, 3u};
    rl_ranges_case(5u, 8u, NULL, 0u, tail_b, 2u, 5u);

    static const uint32_t tail_a[] = {0u, 5u, 5u, 3u};
    rl_ranges_case(8u, 5u, NULL, 0u, tail_a, 2u, 5u);

    static const uint32_t only_tail[] = {0u, 4u};
    rl_ranges_case(0u, 4u, NULL, 0u, only_tail, 1u, 0u);
    CHECK(g_ecx == 0x4321u);

    rl_ranges_case(0u, 0u, NULL, 0u, NULL, 0u, 0u);

    static const uint32_t hundred[] = {0u, 32u, 33u, 32u, 66u, 32u, 99u, 1u};
    rl_ranges_case(100u, 100u, NULL, 0u, hundred, 4u, 100u);

    static const uint32_t diff_pair[] = {32u, 33u};
    static const uint32_t diff_expect[] = {0u, 34u, 35u, 5u};
    rl_ranges_case(40u, 40u, diff_pair, 2u, diff_expect, 2u, 40u);
}

static void test_t1583_reach_list001(void)
{
    test_rl_compare();
    test_rl_stream_release();
    test_rl_ranges();
    rl_active = 0;
}
