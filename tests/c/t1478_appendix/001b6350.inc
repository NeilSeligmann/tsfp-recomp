/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native control for 0x001B6350 (find a 12-byte table entry by id and mode). Expected values
 * were derived by hand from the original instructions, not from the draft:
 *   args (after the 4 saves esp+0x14/0x18/0x1C): record, id, mode.
 *   count = [record+0x194]; count <= 0 (signed) -> eax = 0, edx = 0, ecx untouched.
 *   per entry (stride 12): ecx = sx16(word0); if ecx == id: cx = word1 (low 16 bits only),
 *   ebp = sx16(cx); mode == ebp -> return the entry (eax = entry, edx = index);
 *   else if (cx == 2 && mode == 0) or (cx == 0 && mode == 2): ebx = entry (last wins).
 *   Loop end: eax = ebx, edx = count, ecx = value left by the last scanned entry.
 *   ebx/ebp/esi/edi are saved by real pushes at esp-4/-8/-12/-16 and popped back. */

#define T1478_B6350_REC 0x300000u
#define T1478_B6350_TAB 0x301000u

static uint32_t t1478_001b6350_sent[4]; /* ebx, ebp, esi, edi entry values */

static void t1478_001b6350_put16(uint32_t address, uint32_t value)
{
    memory[address] = (uint8_t)value;
    memory[address + 1u] = (uint8_t)(value >> 8);
}

/* Write entry `index` of the table at TAB (bytes 4..11 are garbage that must be ignored). */
static void t1478_001b6350_entry(uint32_t index, uint32_t id, uint32_t mode)
{
    const uint32_t at = T1478_B6350_TAB + 12u * index;
    t1478_001b6350_put16(at, id);
    t1478_001b6350_put16(at + 2u, mode);
    memset(&memory[at + 4u], 0xEE, 8u);
}

static void t1478_001b6350_reset(void)
{
    memset(&memory[0x300000], 0, 0x2000);
    memset(&memory[0x3FE00], 0, 0x300);
    t1478_001b6350_sent[0] = 0xB1B1B1B1u;
    t1478_001b6350_sent[1] = 0xB2B2B2B2u;
    t1478_001b6350_sent[2] = 0xB3B3B3B3u;
    t1478_001b6350_sent[3] = 0xB4B4B4B4u;
}

static void t1478_001b6350_setup(uint32_t count)
{
    guest_write32(T1478_B6350_REC + 0x190u, T1478_B6350_TAB);
    guest_write32(T1478_B6350_REC + 0x194u, count);
}

static void t1478_001b6350_run(uint32_t top, uint32_t rec, uint32_t id, uint32_t mode)
{
    static uint8_t snapshot[0x2000];
    memcpy(snapshot, &memory[0x300000], sizeof snapshot);
    prepare();
    g_esp = top;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, rec);
    guest_write32(g_esp + 8u, id);
    guest_write32(g_esp + 12u, mode);
    g_ebx = t1478_001b6350_sent[0];
    g_ebp = t1478_001b6350_sent[1];
    g_esi = t1478_001b6350_sent[2];
    g_edi = t1478_001b6350_sent[3];
    invoke(0x001B6350u, 4u);
    CHECK(g_ebx == t1478_001b6350_sent[0] && g_ebp == t1478_001b6350_sent[1]);
    CHECK(g_esi == t1478_001b6350_sent[2] && g_edi == t1478_001b6350_sent[3]);
    /* the four saves stay in the dead area below the return slot */
    CHECK(guest_read32(top - 4u) == t1478_001b6350_sent[0]);
    CHECK(guest_read32(top - 8u) == t1478_001b6350_sent[1]);
    CHECK(guest_read32(top - 12u) == t1478_001b6350_sent[2]);
    CHECK(guest_read32(top - 16u) == t1478_001b6350_sent[3]);
    CHECK(guest_read32(top) == 0x12345678u);
    CHECK(guest_read32(top + 4u) == rec && guest_read32(top + 8u) == id);
    CHECK(guest_read32(top + 12u) == mode);
    if (top == 0x100u) {
        CHECK(memcmp(snapshot, &memory[0x300000], sizeof snapshot) == 0);
    }
}

static void t1478_001b6350_expect(uint32_t eax, uint32_t ecx, uint32_t edx)
{
    CHECK(g_eax == eax);
    CHECK(g_ecx == ecx);
    CHECK(g_edx == edx);
}

#define T1478_B6350_E(i) (T1478_B6350_TAB + 12u * (i))

static void test_t1478_appendix_001b6350(void)
{
    /* count <= 0 (signed): nothing scanned, eax = 0, edx = 0, ecx untouched */
    {
        const uint32_t counts[] = {0u, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu};
        for (unsigned k = 0; k < 4; k++) {
            t1478_001b6350_reset();
            t1478_001b6350_setup(counts[k]);
            t1478_001b6350_entry(0, 7, 0);
            t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0u);
            t1478_001b6350_expect(0u, 0x22222222u, 0u);
        }
    }

    /* table X: (5,1) (7,0) (7,2) (9,3) */
    t1478_001b6350_reset();
    t1478_001b6350_setup(4u);
    t1478_001b6350_entry(0, 5, 1);
    t1478_001b6350_entry(1, 7, 0);
    t1478_001b6350_entry(2, 7, 2);
    t1478_001b6350_entry(3, 9, 3);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 1u); /* nothing: last entry id differs */
    t1478_001b6350_expect(0u, 9u, 4u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 2u); /* e2 exact beats e1 fallback */
    t1478_001b6350_expect(T1478_B6350_E(2), 2u, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0u); /* e1 exact */
    t1478_001b6350_expect(T1478_B6350_E(1), 0u, 1u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 5u, 2u); /* id only matches, mode 1 vs 2 */
    t1478_001b6350_expect(0u, 9u, 4u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 5u, 1u);
    t1478_001b6350_expect(T1478_B6350_E(0), 1u, 0u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 9u, 3u);
    t1478_001b6350_expect(T1478_B6350_E(3), 3u, 3u);
    /* last entry matches the id: ecx keeps only the mode word (high bits 0) */
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 9u, 2u);
    t1478_001b6350_expect(0u, 3u, 4u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 8u, 0u); /* no id match anywhere */
    t1478_001b6350_expect(0u, 9u, 4u);
    /* count limits the scan: entry 3 is not examined */
    t1478_001b6350_setup(3u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 9u, 3u);
    t1478_001b6350_expect(0u, 7u, 3u);
    t1478_001b6350_setup(1u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 5u, 1u);
    t1478_001b6350_expect(T1478_B6350_E(0), 1u, 0u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0u);
    t1478_001b6350_expect(0u, 5u, 1u);
    /* fallback e1 (mode word 0 vs requested 2) when the scan stops at count 2 */
    t1478_001b6350_setup(2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 2u);
    t1478_001b6350_expect(T1478_B6350_E(1), 0u, 2u);
    /* a huge positive count still returns at the first exact entry */
    t1478_001b6350_setup(0x7FFFFFFFu);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 5u, 1u);
    t1478_001b6350_expect(T1478_B6350_E(0), 1u, 0u);

    /* table Y: fallbacks (7,2) (7,2) (8,8); mode argument 0 accepts mode word 2, last wins */
    t1478_001b6350_reset();
    t1478_001b6350_setup(3u);
    t1478_001b6350_entry(0, 7, 2);
    t1478_001b6350_entry(1, 7, 2);
    t1478_001b6350_entry(2, 8, 8);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0u);
    t1478_001b6350_expect(T1478_B6350_E(1), 8u, 3u);
    t1478_001b6350_setup(2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0u);
    t1478_001b6350_expect(T1478_B6350_E(1), 2u, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 1u); /* no fallback for mode 1 */
    t1478_001b6350_expect(0u, 2u, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0x10000u); /* mode is a 32-bit compare */
    t1478_001b6350_expect(0u, 2u, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0x10002u);
    t1478_001b6350_expect(0u, 2u, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 0x10007u, 2u); /* id is a 32-bit compare */
    t1478_001b6350_expect(0u, 7u, 2u);

    /* table Z: (7,0) (7,2): an exact entry beats an earlier fallback */
    t1478_001b6350_reset();
    t1478_001b6350_setup(2u);
    t1478_001b6350_entry(0, 7, 0);
    t1478_001b6350_entry(1, 7, 2);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 2u);
    t1478_001b6350_expect(T1478_B6350_E(1), 2u, 1u);
    t1478_001b6350_entry(0, 7, 2);
    t1478_001b6350_entry(1, 7, 0);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0u);
    t1478_001b6350_expect(T1478_B6350_E(1), 0u, 1u);
    /* mode word 0 with a nonzero requested mode other than 2 is no fallback */
    t1478_001b6350_entry(0, 7, 0);
    t1478_001b6350_entry(1, 7, 0);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0x10000u);
    t1478_001b6350_expect(0u, 0u, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 7u, 0x10002u);
    t1478_001b6350_expect(0u, 0u, 2u);

    /* negative words: sign extension of both words, high half of ecx from the id word */
    t1478_001b6350_reset();
    t1478_001b6350_setup(1u);
    t1478_001b6350_entry(0, 0xFFFEu, 0xFFFFu);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 0xFFFFFFFEu, 0xFFFFFFFFu);
    t1478_001b6350_expect(T1478_B6350_E(0), 0xFFFFFFFFu, 0u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 0xFFFEu, 0xFFFFFFFFu);
    t1478_001b6350_expect(0u, 0xFFFFFFFEu, 1u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 0xFFFFFFFEu, 0xFFFFu);
    t1478_001b6350_expect(0u, 0xFFFFFFFFu, 1u);
    t1478_001b6350_entry(0, 0xFFFEu, 2u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 0xFFFFFFFEu, 0u); /* fallback, ecx high kept */
    t1478_001b6350_expect(T1478_B6350_E(0), 0xFFFF0002u, 1u);
    t1478_001b6350_entry(0, 0x7FFFu, 0x8000u);
    t1478_001b6350_run(0x100u, T1478_B6350_REC, 0x7FFFu, 0xFFFF8000u);
    t1478_001b6350_expect(T1478_B6350_E(0), 0x8000u, 0u);

    /* saved-register slots double as table memory: the pushes happen before any table read */
    t1478_001b6350_reset();
    {
        const uint32_t top = 0x40000u;
        const uint32_t rec = 0x3FE5Cu; /* rec+0x194 is the push slot of edi (top-16) */
        /* count read from the edi slot */
        guest_write32(rec + 0x190u, T1478_B6350_TAB);
        t1478_001b6350_entry(0, 5, 1);
        t1478_001b6350_entry(1, 7, 0);
        t1478_001b6350_sent[3] = 2u;
        t1478_001b6350_run(top, rec, 7u, 0u);
        t1478_001b6350_expect(T1478_B6350_E(1), 0u, 1u);
        t1478_001b6350_sent[3] = 0u; /* count 0 -> nothing */
        t1478_001b6350_run(top, rec, 7u, 0u);
        t1478_001b6350_expect(0u, 0x22222222u, 0u);

        /* table inside the pushed area */
        t1478_001b6350_reset();
        t1478_001b6350_setup(1u);
        guest_write32(T1478_B6350_REC + 0x190u, top - 16u); /* entry 0 = edi slot */
        t1478_001b6350_sent[3] = 0x00020007u;
        t1478_001b6350_run(top, T1478_B6350_REC, 7u, 2u);
        t1478_001b6350_expect(top - 16u, 2u, 0u);
        t1478_001b6350_run(top, T1478_B6350_REC, 7u, 0u); /* fallback */
        t1478_001b6350_expect(top - 16u, 2u, 1u);
        guest_write32(T1478_B6350_REC + 0x190u, top - 8u); /* entry 0 = ebp slot */
        t1478_001b6350_sent[3] = 0xB4B4B4B4u;
        t1478_001b6350_sent[1] = 0x00020007u;
        t1478_001b6350_run(top, T1478_B6350_REC, 7u, 2u);
        t1478_001b6350_expect(top - 8u, 2u, 0u);
        guest_write32(T1478_B6350_REC + 0x190u, top - 4u); /* entry 0 = ebx slot */
        t1478_001b6350_sent[1] = 0xB2B2B2B2u;
        t1478_001b6350_sent[0] = 0xFFFFFFFEu;
        t1478_001b6350_run(top, T1478_B6350_REC, 0xFFFFFFFEu, 0xFFFFFFFFu);
        t1478_001b6350_expect(top - 4u, 0xFFFFFFFFu, 0u);
        /* esi slot then the return address: two entries straddling the stack top */
        t1478_001b6350_setup(2u);
        guest_write32(T1478_B6350_REC + 0x190u, top - 12u);
        t1478_001b6350_sent[0] = 0xB1B1B1B1u;
        t1478_001b6350_sent[2] = 0x00030009u;
        t1478_001b6350_run(top, T1478_B6350_REC, 0x5678u, 0x1234u);
        t1478_001b6350_expect(top, 0x1234u, 1u);
        t1478_001b6350_run(top, T1478_B6350_REC, 9u, 3u);
        t1478_001b6350_expect(top - 12u, 3u, 0u);
    }
}
