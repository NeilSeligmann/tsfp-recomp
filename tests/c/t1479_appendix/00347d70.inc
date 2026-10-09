/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00347D70 (append a 0x2C-byte record, legacy vector mode). Expected values were
 * derived by hand from the original instruction listing, not from the draft:
 *   push ebx; ebx = [0x7670F8]; ebx >= 0x20 (signed jge) -> pop ebx, ret (eax/ecx/edx/xmm0 untouched);
 *   push ebp; ebp = [esp+0x14] (arg2); ebp == 0 -> pop ebp, pop ebx, ret; xmm0 = movss [esp+0x10] (arg1);
 *   edx = ebx*0x2C; push esi; push edi; esi = edx+0x767100; eax = 0; rep stosd 11 dwords at esi;
 *   eax = [esp+0x14] (arg0, AFTER the fill); [edx+0x767124] = eax; [edx+0x767120] = xmm0 low;
 *   [edx+0x767128] = 0; ecx = [ebp+0x2C]; ebx++; pop edi; [esi] = ecx; [0x7670F8] = ebx; pop esi;
 *   pop ebp; pop ebx; ret.
 * PUSH/POP: EBX (esp-4), EBP (esp-8), ESI (esp-12), EDI (esp-16). Alias (b): the stack is placed
 * inside the table so the rep stosd fill and the field stores overwrite the saved registers
 * (all four slots, and the ordering of the EDI pop against the [record+0] store), and on the count
 * dword so the count store overwrites the saved EBX. Alias (c): the count is read from the pushed EBX
 * slot, and the source pointer points at each pushed slot. Alias (a): the stack arguments are inside the
 * zeroed record, so arg0 (read after the fill) reads zero while arg1 (read before) is unaffected.
 * MUTANT: g_edi = entry_pop(); -> g_edi = g_ecx;
 * MUTANT: if ((int32_t)g_ebx >= ENTRY_LIMIT) { -> if (g_ebx >= (uint32_t)ENTRY_LIMIT) {
 * MUTANT: g_eax = guest_read32(g_esp + 0x14u); -> g_eax = 0u;
 * MUTANT: guest_write32(ENTRY_COUNT_ADDR, g_ebx); -> guest_write32(ENTRY_COUNT_ADDR, g_ebx - 1u);
 * MUTANT: g_xmm0.u[0] = guest_read32(g_esp + 0x10u); -> g_xmm0.u[0] = guest_read32(g_esp + 0x14u);
 * (The first MUTANT line is the integrated mutation. All of these and several more (pop order, store order against the EDI pop, record size, ordering of the count store against the pops, XMM upper lanes) were run as scratch mutants and killed. Swapping the [record+0] and count stores is an equivalent mutant (the two addresses cannot coincide).) */

/* Alias onto the runtime XMM slot by symbol name so no type clashes with other declarations
 * of g_xmm0 when this file is embedded in tests/c/test_game_replacements.c. */
extern __thread uint32_t t1479a_00347d70_xmm0[4] __asm__("g_xmm0");

#define T1479_00347D70_COUNT 0x7670F8u
#define T1479_00347D70_TABLE 0x767100u
#define T1479_00347D70_OBJ 0x320000u
#define T1479_00347D70_BASE 0x766800u
#define T1479_00347D70_SPAN 0x1100u

static uint32_t t1479_00347d70_s[4]; /* ebx, ebp, esi, edi sentinels */

static void t1479_00347d70_reset(void)
{
    /* stack area below and around the test stacks: other tests leave stale bytes here when this
     * control is embedded in tests/c/test_game_replacements.c */
    memset(&memory[0], 0, 0x200);
    memset(&memory[T1479_00347D70_BASE], 0xEE, T1479_00347D70_SPAN);
    memset(&memory[T1479_00347D70_OBJ], 0x5A, 0x100);
    guest_write32(T1479_00347D70_OBJ + 0x28u, 0xAAAA0028u);
    guest_write32(T1479_00347D70_OBJ + 0x2Cu, 0x12345678u);
    guest_write32(T1479_00347D70_OBJ + 0x30u, 0xBBBB0030u);
    t1479_00347d70_s[0] = 0xB1B1B1B1u;
    t1479_00347d70_s[1] = 0xB2B2B2B2u;
    t1479_00347d70_s[2] = 0xB3B3B3B3u;
    t1479_00347d70_s[3] = 0xB4B4B4B4u;
}

static void t1479_00347d70_count(uint32_t count)
{
    guest_write32(T1479_00347D70_COUNT, count);
}

static void t1479_00347d70_run(uint32_t top, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    prepare();
    g_esp = top;
    guest_write32(top, 0x12345678u);
    guest_write32(top + 4u, arg0);
    guest_write32(top + 8u, arg1);
    guest_write32(top + 12u, arg2);
    g_ebx = t1479_00347d70_s[0];
    g_ebp = t1479_00347d70_s[1];
    g_esi = t1479_00347d70_s[2];
    g_edi = t1479_00347d70_s[3];
    for (unsigned lane = 0; lane < 4; ++lane) {
        t1479a_00347d70_xmm0[lane] = 0xDEADBE00u + lane;
    }
    invoke(0x00347D70u, 4u);
}

static void t1479_00347d70_regs(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t ebp,
                                uint32_t esi, uint32_t edi)
{
    CHECK(g_eax == eax);
    CHECK(g_ecx == ecx);
    CHECK(g_edx == edx);
    CHECK(g_ebx == ebx && g_ebp == ebp);
    CHECK(g_esi == esi && g_edi == edi);
}

static void t1479_00347d70_xmm_unchanged(void)
{
    for (unsigned lane = 0; lane < 4; ++lane) {
        CHECK(t1479a_00347d70_xmm0[lane] == 0xDEADBE00u + lane);
    }
}

static void t1479_00347d70_xmm(uint32_t lane0)
{
    CHECK(t1479a_00347d70_xmm0[0] == lane0);
    CHECK(t1479a_00347d70_xmm0[1] == 0u);
    CHECK(t1479a_00347d70_xmm0[2] == 0u);
    CHECK(t1479a_00347d70_xmm0[3] == 0u);
}

/* The record at `entry`: [0] = ecx, [4..0x1F] zero, [0x20] = value, [0x24] = arg0, [0x28] = 0, and the
 * dwords just outside it keep the 0xEEEEEEEE fill. */
static void t1479_00347d70_record(uint32_t entry, uint32_t ecx, uint32_t value, uint32_t arg0)
{
    CHECK(guest_read32(entry) == ecx);
    for (uint32_t offset = 4u; offset < 0x20u; offset += 4u) {
        CHECK(guest_read32(entry + offset) == 0u);
    }
    CHECK(guest_read32(entry + 0x20u) == value);
    CHECK(guest_read32(entry + 0x24u) == arg0);
    CHECK(guest_read32(entry + 0x28u) == 0u);
    CHECK(guest_read32(entry + 0x2Cu) == 0xEEEEEEEEu);
    CHECK(guest_read32(entry - 4u) == 0xEEEEEEEEu);
}

static void test_t1479_appendix_00347d70(void)
{
    static uint8_t snapshot[T1479_00347D70_SPAN];
    const uint32_t obj = T1479_00347D70_OBJ;
    const uint32_t values[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00000u, 0xFFFFFFFFu, 0x7F800000u,
                               0x00000001u};
    const uint32_t objvals[] = {0u, 0xFFFFFFFFu, 0x12345678u, 0x80000000u, 0x00000001u};

    /* count >= 0x20 (signed): early exit, the pushed EBX slot is the only write */
    {
        const uint32_t counts[] = {0x20u, 0x21u, 0x7FFFFFFFu, 0x40u, 0x1000u};
        for (unsigned k = 0; k < 5; k++) {
            t1479_00347d70_reset();
            t1479_00347d70_count(counts[k]);
            memcpy(snapshot, &memory[T1479_00347D70_BASE], sizeof snapshot);
            t1479_00347d70_run(0x100u, 0xA0A0A0A0u, 0x3F800000u, obj);
            t1479_00347d70_regs(0x11111111u, 0x22222222u, 0x33333333u, t1479_00347d70_s[0],
                                t1479_00347d70_s[1], t1479_00347d70_s[2], t1479_00347d70_s[3]);
            t1479_00347d70_xmm_unchanged();
            CHECK(memcmp(snapshot, &memory[T1479_00347D70_BASE], sizeof snapshot) == 0);
            CHECK(guest_read32(0x100u - 4u) == t1479_00347d70_s[0]);
            CHECK(guest_read32(0x100u - 8u) == 0u);
            CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == 0xA0A0A0A0u);
        }
    }

    /* null source: early exit after the EBX and EBP pushes, nothing else written */
    {
        const uint32_t counts[] = {0u, 5u, 0x1Fu, 0xFFFFFFFFu};
        for (unsigned k = 0; k < 4; k++) {
            t1479_00347d70_reset();
            t1479_00347d70_count(counts[k]);
            memcpy(snapshot, &memory[T1479_00347D70_BASE], sizeof snapshot);
            t1479_00347d70_run(0x100u, 0xA0A0A0A0u, 0x3F800000u, 0u);
            t1479_00347d70_regs(0x11111111u, 0x22222222u, 0x33333333u, t1479_00347d70_s[0],
                                t1479_00347d70_s[1], t1479_00347d70_s[2], t1479_00347d70_s[3]);
            t1479_00347d70_xmm_unchanged();
            CHECK(memcmp(snapshot, &memory[T1479_00347D70_BASE], sizeof snapshot) == 0);
            CHECK(guest_read32(0x100u - 4u) == t1479_00347d70_s[0]);
            CHECK(guest_read32(0x100u - 8u) == t1479_00347d70_s[1]);
            CHECK(guest_read32(0x100u - 12u) == 0u);
        }
    }

    /* normal appends at count 0, 1, 5, 0x1F, with varied value/arg0/source words */
    {
        const uint32_t counts[] = {0u, 1u, 5u, 0x1Fu, 2u, 3u, 4u};
        for (unsigned k = 0; k < 7; k++) {
            const uint32_t entry = T1479_00347D70_TABLE + counts[k] * 0x2Cu;
            const uint32_t arg0 = (k == 0) ? 0u : (k == 1) ? 0xFFFFFFFFu : 0x12345678u + k;
            t1479_00347d70_reset();
            t1479_00347d70_count(counts[k]);
            guest_write32(obj + 0x2Cu, objvals[k % 5u]);
            memset(&memory[entry], 0xEE, 0x2C);
            t1479_00347d70_run(0x100u, arg0, values[k], obj);
            t1479_00347d70_regs(arg0, objvals[k % 5u], counts[k] * 0x2Cu, t1479_00347d70_s[0],
                                t1479_00347d70_s[1], t1479_00347d70_s[2], t1479_00347d70_s[3]);
            t1479_00347d70_xmm(values[k]);
            t1479_00347d70_record(entry, objvals[k % 5u], values[k], arg0);
            CHECK(guest_read32(T1479_00347D70_COUNT) == counts[k] + 1u);
            CHECK(guest_read32(0x100u - 4u) == t1479_00347d70_s[0]);
            CHECK(guest_read32(0x100u - 8u) == t1479_00347d70_s[1]);
            CHECK(guest_read32(0x100u - 12u) == t1479_00347d70_s[2]);
            CHECK(guest_read32(0x100u - 16u) == t1479_00347d70_s[3]);
            CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == arg0);
            CHECK(guest_read32(0x108u) == values[k] && guest_read32(0x10Cu) == obj);
            CHECK(guest_read32(obj + 0x28u) == 0xAAAA0028u && guest_read32(obj + 0x30u) == 0xBBBB0030u);
        }
    }

    /* every float bit pattern is stored as raw bits and lands in xmm0 lane 0, other lanes zeroed */
    for (unsigned k = 0; k < 7; k++) {
        const uint32_t entry = T1479_00347D70_TABLE + 2u * 0x2Cu;
        t1479_00347d70_reset();
        t1479_00347d70_count(2u);
        t1479_00347d70_run(0x100u, 0x1000u + k, values[k], obj);
        t1479_00347d70_xmm(values[k]);
        t1479_00347d70_record(entry, 0x12345678u, values[k], 0x1000u + k);
    }

    /* source pointer arithmetic wraps at 32 bits (0xFFFFFFD4 + 0x2C = 0), and a tiny pointer works */
    t1479_00347d70_reset();
    t1479_00347d70_count(1u);
    guest_write32(0u, 0x600D0001u);
    t1479_00347d70_run(0x100u, 7u, 0x3F000000u, 0xFFFFFFD4u);
    t1479_00347d70_regs(7u, 0x600D0001u, 0x2Cu, t1479_00347d70_s[0], t1479_00347d70_s[1],
                        t1479_00347d70_s[2], t1479_00347d70_s[3]);
    t1479_00347d70_record(T1479_00347D70_TABLE + 0x2Cu, 0x600D0001u, 0x3F000000u, 7u);
    guest_write32(0u, 0u);
    t1479_00347d70_reset();
    t1479_00347d70_count(1u);
    memory[0x2Du] = 0x11;
    memory[0x2Eu] = 0x22;
    memory[0x2Fu] = 0x33;
    memory[0x30u] = 0x44;
    t1479_00347d70_run(0x100u, 7u, 0x3F000000u, 1u); /* source 1: dword at 0x2D */
    t1479_00347d70_regs(7u, 0x44332211u, 0x2Cu, t1479_00347d70_s[0], t1479_00347d70_s[1],
                        t1479_00347d70_s[2], t1479_00347d70_s[3]);
    memset(&memory[0x2Du], 0, 4);

    /* negative counts pass the signed test and index backwards (or wrap to entry 0/1) */
    {
        const uint32_t counts[] = {0x80000000u, 0x80000001u, 0xFFFFFFE0u, 0xFFFFFFFEu};
        const uint32_t edxs[] = {0u, 0x2Cu, 0xFFFFFA80u, 0xFFFFFFA8u};
        for (unsigned k = 0; k < 4; k++) {
            const uint32_t entry = T1479_00347D70_TABLE + edxs[k];
            t1479_00347d70_reset();
            t1479_00347d70_count(counts[k]);
            guest_write32(obj + 0x2Cu, 0x0BADF00Du);
            t1479_00347d70_run(0x100u, 0x1357u, 0x40400000u, obj);
            t1479_00347d70_regs(0x1357u, 0x0BADF00Du, edxs[k], t1479_00347d70_s[0],
                                t1479_00347d70_s[1], t1479_00347d70_s[2], t1479_00347d70_s[3]);
            t1479_00347d70_xmm(0x40400000u);
            CHECK(guest_read32(T1479_00347D70_COUNT) == counts[k] + 1u);
            t1479_00347d70_record(entry, 0x0BADF00Du, 0x40400000u, 0x1357u);
        }
        /* count -1: the record is 0x7670D4..0x7670FF; the arg0 store lands on the count dword and the
         * count store (0) comes last */
        t1479_00347d70_reset();
        t1479_00347d70_count(0xFFFFFFFFu);
        guest_write32(obj + 0x2Cu, 0x0BADF00Du);
        t1479_00347d70_run(0x100u, 0x1357u, 0x40400000u, obj);
        CHECK(guest_read32(T1479_00347D70_COUNT) == 0u);
        CHECK(guest_read32(0x7670F4u) == 0x40400000u);
        CHECK(guest_read32(0x7670FCu) == 0u);
        CHECK(guest_read32(0x7670D0u) == 0xEEEEEEEEu);
        CHECK(guest_read32(0x7670D4u) == 0x0BADF00Du);
        CHECK(guest_read32(0x7670D8u) == 0u);
    }

    /* alias (c): source pointer reads a pushed slot (the pushes are done before the source read) */
    {
        const uint32_t top = 0x790000u; /* far from the table */
        t1479_00347d70_reset();
        t1479_00347d70_s[0] = 0xC1C1C1C1u;
        t1479_00347d70_s[1] = 0xC2C2C2C2u;
        t1479_00347d70_s[2] = 0xC3C3C3C3u;
        t1479_00347d70_s[3] = 0xC4C4C4C4u;
        const uint32_t slots[] = {top - 4u, top - 8u, top - 12u, top - 16u, top, top + 4u, top + 8u};
        const uint32_t want[] = {0xC1C1C1C1u, 0xC2C2C2C2u, 0xC3C3C3C3u, 0xC4C4C4C4u, 0x12345678u,
                                 0x00001357u, 0x40400000u};
        for (unsigned k = 0; k < 7; k++) {
            t1479_00347d70_count(4u);
            memset(&memory[T1479_00347D70_TABLE + 4u * 0x2Cu], 0xEE, 0x2C);
            t1479_00347d70_run(top, 0x1357u, 0x40400000u, slots[k] - 0x2Cu);
            t1479_00347d70_regs(0x1357u, want[k], 4u * 0x2Cu, 0xC1C1C1C1u, 0xC2C2C2C2u, 0xC3C3C3C3u,
                                0xC4C4C4C4u);
            t1479_00347d70_record(T1479_00347D70_TABLE + 4u * 0x2Cu, want[k], 0x40400000u, 0x1357u);
        }
    }

    /* alias (c)+(b): the count is read from the pushed EBX slot at the count dword itself (stack top
     * 0x7670FC). Saved EBX 7 selects entry 7, and the count store (8) overwrites the saved EBX, so
     * the popped EBX is 8; EBP/ESI/EDI slots below stay intact */
    {
        const uint32_t top = 0x7670FCu;
        t1479_00347d70_reset();
        t1479_00347d70_count(0x1Fu); /* ignored: the push overwrites it with the saved EBX */
        t1479_00347d70_s[0] = 7u;
        memset(&memory[T1479_00347D70_TABLE + 7u * 0x2Cu], 0xEE, 0x2C);
        t1479_00347d70_run(top, 0xA1A1A1A1u, 0x40800000u, obj);
        t1479_00347d70_regs(0xA1A1A1A1u, 0x12345678u, 7u * 0x2Cu, 8u, t1479_00347d70_s[1],
                            t1479_00347d70_s[2], t1479_00347d70_s[3]);
        t1479_00347d70_xmm(0x40800000u);
        CHECK(guest_read32(T1479_00347D70_COUNT) == 8u);
        t1479_00347d70_record(T1479_00347D70_TABLE + 7u * 0x2Cu, 0x12345678u, 0x40800000u, 0xA1A1A1A1u);
        /* saved EBX 0: entry 0 is the argument area itself. The fill zeroes arg0, so EAX (read after the
         * fill) is 0, while the value (read before) is still stored. Popped EBX = 1 */
        t1479_00347d70_reset();
        t1479_00347d70_count(0x1Fu);
        t1479_00347d70_s[0] = 0u;
        t1479_00347d70_run(top, 0xA1A1A1A1u, 0x40800000u, obj);
        t1479_00347d70_regs(0u, 0x12345678u, 0u, 1u, t1479_00347d70_s[1], t1479_00347d70_s[2],
                            t1479_00347d70_s[3]);
        t1479_00347d70_xmm(0x40800000u);
        CHECK(guest_read32(T1479_00347D70_COUNT) == 1u);
        CHECK(guest_read32(T1479_00347D70_TABLE) == 0x12345678u);
        CHECK(guest_read32(T1479_00347D70_TABLE + 4u) == 0u);
        CHECK(guest_read32(T1479_00347D70_TABLE + 8u) == 0u);
        CHECK(guest_read32(T1479_00347D70_TABLE + 0x20u) == 0x40800000u);
        CHECK(guest_read32(T1479_00347D70_TABLE + 0x24u) == 0u);
        CHECK(guest_read32(T1479_00347D70_TABLE + 0x28u) == 0u);
        CHECK(guest_read32(top) == 0x12345678u);
    }

    /* alias (b): the stack sits inside record 3 (top = record+0x24): the fill zeroes all four saved
     * registers, the return slot and arg0; the value store then overwrites the saved EBX slot */
    {
        const uint32_t entry = T1479_00347D70_TABLE + 3u * 0x2Cu;
        const uint32_t top = entry + 0x24u;
        t1479_00347d70_reset();
        t1479_00347d70_count(3u);
        guest_write32(obj + 0x2Cu, 0x0C0FFEE0u);
        t1479_00347d70_run(top, 0xA1A1A1A1u, 0x3FC00000u, obj);
        t1479_00347d70_regs(0u, 0x0C0FFEE0u, 3u * 0x2Cu, 0x3FC00000u, 0u, 0u, 0u);
        t1479_00347d70_xmm(0x3FC00000u);
        CHECK(guest_read32(T1479_00347D70_COUNT) == 4u);
        CHECK(guest_read32(entry) == 0x0C0FFEE0u);
        CHECK(guest_read32(entry + 0x20u) == 0x3FC00000u);
        CHECK(guest_read32(entry + 0x24u) == 0u && guest_read32(entry + 0x28u) == 0u);
        CHECK(guest_read32(entry + 0x2Cu) == 0x3FC00000u); /* the value argument slot, outside the fill */
        CHECK(guest_read32(entry + 0x30u) == obj);
        for (uint32_t offset = 4u; offset < 0x20u; offset += 4u) {
            CHECK(guest_read32(entry + offset) == 0u);
        }
    }
    /* alias (b): the stack starts at the record (saved EDI slot = record+0): EDI is popped BEFORE the
     * [record+0] = ECX store, so it sees the fill's zero, while ESI/EBP/EBX (record+4/8/0xC) read the
     * zeroed slots too */
    {
        const uint32_t entry = T1479_00347D70_TABLE + 3u * 0x2Cu;
        const uint32_t top = entry + 16u;
        t1479_00347d70_reset();
        t1479_00347d70_count(3u);
        guest_write32(obj + 0x2Cu, 0x0C0FFEE0u);
        t1479_00347d70_run(top, 0xA1A1A1A1u, 0x3FC00000u, obj);
        t1479_00347d70_regs(0u, 0x0C0FFEE0u, 3u * 0x2Cu, 0u, 0u, 0u, 0u);
        t1479_00347d70_xmm(0x3FC00000u);
        CHECK(guest_read32(T1479_00347D70_COUNT) == 4u);
        CHECK(guest_read32(entry) == 0x0C0FFEE0u);
        CHECK(guest_read32(entry + 4u) == 0u && guest_read32(entry + 8u) == 0u);
        CHECK(guest_read32(entry + 0x20u) == 0x3FC00000u);
        CHECK(guest_read32(entry + 0x24u) == 0u);
    }
    /* alias (a)+(c): the source pointer points into the record, so [source+0x2C] reads a field that was
     * just written (arg0 at +0x24, the value at +0x20, a zeroed field at +0x28 and +4) */
    {
        const uint32_t entry = T1479_00347D70_TABLE + 2u * 0x2Cu;
        const uint32_t fields[] = {0x24u, 0x20u, 0x28u, 0x04u};
        const uint32_t want[] = {0xA1A1A1A1u, 0x40000000u, 0u, 0u};
        for (unsigned k = 0; k < 4; k++) {
            t1479_00347d70_reset();
            t1479_00347d70_count(2u);
            t1479_00347d70_run(0x100u, 0xA1A1A1A1u, 0x40000000u, entry + fields[k] - 0x2Cu);
            t1479_00347d70_regs(0xA1A1A1A1u, want[k], 2u * 0x2Cu, t1479_00347d70_s[0],
                                t1479_00347d70_s[1], t1479_00347d70_s[2], t1479_00347d70_s[3]);
            CHECK(guest_read32(entry) == want[k]);
            CHECK(guest_read32(entry + 0x24u) == 0xA1A1A1A1u);
        }
    }
    /* alias (a): only the arg0 slot is inside the record (record+0x28 = arg0 slot): arg0 reads the
     * zero the fill left there, arg1/arg2 (outside) are read normally */
    {
        const uint32_t entry = T1479_00347D70_TABLE + 3u * 0x2Cu;
        const uint32_t top2 = entry + 0x24u; /* ret = record+0x24, arg0 = record+0x28 */
        t1479_00347d70_reset();
        t1479_00347d70_count(3u);
        t1479_00347d70_s[0] = 0xC1C1C1C1u;
        t1479_00347d70_run(top2, 0x77777777u, 0x40000000u, obj);
        CHECK(g_eax == 0u);
        CHECK(guest_read32(entry + 0x24u) == 0u && guest_read32(entry + 0x28u) == 0u);
        CHECK(guest_read32(entry + 0x2Cu) == 0x40000000u);
        CHECK(guest_read32(entry + 0x20u) == 0x40000000u);
    }
    memset(&memory[T1479_00347D70_BASE], 0, T1479_00347D70_SPAN);
    memset(&memory[0x790000u - 0x20u], 0, 0x40);
}
