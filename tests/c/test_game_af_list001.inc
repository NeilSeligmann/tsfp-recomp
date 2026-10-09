/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1588 native tests for the 7 admitted roots of src/game/game_af_list001.c (included by
 * test_game_replacements.c). Expectations are hand-derived from the retail disassembly
 * (register outputs, memory, call return VA and modelled stack depth), not by running the
 * replacement against itself. The nested callee 0x0035E960 is a recorded stub reached through
 * recomp_lookup while af_active (entry ESP 0x2000). */

#define AF_ENTRY 0x2000u
#define AF_BASE 0x10000u

static uint32_t af_stub_eax;
static unsigned af_stub_calls;
static uint32_t af_stub_return_va, af_stub_esp;

static void af_stub_35e960(void)
{
    af_stub_calls++;
    af_stub_return_va = guest_read32(g_esp);
    af_stub_esp = g_esp;
    g_eax = af_stub_eax;
    g_ecx = 0xE1000001u;
    g_edx = 0xE2000001u;
    g_esp += 4u;
}

static game_guest_function af_lookup(uint32_t xbox_va)
{
    return xbox_va == 0x0035E960u ? af_stub_35e960 : NULL;
}

static void af_begin(void)
{
    prepare();
    memset(&memory[AF_BASE], 0xCC, 0x400);
    g_ebx = 0xB0B0B0B0u;
    g_ebp = 0xBEBEBE01u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
}

static void af_check_untouched_registers(void)
{
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
}

static void test_af_store_pair(void)
{
    /* 3ACB50: stdcall(a, b), ECX = object: [obj+0x28] = a, [obj+0x2C] = b, EAX = a, EDX = b. */
    af_begin();
    g_ecx = AF_BASE;
    guest_write32(g_esp + 4u, 0xAAAA1111u);
    guest_write32(g_esp + 8u, 0xBBBB2222u);
    invoke(0x003ACB50u, 12u);
    CHECK(g_eax == 0xAAAA1111u && g_edx == 0xBBBB2222u && g_ecx == AF_BASE);
    CHECK(guest_read32(AF_BASE + 0x28u) == 0xAAAA1111u && guest_read32(AF_BASE + 0x2Cu) == 0xBBBB2222u);
    CHECK(guest_read32(AF_BASE + 0x24u) == 0xCCCCCCCCu && guest_read32(AF_BASE + 0x30u) == 0xCCCCCCCCu);
    af_check_untouched_registers();
}

static void test_af_zero_three(void)
{
    /* 3BEAC0: EAX = 0, [obj+4], [obj+8], [obj+0xC] = 0, [obj] and [obj+0x10] untouched. */
    af_begin();
    g_ecx = AF_BASE;
    invoke(0x003BEAC0u, 4u);
    CHECK(g_eax == 0u && g_ecx == AF_BASE && g_edx == 0x33333333u);
    CHECK(guest_read32(AF_BASE + 4u) == 0u && guest_read32(AF_BASE + 8u) == 0u &&
          guest_read32(AF_BASE + 0xCu) == 0u);
    CHECK(guest_read32(AF_BASE) == 0xCCCCCCCCu && guest_read32(AF_BASE + 0x10u) == 0xCCCCCCCCu);
    af_check_untouched_registers();
}

static void test_af_pointer_field(void)
{
    /* 386FCF: [[obj+0x20]+0xC], or 0 for a null pointer. */
    af_begin();
    g_ecx = AF_BASE;
    guest_write32(AF_BASE + 0x20u, 0u);
    invoke(0x00386FCFu, 4u);
    CHECK(g_eax == 0u && g_ecx == AF_BASE && g_edx == 0x33333333u);

    af_begin();
    g_ecx = AF_BASE;
    guest_write32(AF_BASE + 0x20u, AF_BASE + 0x100u);
    guest_write32(AF_BASE + 0x100u + 0xCu, 0x77u);
    invoke(0x00386FCFu, 4u);
    CHECK(g_eax == 0x77u && g_ecx == AF_BASE && g_edx == 0x33333333u);
    af_check_untouched_registers();
}

static void test_af_live_predicate(void)
{
    /* 35E670: 0x35E960() zero gives 0x10, otherwise [0x76BBBC]. ECX/EDX are the callee's. */
    for (unsigned scenario = 0u; scenario < 2u; scenario++) {
        af_begin();
        g_esp = AF_ENTRY;
        guest_write32(g_esp, 0x12345678u);
        guest_write32(0x0076BBBCu, 0x1234u);
        af_stub_eax = scenario == 0u ? 0u : 5u;
        af_stub_calls = 0u;
        af_active = 1;
        invoke(0x0035E670u, 4u);
        af_active = 0;
        CHECK(g_eax == (scenario == 0u ? 0x10u : 0x1234u));
        CHECK(g_ecx == 0xE1000001u && g_edx == 0xE2000001u);
        CHECK(af_stub_calls == 1u && af_stub_return_va == 0x0035E675u && af_stub_esp == AF_ENTRY - 4u);
        af_check_untouched_registers();
    }
}

static void test_af_construct(void)
{
    /* 3BFBE0: stdcall(value), ECX = object. EAX = object, ECX = 0. */
    af_begin();
    g_ecx = AF_BASE;
    guest_write32(g_esp + 4u, 0x55u);
    invoke(0x003BFBE0u, 8u);
    CHECK(g_eax == AF_BASE && g_ecx == 0u && g_edx == 0x33333333u);
    CHECK(guest_read32(AF_BASE) == 0x004B6374u && guest_read32(AF_BASE + 4u) == 0x55u);
    CHECK(guest_read32(AF_BASE + 8u) == 0u && guest_read32(AF_BASE + 0xCu) == 0u &&
          guest_read32(AF_BASE + 0x10u) == 0u && guest_read32(AF_BASE + 0x14u) == 0xCCCCCCCCu);
    af_check_untouched_registers();
}

static void test_af_find_record(void)
{
    /* 3B3120: 4 records of 0xAC bytes from obj+0x60 to obj+0x310. Key (a, b) is [rec+0x24] and
     * [rec+0x20], and [rec+8] must be non-zero. */
    for (unsigned scenario = 0u; scenario < 3u; scenario++) {
        af_begin();
        memset(&memory[AF_BASE + 0x60u], 0, 0x2B0);
        for (uint32_t k = 0u; k < 4u; k++) {
            const uint32_t record = AF_BASE + 0x60u + 0xACu * k;
            guest_write32(record + 0x24u, 0xA1u);
            guest_write32(record + 0x20u, 0xB2u);
        }
        guest_write32(AF_BASE + 0x60u + 0xACu * 1u + 8u, 1u);
        guest_write32(AF_BASE + 0x60u + 0xACu * 1u + 0x20u, 0xB3u);
        if (scenario == 0u) guest_write32(AF_BASE + 0x60u + 0xACu * 2u + 8u, 1u);
        if (scenario == 1u) guest_write32(AF_BASE + 0x60u + 0xACu * 3u + 8u, 1u);
        g_ecx = AF_BASE;
        guest_write32(g_esp + 4u, 0xA1u);
        guest_write32(g_esp + 8u, 0xB2u);
        invoke(0x003B3120u, 12u);
        const uint32_t expected[] = {AF_BASE + 0x60u + 0xACu * 2u, AF_BASE + 0x60u + 0xACu * 3u, 0u};
        CHECK(g_eax == expected[scenario] && g_ecx == AF_BASE + 0x310u && g_edx == 0xB2u);
        af_check_untouched_registers();
        CHECK(guest_read32(0x100u) == 0x12345678u);
    }
}

static void test_af_clear_record(void)
{
    /* 3C44C0: the listed dwords and bytes are zeroed, everything else keeps its fill. */
    af_begin();
    g_ecx = AF_BASE;
    invoke(0x003C44C0u, 4u);
    CHECK(g_eax == 0u && g_edx == 0u && g_ecx == AF_BASE);
    for (uint32_t offset = 0u; offset < 0x180u; offset++) {
        int cleared = 0;
        if (offset >= 4u && offset < 0x60u) cleared = 1;
        if (offset >= 0x64u && offset < 0x68u) cleared = 1;
        if (offset == 0x168u || offset == 0x174u) cleared = 1;
        if (offset >= 0x16Cu && offset < 0x174u) cleared = 1;
        CHECK(guest_read8(AF_BASE + offset) == (cleared ? 0u : 0xCCu));
    }
    af_check_untouched_registers();
}

static void test_t1588_af_list001(void)
{
    test_af_store_pair();
    test_af_zero_three();
    test_af_pointer_field();
    test_af_live_predicate();
    test_af_construct();
    test_af_find_record();
    test_af_clear_record();
    af_active = 0;
}
