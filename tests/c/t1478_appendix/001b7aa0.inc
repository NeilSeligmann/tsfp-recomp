/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x001B7AA0, derived from the original instruction listing:
 *   edx=[esp+4]; xorps xmm0; eax=0; push edi; ecx=0xF6; edi=edx; rep stosd (eax=0);
 *   ecx=-1; [edx+30]=ecx; [edx+44]=eax; [edx+34]=eax; [edx+54]=eax; [edx+5C]=eax; [edx+C8]=ecx;
 *   ecx=[esp+0x10]; [edx+68]=eax; eax=[esp+0xC]; [edx+48]=xmm0 low (0); [edx+3CC]=eax;
 *   [edx+3C]=ecx; pop edi; ret.
 * After the push, [esp+0xC] is the SECOND stack argument and [esp+0x10] the THIRD. Both are read
 * after the fill and the first six stores, and EDI is popped from the saved slot at the end. */
extern __thread uint32_t t1478a_001b7aa0_xmm0[4] __asm__("g_xmm0");

typedef struct {
    uint32_t record;
    uint32_t exp_eax;   /* hand-derived value of the second stack argument as read */
    uint32_t exp_ecx;   /* hand-derived value of the third stack argument as read */
    uint32_t exp_edi;   /* hand-derived popped value */
} T1478A7aa0Case;

#define T1478A_7AA0_STACK 0x5000u
#define T1478A_7AA0_EDI 0xD1D1D1D1u
#define T1478A_7AA0_A 0xA1A1A1A1u
#define T1478A_7AA0_B 0xA2A2A2A2u

static uint32_t t1478a_7aa0_canary(uint32_t address)
{
    return 0xC0DE0000u ^ (address * 2654435761u);
}

/* Expected dword at 'address' after the run: canary, saved EDI push, fill, then the ordered
 * stores (values taken from the hand-derived reads of the case). */
static uint32_t t1478a_7aa0_expected(const T1478A7aa0Case *c, uint32_t address)
{
    const uint32_t stack = T1478A_7AA0_STACK;
    uint32_t value = t1478a_7aa0_canary(address);
    if (address == stack) value = 0x12345678u;
    if (address == stack + 4u) value = c->record;
    if (address == stack + 8u) value = T1478A_7AA0_A;
    if (address == stack + 12u) value = T1478A_7AA0_B;
    if (address == stack - 4u) value = T1478A_7AA0_EDI;       /* push edi */
    if (address - c->record < 0x3D8u) value = 0u;             /* rep stosd fill */
    const uint32_t offsets[] = {0x30u, 0x44u, 0x34u, 0x54u, 0x5Cu, 0xC8u,
                                0x68u, 0x48u, 0x3CCu, 0x3Cu};
    const uint32_t values[] = {0xFFFFFFFFu, 0u, 0u, 0u, 0u, 0xFFFFFFFFu,
                               0u, 0u, c->exp_eax, c->exp_ecx};
    for (unsigned k = 0; k < 10; ++k)
        if (address == c->record + offsets[k]) value = values[k];
    return value;
}

static void t1478a_7aa0_run(const T1478A7aa0Case *c)
{
    prepare();
    for (uint32_t address = 0x4B00u; address < 0x5600u; address += 4u)
        guest_write32(address, t1478a_7aa0_canary(address));
    for (uint32_t offset = 0; offset < 0x3E0u; offset += 4u)
        guest_write32(c->record - 8u + offset, t1478a_7aa0_canary(c->record - 8u + offset));
    g_esp = T1478A_7AA0_STACK;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, c->record);
    guest_write32(g_esp + 8u, T1478A_7AA0_A);
    guest_write32(g_esp + 12u, T1478A_7AA0_B);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = T1478A_7AA0_EDI; g_ebp = 0xBBBB0001u;
    g_df = 0;
    for (unsigned lane = 0; lane < 4; ++lane) t1478a_001b7aa0_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x001B7AA0u, 4u);
    CHECK(g_eax == c->exp_eax && g_ecx == c->exp_ecx && g_edx == c->record);
    CHECK(g_edi == c->exp_edi);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u && g_ebp == 0xBBBB0001u);
    for (unsigned lane = 0; lane < 4; ++lane) CHECK(t1478a_001b7aa0_xmm0[lane] == 0u);
    CHECK(g_esp == T1478A_7AA0_STACK + 4u);
    for (uint32_t address = 0x4B00u; address < 0x5600u; address += 4u)
        CHECK(guest_read32(address) == t1478a_7aa0_expected(c, address));
    for (uint32_t offset = 0; offset < 0x3E0u; offset += 4u) {
        const uint32_t address = c->record - 8u + offset;
        CHECK(guest_read32(address) == t1478a_7aa0_expected(c, address));
    }
    /* the popped EDI is the final content of the saved slot */
    CHECK(g_edi == t1478a_7aa0_expected(c, T1478A_7AA0_STACK - 4u));
}

static void test_t1478_appendix_001b7aa0(void)
{
    const T1478A7aa0Case cases[] = {
        /* ordinary record away from the stack: all fields and neighbours */
        {0x20000u, T1478A_7AA0_A, T1478A_7AA0_B, T1478A_7AA0_EDI},
        /* record starts at the return slot: argument slots are zeroed before they are read */
        {0x5000u, 0u, 0u, T1478A_7AA0_EDI},
        /* fill starts at the third argument: B zeroed, A intact */
        {0x500Cu, T1478A_7AA0_A, 0u, T1478A_7AA0_EDI},
        /* fill ends before the third argument and covers the saved slot: A zeroed, B intact,
         * EDI popped as the zeroed slot; the +0x3CC store lands on the return slot */
        {0x4C34u, 0u, T1478A_7AA0_B, 0u},
        /* fill ends before the second argument; the +0x3CC store (value A) overwrites the
         * saved EDI slot, so the pop returns A and not the entry EDI */
        {0x4C30u, T1478A_7AA0_A, T1478A_7AA0_B, T1478A_7AA0_A},
        /* +0x30 (-1) lands on the second argument slot and +0x34 (0) on the third: the reads
         * see -1 and 0 even though the fill zeroed them first; the saved slot is zeroed */
        {0x4FD8u, 0xFFFFFFFFu, 0u, 0u},
        /* +0x30 store overwrites the saved EDI slot with -1 (fill covers the arguments) */
        {0x4FCCu, 0u, 0u, 0xFFFFFFFFu},
        /* fill covers the saved slot only (ends before the arguments) */
        {0x4C2Cu, T1478A_7AA0_A, T1478A_7AA0_B, 0u},
        /* +0x3C store (value B read as zero) lands on the saved slot */
        {0x4FC0u, 0u, 0u, 0u},
        /* +0xC8 store (-1) lands on the saved slot */
        {0x4F34u, 0u, 0u, 0xFFFFFFFFu},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) t1478a_7aa0_run(&cases[i]);
}
