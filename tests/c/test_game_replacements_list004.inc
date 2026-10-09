/* T1543: native tests of src/game/game_calls_list004.c. Included by test_game_replacements.c just
 * before main so it can use prepare, invoke and the t1480e call log. The callees 00059D70 and
 * 00059D20 are stubs recording the live frame; the bits of list004_hit_mask pick which numbered
 * call returns nonzero. */
static uint32_t list004_hit_mask;

static void list004_stub(uint32_t va)
{
    const unsigned index = t1480e_calls;
    t1480e_record(va);
    g_eax = ((list004_hit_mask >> index) & 1u) != 0u ? 0x77u : 0u;
    g_ecx = 0x6002u;
    g_edx = 0x6003u;
    g_esp += 4u;
}

static void guest_list004_59d70(void) { list004_stub(0x00059D70u); }
static void guest_list004_59d20(void) { list004_stub(0x00059D20u); }

static game_guest_function list004_lookup(uint32_t xbox_va)
{
    switch (xbox_va) {
    case 0x00059D70u: return guest_list004_59d70;
    case 0x00059D20u: return guest_list004_59d20;
    default: return NULL;
    }
}

/* One probe-chain root: three blocks over `tables`, each reading the record selector from the
 * owner at object+0x7C. `record` is the owner's +0x204 id, `flags` the byte at row+0xC. */
static void list004_probe_case(uint32_t va, uint32_t callee, const uint32_t tables[3],
                               const uint32_t pcs[3], uint32_t record, uint32_t flags,
                               uint32_t hit_mask, unsigned expected_calls)
{
    const uint32_t entry = 0x1100u;
    const uint32_t object = 0x2000u;
    const uint32_t owner = 0x2200u;
    const uint32_t base = 0x3000u;
    const uint32_t selected = (record == 0xFFFFFFFFu || (flags & 1u) != 0u) ? 1u : 0u;
    t1480e_reset();
    prepare();
    g_esp = entry;
    g_esi = 0x51u;
    g_edi = 0x52u;
    g_ebx = 0x53u;
    guest_write32(entry + 4u, object);
    guest_write32(object + 0x7Cu, owner);
    guest_write32(owner + 0x204u, record);
    guest_write32(0x004F9BACu, base);
    guest_write8(base + record * 0x29Cu + 0xCu, (uint8_t)flags);
    for (unsigned block = 0u; block < 3u; block++) {
        guest_write32(tables[block], 0xA000u + block * 8u);
        guest_write32(tables[block] + 4u, 0xB000u + block * 8u);
    }
    list004_hit_mask = hit_mask;
    invoke(va, 4u);
    CHECK(t1480e_calls == expected_calls);
    for (unsigned block = 0u; block < expected_calls; block++) {
        t1480e_expect(block, callee, entry - 20u, pcs[block]);
        CHECK(t1480e_log[block].arg[0] == object);
        CHECK(t1480e_log[block].arg[1] == (selected != 0u ? 0xB000u : 0xA000u) + block * 8u);
    }
    const int hit = expected_calls != 0u && ((hit_mask >> (expected_calls - 1u)) & 1u) != 0u;
    CHECK(g_eax == (hit ? 1u : 0u));
    CHECK(g_esi == 0x51u && g_edi == 0x52u && g_ebx == 0x53u);
    CHECK(guest_read32(entry - 4u) == 0x51u && guest_read32(entry - 8u) == 0x52u);
    CHECK(g_ecx == 0x6002u && g_edx == 0x6003u);
}

static void test_t1543_list004(void)
{
    list004_active = 1;
    static const uint32_t tables_a[3] = {0x005202F8u, 0x00520308u, 0x005202E8u};
    static const uint32_t pcs_a[3] = {0x0026CCBEu, 0x0026CCFAu, 0x0026CD36u};
    static const uint32_t tables_b[3] = {0x00520338u, 0x00520328u, 0x00520318u};
    static const uint32_t pcs_b[3] = {0x0026CD8Eu, 0x0026CDCAu, 0x0026CE06u};
    for (unsigned variant = 0u; variant < 2u; variant++) {
        const uint32_t va = variant == 0u ? 0x0026CC80u : 0x0026CD50u;
        const uint32_t callee = variant == 0u ? 0x00059D70u : 0x00059D20u;
        const uint32_t *tables = variant == 0u ? tables_a : tables_b;
        const uint32_t *pcs = variant == 0u ? pcs_a : pcs_b;
        /* No record id: selector 1 for every block, all three miss. */
        list004_probe_case(va, callee, tables, pcs, 0xFFFFFFFFu, 0u, 0u, 3u);
        /* Record row without flag 1: selector 0, the second block hits. */
        list004_probe_case(va, callee, tables, pcs, 2u, 0x10u, 2u, 2u);
        /* Record row with flag 1: selector 1, the first block hits. */
        list004_probe_case(va, callee, tables, pcs, 3u, 0x11u, 1u, 1u);
        /* Third block hit. */
        list004_probe_case(va, callee, tables, pcs, 1u, 0u, 4u, 3u);
    }
    list004_active = 0;
}
