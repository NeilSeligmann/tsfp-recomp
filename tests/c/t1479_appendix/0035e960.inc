/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x0035E960 (cdecl, 0 stack args). Derived from the original
 * instructions only:
 *   ecx = dword [0x774024] ; eax = 0 ; cmp ecx,1 ; sete al ; ret.
 *   EAX = 1 iff the whole dword equals 1 (upper bits of EAX clear), ECX = the dword,
 *   EDX unchanged. No stack argument, no guest write, NO push and NO pop (so the POP-slot
 *   aliases do not exist). Alias (a) is covered by putting the stack on the flag dword: the
 *   return-address slot, and the word just above it, ARE [0x774024].
 * MUTANT: g_eax = (g_ecx == 1u) ? 1u : 0u; -> g_eax = (g_ecx <= 1u) ? 1u : 0u;
 * MUTANT: g_eax = (g_ecx == 1u) ? 1u : 0u; -> g_eax = ((g_ecx & 0xffu) == 1u) ? 1u : 0u;
 * MUTANT: g_ecx = guest_read32(0x774024u); -> g_ecx = guest_read32(0x774028u);
 */

#define T1479_35E960_ADDR 0x774024u

static uint8_t t1479_35e960_before[sizeof memory];

static void t1479_35e960_exec(uint32_t exp_eax, uint32_t exp_ecx)
{
    memcpy(t1479_35e960_before, memory, sizeof memory);
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0xB2B2B2B2u;
    g_edi = 0xB3B3B3B3u;
    g_ebp = 0xB4B4B4B4u;
    g_df = 0;
    invoke(0x0035E960u, 4u);
    CHECK(g_eax == exp_eax);
    CHECK(g_ecx == exp_ecx);
    CHECK(g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0xB2B2B2B2u);
    CHECK(g_edi == 0xB3B3B3B3u && g_ebp == 0xB4B4B4B4u);
    CHECK(memcmp(t1479_35e960_before, memory, sizeof memory) == 0);
}

static void t1479_35e960_run(uint32_t value, uint32_t exp_eax)
{
    prepare();
    g_esp = 0x100u;
    guest_write32(0x100u, 0x12345678u);
    guest_write32(0x104u, 1u); /* a caller word, not an argument: must not matter */
    guest_write32(T1479_35E960_ADDR, value);
    t1479_35e960_exec(exp_eax, value);
}

static void test_t1479_appendix_0035e960(void)
{
    static const uint32_t not_one[] = {0u, 2u, 3u, 0xFFFFFFFFu, 0x80000000u, 0x80000001u,
                                       0x101u, 0x10001u, 0x01000001u, 0x100u, 0x7FFFFFFFu,
                                       0xFFFFFFFEu};
    unsigned k;

    guest_write32(T1479_35E960_ADDR - 4u, 1u); /* neighbours equal 1: must be ignored */
    guest_write32(T1479_35E960_ADDR + 4u, 1u);
    t1479_35e960_run(1u, 1u);
    for (k = 0; k < sizeof not_one / sizeof not_one[0]; k++) {
        t1479_35e960_run(not_one[k], 0u);
        t1479_35e960_run(1u, 1u);
    }
    /* the neighbours are untouched and still 1 */
    CHECK(guest_read32(T1479_35E960_ADDR - 4u) == 1u);
    CHECK(guest_read32(T1479_35E960_ADDR + 4u) == 1u);
    guest_write32(T1479_35E960_ADDR - 4u, 0u);
    guest_write32(T1479_35E960_ADDR + 4u, 0u);
    t1479_35e960_run(1u, 1u);

    /* EAX upper bits entered nonzero (prepare() sets 0x11111111) and EAX is 0 or 1 only */
    t1479_35e960_run(0u, 0u);
    CHECK(g_eax == 0u);

    /* alias (a): the stack sits on the flag dword, so the return slot is [0x774024] */
    prepare();
    g_esp = T1479_35E960_ADDR;
    guest_write32(g_esp, 1u);
    t1479_35e960_exec(1u, 1u);
    prepare();
    g_esp = T1479_35E960_ADDR;
    guest_write32(g_esp, 2u);
    t1479_35e960_exec(0u, 2u);
    prepare();
    g_esp = T1479_35E960_ADDR;
    guest_write32(g_esp, 0x12345678u);
    t1479_35e960_exec(0u, 0x12345678u);
    /* and the word above the return slot is the flag dword (esp = flag - 4) */
    prepare();
    g_esp = T1479_35E960_ADDR - 4u;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(T1479_35E960_ADDR, 1u);
    t1479_35e960_exec(1u, 1u);
    prepare();
    g_esp = T1479_35E960_ADDR - 4u;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(T1479_35E960_ADDR, 0u);
    t1479_35e960_exec(0u, 0u);
    /* a plain call with the flag deliberately different from the word below the return slot */
    prepare();
    g_esp = T1479_35E960_ADDR + 8u;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(T1479_35E960_ADDR, 1u);
    guest_write32(g_esp - 4u, 0u);
    t1479_35e960_exec(1u, 1u);
}
