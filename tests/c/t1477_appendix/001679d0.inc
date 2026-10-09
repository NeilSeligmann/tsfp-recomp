/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x001679D0, derived from the original listing only:
 *   mov eax,[esp+4]; mov ecx,[0x74C458]; and eax,0xFFFF; shl eax,9; lea eax,[eax+ecx+0x140]; ret
 * Exit (single path): EAX = ((id & 0xFFFF) << 9) + base + 0x140 (mod 2^32), ECX = base
 * (the dword at 0x74C458), EDX/EBX/ESI/EDI/EBP untouched, ESP + 4 (plain cdecl ret).
 * The original performs NO guest store, so the whole memory image must be unchanged.
 * There is NO PUSH or POP in the original, so saved-register slot aliasing does not exist
 * here. The alias controls put the stack ON the base global: the return slot (esp = base
 * address) and the argument slot (esp + 4 = base address, so the id and the base are the
 * same dword). No store exists, so the read order is unobservable, only the values are. */

#define C001679d0_BASEVA 0x0074C458u

static uint8_t c001679d0_before[sizeof memory];

static void c001679d0_run(uint32_t esp, uint32_t id, uint32_t base, uint32_t expected_eax,
                          uint32_t expected_base)
{
    prepare();
    g_esp = esp;
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = 0;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(C001679d0_BASEVA, base);
    guest_write32(g_esp + 4u, id); /* last: with esp + 4 == base address the id wins */
    memcpy(c001679d0_before, memory, sizeof memory);
    invoke(0x001679D0u, 4u);
    CHECK(g_eax == expected_eax);
    CHECK(g_ecx == expected_base);
    CHECK(g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB4B4B4B4u && g_esi == 0x54545454u);
    CHECK(g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
    CHECK(g_esp == esp + 4u && g_df == 0);
    CHECK(memcmp(c001679d0_before, memory, sizeof memory) == 0); /* no stores at all */
}

static void test_t1477_appendix_001679d0(void)
{
    const uint32_t ids[] = {0u, 1u, 2u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0x7FFFu, 0x8000u, 0xFFFFu,
                            0x10000u, 0x10001u, 0x12345u, 0xFFFF0000u, 0xFFFF0007u, 0xFFFFFFFFu,
                            0x80000000u, 0x8000FFFFu};
    const uint32_t bases[] = {0u, 0x300000u, 0x74C458u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu,
                              0xFFFFFEC0u, 0xFFFFFEBFu, 0xFFFFFEC1u, 0xFE000200u};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (unsigned j = 0; j < sizeof bases / sizeof bases[0]; j++) {
            /* expectation written from the listing: mask to 16 bits, scale by 512, add base
             * and 0x140, everything mod 2^32 */
            uint32_t offset = (ids[i] & 0xFFFFu) * 512u;
            c001679d0_run(0x100u, ids[i], bases[j], offset + bases[j] + 0x140u, bases[j]);
        }
    }
    /* fixed numbers computed by hand: id 3 -> offset 0x600, base 0x300000 -> 0x300740 */
    c001679d0_run(0x100u, 3u, 0x300000u, 0x300740u, 0x300000u);
    /* the id masks to 0 -> only base + 0x140 */
    c001679d0_run(0x100u, 0xABCD0000u, 0x1000u, 0x1140u, 0x1000u);
    /* max id 0xFFFF -> offset 0x1FFFE00 */
    c001679d0_run(0x100u, 0xFFFFu, 0x100u, 0x100u + 0x1FFFE00u + 0x140u, 0x100u);
    /* 32-bit wrap: 0x1FFFE00 + 0xFE000200 + 0x140 == 0x140 */
    c001679d0_run(0x100u, 0xFFFFu, 0xFE000200u, 0x140u, 0xFE000200u);

    /* (a) return-slot alias: the stack sits on the base global, esp = 0x74C458, so the return
     * slot is the base. prepare() wrote the return value there, the helper rewrites the base
     * after it, so the base wins. No store follows, so EAX/ECX use the base value. */
    c001679d0_run(C001679d0_BASEVA, 5u, 0x00200000u, 5u * 512u + 0x00200000u + 0x140u, 0x00200000u);
    /* (a) argument-slot alias: esp + 4 = base address, so id and base are one dword. The helper
     * writes the id last, so base == id == v and EAX = ((v & 0xFFFF) << 9) + v + 0x140. */
    {
        const uint32_t v = 0x00123456u;
        c001679d0_run(C001679d0_BASEVA - 4u, v, 0xDEADBEEFu, ((v & 0xFFFFu) << 9) + v + 0x140u, v);
        const uint32_t w = 0xFFFF8003u;
        c001679d0_run(C001679d0_BASEVA - 4u, w, 0u, ((w & 0xFFFFu) << 9) + w + 0x140u, w);
    }
    /* the neighbours of the base global do not matter, flipping them changes nothing */
    guest_write32(C001679d0_BASEVA - 4u, 0xFFFFFFFFu);
    guest_write32(C001679d0_BASEVA + 4u, 0xFFFFFFFFu);
    c001679d0_run(0x100u, 4u, 0x400u, 4u * 512u + 0x400u + 0x140u, 0x400u);
}
