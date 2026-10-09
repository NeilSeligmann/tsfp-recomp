/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x003562E0 (cdecl, 0 stack args). Expected values derived from the ORIGINAL listing:
 *   xor eax,eax ; mov ecx,0x7744C0 ; mov dl,1 ; lea esp,[esp] (nop) ;
 *   loop: test byte [ecx-0x30C],dl ; je +1 ; inc eax   (and the same for [ecx], [ecx+0x30C], [ecx+0x618])
 *         add ecx,0xC30 ; cmp ecx,0x777580 ; jl loop ; ret
 * ecx runs 0x7744C0, 0x7750F0, 0x775D20, 0x776950 (steps of 0xC30) and the loop exits after exactly four passes, so the 16
 * flag bytes are 0x7741B4 + 0x30C*i (i = 0..15) and only bit 0 of each counts. Exit: eax = number of set
 * bit-0s (0..16), ecx = 0x777580, edx = (entry edx & 0xFFFFFF00) | 1 (mov dl,1 keeps the upper 24 bits),
 * ebx/esi/edi/ebp untouched. Memory is only read.
 * No guest PUSH or POP in the original (the lea esp,[esp] is a no-op), so there is no saved-register slot
 * to alias (cases b, c do not exist). Alias (a): the function has no stack argument, so the stack is
 * placed inside the scanned table (the return slot IS flag byte 5, and a second run puts it on flag byte
 * 0), the return-address dword is read as a flag byte and nothing is written. The ecx compare is a
 * signed jl but ecx never leaves 0x7744C0..0x777580, so no signed/unsigned boundary is observable.
 * Garbage 0x01 is written around and between all flag bytes (bytes at +-1 and the 17th/0th-1 record) so
 * that a wrong stride, offset, pass count or bit shows up.
 * MUTANT: for (uint32_t index = 0u; index < T1479A_003562E0_RECORDS; index++) -> for (uint32_t index = 0u; index <= T1479A_003562E0_RECORDS; index++)
 * MUTANT: if ((flags & 1u) != 0u) { -> if ((flags & 3u) != 0u) {
 * MUTANT: g_edx = (g_edx & 0xFFFFFF00u) | 1u; -> g_edx = 1u;
 */

#define T1479_3562E0_FIRST 0x007741B4u
#define T1479_3562E0_STRIDE 0x30Cu
#define T1479_3562E0_LO 0x00773000u
#define T1479_3562E0_HI 0x00779000u

static uint8_t t1479_3562e0_snapshot[T1479_3562E0_HI - T1479_3562E0_LO];

static void t1479_3562e0_fill(void)
{
    memset(&memory[T1479_3562E0_LO], 0x01, T1479_3562E0_HI - T1479_3562E0_LO);
}

static uint32_t t1479_3562e0_flag(unsigned index)
{
    return T1479_3562E0_FIRST + T1479_3562E0_STRIDE * index;
}

/* mask bit i set -> flag byte i gets bit 0 set (random high bits), clear -> bit 0 clear (high bits set) */
static void t1479_3562e0_set(uint32_t mask, uint32_t high_bits)
{
    for (unsigned index = 0; index < 16; index++) {
        const uint32_t hi = high_bits & 0xFEu;
        memory[t1479_3562e0_flag(index)] = (uint8_t)(((mask >> index) & 1u) ? (hi | 1u) : hi);
    }
}

static void t1479_3562e0_run(uint32_t esp, uint32_t edx_in, unsigned expected_count)
{
    memcpy(t1479_3562e0_snapshot, &memory[T1479_3562E0_LO], sizeof t1479_3562e0_snapshot);
    prepare();
    g_esp = esp;
    guest_write32(esp, 0x12345678u);
    if (esp >= T1479_3562E0_LO && esp < T1479_3562E0_HI) {
        memcpy(t1479_3562e0_snapshot, &memory[T1479_3562E0_LO], sizeof t1479_3562e0_snapshot);
    }
    g_edx = edx_in;
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    invoke(0x003562E0u, 4u);
    CHECK(g_eax == expected_count);
    CHECK(g_ecx == 0x00777580u);
    CHECK(g_edx == ((edx_in & 0xFFFFFF00u) | 1u));
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(memcmp(t1479_3562e0_snapshot, &memory[T1479_3562E0_LO], sizeof t1479_3562e0_snapshot) == 0);
}

static void test_t1479_appendix_003562e0(void)
{
    t1479_3562e0_fill();
    /* all clear (with all other bits set), all set, with several high-bit fillers */
    static const uint32_t highs[] = {0x00u, 0xFEu, 0xAAu, 0x54u, 0x80u};
    for (unsigned h = 0; h < 5; h++) {
        t1479_3562e0_set(0x0000u, highs[h]);
        t1479_3562e0_run(0x100u, 0x33333333u, 0u);
        t1479_3562e0_set(0xFFFFu, highs[h]);
        t1479_3562e0_run(0x100u, 0x33333333u, 16u);
    }
    /* each single record, alone and against a field of high-bit-only bytes, every edx class */
    static const uint32_t edxs[] = {0x33333333u, 0u, 0xFFFFFFFFu, 0x80000000u, 0x12345600u, 0xAABBCCFEu};
    for (unsigned index = 0; index < 16; index++) {
        t1479_3562e0_set(1u << index, 0x00u);
        t1479_3562e0_run(0x100u, edxs[index % 6], 1u);
        t1479_3562e0_set(1u << index, 0xFEu);
        t1479_3562e0_run(0x100u, edxs[(index + 1) % 6], 1u);
        t1479_3562e0_set(0xFFFFu ^ (1u << index), 0xFEu);
        t1479_3562e0_run(0x100u, edxs[(index + 2) % 6], 15u);
    }
    /* named patterns with hand-counted results: checkerboards, first and last record only, middle block */
    t1479_3562e0_set(0x5555u, 0xAAu);
    t1479_3562e0_run(0x100u, 0x33333333u, 8u);
    t1479_3562e0_set(0xAAAAu, 0x54u);
    t1479_3562e0_run(0x100u, 0x33333333u, 8u);
    t1479_3562e0_set(0x8001u, 0xFEu);
    t1479_3562e0_run(0x100u, 0x33333333u, 2u);
    t1479_3562e0_set(0x0FF0u, 0xFEu);
    t1479_3562e0_run(0x100u, 0x33333333u, 8u);
    t1479_3562e0_set(0x00FFu, 0x00u);
    t1479_3562e0_run(0x100u, 0x33333333u, 8u);
    t1479_3562e0_set(0xFF00u, 0x00u);
    t1479_3562e0_run(0x100u, 0x33333333u, 8u);
    /* exhaustive: all 65536 flag masks, count equals the popcount */
    for (uint32_t mask = 0; mask < 0x10000u; mask++) {
        unsigned bits = 0;
        for (unsigned index = 0; index < 16; index++) bits += (mask >> index) & 1u;
        t1479_3562e0_set(mask, (mask * 0x9Du) & 0xFEu);
        t1479_3562e0_run(0x100u, 0xA5000000u | (mask & 0xFFu), bits);
    }
    /* stack inside the scanned table: the return slot is flag byte 5, then flag byte 0 and byte 15 */
    static const unsigned slots[] = {5u, 0u, 15u};
    for (unsigned s = 0; s < 3; s++) {
        t1479_3562e0_fill();
        t1479_3562e0_set(0x0000u, 0xFEu);
        g_esp = 0;
        prepare();
        g_esp = t1479_3562e0_flag(slots[s]);
        guest_write32(g_esp, 0x01010101u); /* the return address reads as flag byte 0x01 */
        memcpy(t1479_3562e0_snapshot, &memory[T1479_3562E0_LO], sizeof t1479_3562e0_snapshot);
        g_edx = 0x33333333u;
        invoke(0x003562E0u, 4u);
        CHECK(g_eax == 1u && g_ecx == 0x00777580u && g_edx == 0x33333301u);
        CHECK(memcmp(t1479_3562e0_snapshot, &memory[T1479_3562E0_LO], sizeof t1479_3562e0_snapshot) == 0);
        CHECK(g_esp == t1479_3562e0_flag(slots[s]) + 4u);
        /* the same slot holding an even return word (bit 0 clear) adds nothing */
        t1479_3562e0_fill();
        t1479_3562e0_set(0x0000u, 0xFEu);
        prepare();
        g_esp = t1479_3562e0_flag(slots[s]);
        guest_write32(g_esp, 0xFEFEFEFEu);
        g_edx = 0x33333333u;
        invoke(0x003562E0u, 4u);
        CHECK(g_eax == 0u && g_ecx == 0x00777580u && g_edx == 0x33333301u);
    }
}
