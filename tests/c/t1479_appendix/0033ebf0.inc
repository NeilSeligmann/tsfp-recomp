/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0033EBF0 (copy a counted list of words from packed to stride-8 form). Expected
 * values were derived by hand from the original instruction listing, not from the draft:
 *   eax = [esp+4] (dest); push esi; esi = [esp+0xC] (source); ecx = movzx byte [esi]; [eax] = ecx (dword);
 *   cl = byte [esi] (re-read); edx = 0; cl == 0 -> pop esi, ret (eax = dest, ecx = cl, edx = 0).
 *   Else ecx = dest+4; eax = source+2; push edi; loop: word [ecx] = word [eax]; edi = movzx byte [esi]
 *   (re-read); edx++; eax += 4; ecx += 8; repeat while edx < edi (signed); pop edi; pop esi.
 * PUSH/POP: ESI is pushed at entry esp-4 always, EDI at esp-8 on the loop path only, and both pops read
 * the slot back after the function's own stores. Alias (b) is covered by destinations that overwrite
 * the ESI slot (dword count, 16-bit word) and the EDI slot (16-bit word, and a dword count that the
 * EDI push then overwrites). Alias (c) is covered by sources that point at the ESI slot and at the
 * EDI slot (the count byte is re-read after the push). Alias (a) is covered by destinations on the
 * return-address and argument slots and a source on the return-address slot. Overlapping dest and
 * source pin the count re-reads.
 * MUTANT: g_ecx += 8u; -> g_ecx += 6u;
 * MUTANT: g_esi = counted_words_pop(); -> (void)counted_words_pop();
 * MUTANT: g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_esi); -> g_ecx = g_ecx | 0u;
 * MUTANT: counted_words_push(g_edi); -> counted_words_push(0xDEADu);
 * MUTANT: g_edi = guest_read8(g_esi); -> g_edi = (g_edx == 0u) ? guest_read8(g_esi) : g_edi;
 * (The first MUTANT line is the integrated mutation. All five were run as scratch mutants and killed.) */

#define T1479_0033EBF0_SRC 0x300100u
#define T1479_0033EBF0_DST 0x301000u
#define T1479_0033EBF0_BASE 0x300000u
#define T1479_0033EBF0_SPAN 0x3000u

static uint32_t t1479_0033ebf0_esi_s, t1479_0033ebf0_edi_s;

static uint32_t t1479_0033ebf0_r16(uint32_t address)
{
    return (uint32_t)memory[address] | ((uint32_t)memory[address + 1u] << 8);
}

static void t1479_0033ebf0_w16(uint32_t address, uint32_t value)
{
    memory[address] = (uint8_t)value;
    memory[address + 1u] = (uint8_t)(value >> 8);
}

static void t1479_0033ebf0_reset(void)
{
    memset(&memory[T1479_0033EBF0_BASE], 0xCC, T1479_0033EBF0_SPAN);
    memset(&memory[0x380], 0, 0x400);
    t1479_0033ebf0_esi_s = 0xB3B3B3B3u;
    t1479_0033ebf0_edi_s = 0xB4B4B4B4u;
}

/* Packed source list: count byte, junk byte, then n words at +2+4*i with two junk bytes between. */
static void t1479_0033ebf0_src(uint32_t src, uint32_t count, const uint16_t *words, unsigned n)
{
    memory[src] = (uint8_t)count;
    memory[src + 1u] = 0x77;
    for (unsigned i = 0; i < n; i++) {
        t1479_0033ebf0_w16(src + 2u + 4u * i, words[i]);
        memory[src + 4u + 4u * i] = 0xDD;
        memory[src + 5u + 4u * i] = 0xDD;
    }
}

static void t1479_0033ebf0_run(uint32_t top, uint32_t dst, uint32_t src, uint32_t eax,
                               uint32_t ecx, uint32_t edx, uint32_t esi_x, uint32_t edi_x)
{
    prepare();
    g_esp = top;
    guest_write32(top, 0x12345678u);
    guest_write32(top + 4u, dst);
    guest_write32(top + 8u, src);
    g_ebx = 0xB1B1B1B1u;
    g_ebp = 0xB2B2B2B2u;
    g_esi = t1479_0033ebf0_esi_s;
    g_edi = t1479_0033ebf0_edi_s;
    invoke(0x0033EBF0u, 4u);
    CHECK(g_eax == eax);
    CHECK(g_ecx == ecx);
    CHECK(g_edx == edx);
    CHECK(g_esi == esi_x);
    CHECK(g_edi == edi_x);
    CHECK(g_ebx == 0xB1B1B1B1u && g_ebp == 0xB2B2B2B2u);
}

/* Destination list check: count dword, n words at +4+8*i, every other byte of the span untouched. */
static void t1479_0033ebf0_list(uint32_t dst, uint32_t count, const uint16_t *words, unsigned n)
{
    CHECK(guest_read32(dst) == count);
    for (unsigned i = 0; i < n; i++) {
        CHECK(t1479_0033ebf0_r16(dst + 4u + 8u * i) == words[i]);
        CHECK(memory[dst + 6u + 8u * i] == 0xCC && memory[dst + 7u + 8u * i] == 0xCC);
    }
    CHECK(memory[dst - 1u] == 0xCC);
    CHECK(memory[dst + 4u + 8u * n] == 0xCC && memory[dst + 5u + 8u * n] == 0xCC);
}

static void test_t1479_appendix_0033ebf0(void)
{
    const uint32_t src = T1479_0033EBF0_SRC;
    const uint32_t dst = T1479_0033EBF0_DST;
    const uint16_t w[8] = {0x1111u, 0x2222u, 0x8003u, 0xFFFFu, 0x0000u, 0x7FFFu, 0x0100u, 0xABCDu};
    uint16_t big[256];
    for (unsigned i = 0; i < 256; i++) {
        big[i] = (uint16_t)(0x9E37u * (i + 1u));
    }

    /* count 0: dword zero stored, nothing else written, no EDI push, ESI slot still pushed */
    t1479_0033ebf0_reset();
    t1479_0033ebf0_src(src, 0u, w, 8);
    guest_write32(0x100u - 8u, 0x0EDEDEDEu);
    t1479_0033ebf0_run(0x100u, dst, src, dst, 0u, 0u, t1479_0033ebf0_esi_s, t1479_0033ebf0_edi_s);
    t1479_0033ebf0_list(dst, 0u, w, 0);
    CHECK(guest_read32(0x100u - 4u) == t1479_0033ebf0_esi_s);
    CHECK(guest_read32(0x100u - 8u) == 0x0EDEDEDEu);
    CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == dst);
    CHECK(guest_read32(0x108u) == src);

    /* counts 1 2 3 5 8 */
    {
        const unsigned counts[] = {1, 2, 3, 5, 8};
        for (unsigned k = 0; k < 5; k++) {
            const unsigned n = counts[k];
            t1479_0033ebf0_reset();
            t1479_0033ebf0_src(src, n, w, n);
            guest_write32(0x100u - 8u, 0u);
            t1479_0033ebf0_run(0x100u, dst, src, src + 2u + 4u * n, dst + 4u + 8u * n, n,
                               t1479_0033ebf0_esi_s, t1479_0033ebf0_edi_s);
            t1479_0033ebf0_list(dst, n, w, n);
            CHECK(guest_read32(0x100u - 4u) == t1479_0033ebf0_esi_s);
            CHECK(guest_read32(0x100u - 8u) == t1479_0033ebf0_edi_s);
            CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == dst);
            CHECK(guest_read32(0x108u) == src);
            /* the source list is not modified */
            CHECK(memory[src] == n && memory[src + 1u] == 0x77);
            for (unsigned i = 0; i < n; i++) {
                CHECK(t1479_0033ebf0_r16(src + 2u + 4u * i) == w[i]);
            }
        }
    }

    /* byte-wide count: 0x7F 0x80 0x81 0xFF are zero-extended (never sign-extended) */
    {
        const unsigned counts[] = {0x7F, 0x80, 0x81, 0xFF, 0x40};
        for (unsigned k = 0; k < 5; k++) {
            const unsigned n = counts[k];
            t1479_0033ebf0_reset();
            t1479_0033ebf0_src(src, n, big, n);
            t1479_0033ebf0_run(0x100u, dst, src, src + 2u + 4u * n, dst + 4u + 8u * n, n,
                               t1479_0033ebf0_esi_s, t1479_0033ebf0_edi_s);
            t1479_0033ebf0_list(dst, n, big, n);
            CHECK(guest_read32(0x100u - 4u) == t1479_0033ebf0_esi_s);
            CHECK(guest_read32(0x100u - 8u) == t1479_0033ebf0_edi_s);
        }
    }

    /* dest = source - 1, -2, -3: the count dword store zeroes the source count byte, so the re-read
     * gives zero: eax = dest, ecx = 0, edx = 0, no EDI push */
    for (uint32_t back = 1u; back <= 3u; back++) {
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        guest_write32(0x100u - 8u, 0x0EDEDEDEu);
        t1479_0033ebf0_run(0x100u, src - back, src, src - back, 0u, 0u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(memory[src - back] == 2u);
        CHECK(memory[src] == 0u);
        CHECK(guest_read32(0x100u - 8u) == 0x0EDEDEDEu);
        CHECK(guest_read32(0x100u - 4u) == t1479_0033ebf0_esi_s);
    }
    /* dest = source - 4: no overlap with the count dword. Word 0 = 0x0102 is stored onto the source
     * count byte (count becomes 2), the loop ends after 2 iterations instead of 3 */
    {
        const uint16_t ww[3] = {0x0102u, 0x3344u, 0x5566u};
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 3u, ww, 3);
        t1479_0033ebf0_run(0x100u, src - 4u, src, src + 10u, src + 16u, 2u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(src - 4u) == 3u);
        CHECK(memory[src] == 0x02u && memory[src + 1u] == 0x01u);
        CHECK(t1479_0033ebf0_r16(src + 8u) == 0x3344u);
        CHECK(memory[src + 16u] == 0xCC && memory[src + 17u] == 0xCC);
        /* the same with word 0 raising the count to 5: five iterations, the later words are read
         * from the now partly overwritten source */
        t1479_0033ebf0_reset();
        {
            const uint16_t wr[3] = {0x0105u, 0x3344u, 0x5566u};
            t1479_0033ebf0_src(src, 3u, wr, 3);
        }
        t1479_0033ebf0_run(0x100u, src - 4u, src, src + 22u, src + 40u, 5u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(src - 4u) == 3u);
        CHECK(memory[src] == 0x05u && memory[src + 1u] == 0x01u);
        CHECK(t1479_0033ebf0_r16(src + 8u) == 0x3344u);
        CHECK(t1479_0033ebf0_r16(src + 16u) == 0x5566u);
        CHECK(t1479_0033ebf0_r16(src + 24u) == 0xCCCCu);
        CHECK(t1479_0033ebf0_r16(src + 32u) == 0xCCCCu);
        CHECK(memory[src + 40u] == 0xCC);
    }
    /* dest = source - 12: the word-1 store (iteration 1) lands on the source count byte, so the bound
     * changes in the middle of the loop. Count 5 becomes 3, and count 5 becomes 7 */
    {
        const uint16_t ww[5] = {0x1111u, 0x0103u, 0x5566u, 0x7788u, 0x99AAu};
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 5u, ww, 5);
        t1479_0033ebf0_run(0x100u, src - 12u, src, src + 14u, src + 16u, 3u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(src - 12u) == 5u);
        CHECK(memory[src] == 0x03u && memory[src + 1u] == 0x01u);
        CHECK(t1479_0033ebf0_r16(src - 8u) == 0x1111u);
        CHECK(t1479_0033ebf0_r16(src + 8u) == 0x5566u);
        CHECK(t1479_0033ebf0_r16(src + 16u) == 0xDDDDu); /* gap byte pair after word 3, not stored */
        CHECK(memory[src + 18u] == 0xAA);
        t1479_0033ebf0_reset();
        {
            const uint16_t wu[5] = {0x1111u, 0x0107u, 0x5566u, 0x7788u, 0x99AAu};
            t1479_0033ebf0_src(src, 5u, wu, 5);
        }
        t1479_0033ebf0_run(0x100u, src - 12u, src, src + 30u, src + 48u, 7u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(memory[src] == 0x07u && memory[src + 1u] == 0x01u);
        CHECK(t1479_0033ebf0_r16(src + 8u) == 0x5566u);
        CHECK(t1479_0033ebf0_r16(src + 16u) == 0x7788u);
        CHECK(t1479_0033ebf0_r16(src + 24u) == 0x99AAu);
        CHECK(t1479_0033ebf0_r16(src + 32u) == 0xCCCCu);
    }
    /* dest = source: the count dword (n,0,0,0) zeroes word 0 of the source, the count byte is kept */
    {
        const uint16_t ww[2] = {0x1111u, 0x3344u};
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, ww, 2);
        t1479_0033ebf0_run(0x100u, src, src, src + 10u, src + 20u, 2u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(src) == 2u);
        CHECK(t1479_0033ebf0_r16(src + 4u) == 0u);
        CHECK(t1479_0033ebf0_r16(src + 12u) == 0x3344u);
        CHECK(memory[src + 6u] == 0x44u && memory[src + 7u] == 0x33u);
    }
    /* dest = source + 1: the count dword zeroes word 0, then the word-0 copy (zero) lands on the low
     * byte of word 1, so word 1 is copied as 0x3300 */
    {
        const uint16_t ww[2] = {0x1111u, 0x3344u};
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, ww, 2);
        t1479_0033ebf0_run(0x100u, src + 1u, src, src + 10u, src + 21u, 2u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(memory[src] == 2u && memory[src + 1u] == 2u);
        CHECK(t1479_0033ebf0_r16(src + 5u) == 0u);
        CHECK(memory[src + 6u] == 0u && memory[src + 7u] == 0x33u);
        CHECK(memory[src + 13u] == 0u && memory[src + 14u] == 0x33u);
    }

    /* alias (b), ESI slot hit by the dword count (dest = top-4), loop path */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        t1479_0033ebf0_run(top, top - 4u, src, src + 10u, top + 16u, 2u, 2u, t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top - 8u) == t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top - 4u) == 2u);
        CHECK(guest_read32(top) == 0x12341111u);          /* word 0 stored at dest+4 = top (low half) */
        CHECK(guest_read32(top + 4u) == top - 4u);
        CHECK(guest_read32(top + 8u) == (src & 0xFFFF0000u) + 0x2222u); /* word 1 at dest+12 */
        /* zero count: the pop still reads the overwritten ESI slot (0) */
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 0u, w, 2);
        t1479_0033ebf0_run(top, top - 4u, src, top - 4u, 0u, 0u, 0u, t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top - 4u) == 0u);
    }
    /* alias (b), EDI slot hit by a word store (dest = top-12): word 0 replaces the low half of the
     * saved EDI before the pop, word 1 lands on the return-address slot */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        t1479_0033ebf0_run(top, top - 12u, src, src + 10u, top + 8u, 2u, t1479_0033ebf0_esi_s,
                           0xB4B40000u | 0x1111u);
        CHECK(guest_read32(top - 12u) == 2u);
        CHECK(guest_read32(top - 8u) == (0xB4B40000u | 0x1111u));
        CHECK(guest_read32(top - 4u) == t1479_0033ebf0_esi_s);
        CHECK(guest_read32(top) == 0x12342222u);
    }
    /* alias (b), dest = top-8: the dword count lands on the EDI slot BEFORE the EDI push, so the push
     * overwrites it (slot = saved EDI); word 0 hits the low half of the ESI slot, word 1 hits arg0 */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        t1479_0033ebf0_run(top, top - 8u, src, src + 10u, top + 12u, 2u, 0xB3B30000u | 0x1111u,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top - 8u) == t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top - 4u) == (0xB3B30000u | 0x1111u));
        CHECK(guest_read32(top + 4u) == ((top - 8u) & 0xFFFF0000u) + 0x2222u);
        /* zero count with dest = top-8: the zero dword is stored there and nothing is pushed over it */
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 0u, w, 2);
        guest_write32(top - 8u, 0x0EDEDEDEu);
        t1479_0033ebf0_run(top, top - 8u, src, top - 8u, 0u, 0u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top - 8u) == 0u);
        CHECK(guest_read32(top - 4u) == t1479_0033ebf0_esi_s);
    }

    /* alias (c): source = the ESI slot (count byte = low byte of the saved ESI, words = its high half
     * and the high half of the return slot) */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_esi_s = 0xBEEF0002u;
        t1479_0033ebf0_run(top, dst, top - 4u, top + 6u, dst + 20u, 2u, 0xBEEF0002u,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(dst) == 2u);
        CHECK(t1479_0033ebf0_r16(dst + 4u) == 0xBEEFu);
        CHECK(t1479_0033ebf0_r16(dst + 12u) == 0x1234u);
        CHECK(guest_read32(top - 4u) == 0xBEEF0002u);
        /* the low byte zero: nothing to copy */
        t1479_0033ebf0_esi_s = 0xBEEF0000u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_esi_s = 0xBEEF0000u;
        t1479_0033ebf0_run(top, dst, top - 4u, dst, 0u, 0u, 0xBEEF0000u, t1479_0033ebf0_edi_s);
        CHECK(guest_read32(dst) == 0u);
    }
    /* alias (c): source = the EDI slot. The first count read sees the previous slot value (3), then
     * the EDI push stores 0xB4B4B402 there so every loop re-read sees 2 */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_edi_s = 0xB4B4B402u;
        guest_write32(top - 8u, 3u);
        t1479_0033ebf0_run(top, dst, top - 8u, top + 2u, dst + 20u, 2u, t1479_0033ebf0_esi_s,
                           0xB4B4B402u);
        CHECK(guest_read32(dst) == 3u);
        CHECK(t1479_0033ebf0_r16(dst + 4u) == 0xB4B4u);
        CHECK(t1479_0033ebf0_r16(dst + 12u) == 0xB3B3u);
        CHECK(guest_read32(top - 8u) == 0xB4B4B402u);
        /* the previous slot value 0: zero path, no push, the slot keeps 0 */
        t1479_0033ebf0_reset();
        t1479_0033ebf0_edi_s = 0xB4B4B402u;
        guest_write32(top - 8u, 0u);
        t1479_0033ebf0_run(top, dst, top - 8u, dst, 0u, 0u, t1479_0033ebf0_esi_s, 0xB4B4B402u);
        CHECK(guest_read32(dst) == 0u);
        CHECK(guest_read32(top - 8u) == 0u);
    }

    /* alias (a): dest on the return-address slot, the arg0 slot, the arg1 slot */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        t1479_0033ebf0_run(top, top, src, src + 10u, top + 20u, 2u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top) == 2u);
        CHECK(guest_read32(top + 4u) == ((top & 0xFFFF0000u) | 0x1111u));
        CHECK(guest_read32(top + 8u) == src);
        CHECK(t1479_0033ebf0_r16(top + 12u) == 0x2222u);
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        t1479_0033ebf0_run(top, top + 4u, src, src + 10u, top + 24u, 2u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top + 4u) == 2u);
        CHECK(guest_read32(top + 8u) == ((src & 0xFFFF0000u) | 0x1111u));
        CHECK(t1479_0033ebf0_r16(top + 16u) == 0x2222u);
        t1479_0033ebf0_reset();
        t1479_0033ebf0_src(src, 2u, w, 2);
        t1479_0033ebf0_run(top, top + 8u, src, src + 10u, top + 28u, 2u, t1479_0033ebf0_esi_s,
                           t1479_0033ebf0_edi_s);
        CHECK(guest_read32(top + 8u) == 2u);
        CHECK(t1479_0033ebf0_r16(top + 12u) == 0x1111u);
        CHECK(t1479_0033ebf0_r16(top + 20u) == 0x2222u);
    }
    /* alias (a): source = the return-address slot; count = its low byte 0x78 (120), words are read
     * from the stack (return high half, arg0 high half, arg1 high half) then zero memory */
    {
        const uint32_t top = 0x400u;
        t1479_0033ebf0_reset();
        t1479_0033ebf0_run(top, dst, top, top + 2u + 4u * 120u, dst + 4u + 8u * 120u, 120u,
                           t1479_0033ebf0_esi_s, t1479_0033ebf0_edi_s);
        CHECK(guest_read32(dst) == 0x78u);
        CHECK(t1479_0033ebf0_r16(dst + 4u) == 0x1234u);
        CHECK(t1479_0033ebf0_r16(dst + 12u) == (dst >> 16));
        CHECK(t1479_0033ebf0_r16(dst + 20u) == 0u);
        CHECK(t1479_0033ebf0_r16(dst + 4u + 8u * 119u) == 0u);
        CHECK(memory[dst + 4u + 8u * 120u] == 0xCC);
        CHECK(guest_read32(top - 4u) == t1479_0033ebf0_esi_s);
        CHECK(guest_read32(top - 8u) == t1479_0033ebf0_edi_s);
        memset(&memory[top], 0, 0x400);
    }
}
