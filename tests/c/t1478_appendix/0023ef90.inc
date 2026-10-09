/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x0023EF90, derived from the original instructions only:
 * ecx = arg; eax walks 0x513EB8 in steps of 0x5C while eax < 0x514758 (24 records),
 * comparing the first dword of each record; hit -> eax = 1, miss -> eax = 0; ecx = arg
 * on both paths, edx and every other register untouched, no memory writes. */

#define C23EF90_TABLE 0x513eb8u
#define C23EF90_END 0x514758u
#define C23EF90_STRIDE 0x5cu

static uint8_t c23ef90_before[sizeof memory];

static void c23ef90_call(uint32_t value, uint32_t expected_eax)
{
    prepare();
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBBBBBBu;
    g_df = 0;
    guest_write32(g_esp + 4u, value);
    memcpy(c23ef90_before, memory, sizeof memory);
    invoke(0x0023EF90u, 4u);
    CHECK(g_eax == expected_eax);
    CHECK(g_ecx == value);
    CHECK(g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBBBBBBu);
    CHECK(memcmp(c23ef90_before, memory, sizeof memory) == 0);
}

static void test_t1478_appendix_0023ef90(void)
{
    memset(&memory[C23EF90_TABLE - 0x200u], 0, 0x1000u);
    /* all-zero table: zero is found in record 0, anything else misses (the dword just
     * past the 24th record is zero too, so a nonzero query cannot hit it) */
    c23ef90_call(0u, 1u);
    c23ef90_call(1u, 0u);
    c23ef90_call(0xFFFFFFFFu, 0u);

    for (uint32_t i = 0; i < 24u; i++) {
        uint32_t record = C23EF90_TABLE + i * C23EF90_STRIDE;
        guest_write32(record, 0x1000u + 3u * i);
        guest_write32(record + 4u, 0x9000u + i);
        for (uint32_t offset = 8u; offset < C23EF90_STRIDE; offset += 4u) {
            guest_write32(record + offset, 0xAAAA0000u + offset);
        }
    }
    guest_write32(C23EF90_END, 0x7777u);                          /* record 24: out of range */
    guest_write32(C23EF90_TABLE - C23EF90_STRIDE, 0x6666u);       /* record -1: out of range */
    for (uint32_t i = 0; i < 24u; i++) {
        c23ef90_call(0x1000u + 3u * i, 1u);          /* first dword hits */
        c23ef90_call(0x9000u + i, 0u);               /* only +4 holds it: not compared */
        c23ef90_call(0x1000u + 3u * i + 1u, 0u);
    }
    c23ef90_call(0xAAAA0000u + 8u, 0u);              /* later fields are not compared */
    c23ef90_call(0xAAAA0000u + 0x58u, 0u);
    c23ef90_call(0x7777u, 0u);                       /* one past the last record */
    c23ef90_call(0x6666u, 0u);                       /* one before the first record */
    c23ef90_call(0u, 0u);                            /* zero no longer present */

    /* high-bit values compare as plain equality; first match wins; only the last
     * record holds the value */
    guest_write32(C23EF90_TABLE + 23u * C23EF90_STRIDE, 0x80000000u);
    c23ef90_call(0x80000000u, 1u);
    c23ef90_call(0x7FFFFFFFu, 0u);
    guest_write32(C23EF90_TABLE, 0x80000000u);
    c23ef90_call(0x80000000u, 1u);
    /* a hit only in the first record of an otherwise empty-valued table */
    memset(&memory[C23EF90_TABLE], 0xFF, C23EF90_END - C23EF90_TABLE);
    c23ef90_call(0xFFFFFFFFu, 1u);
    c23ef90_call(0xFFFFFFFEu, 0u);
}
