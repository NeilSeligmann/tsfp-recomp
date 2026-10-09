/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x00168160, derived from the original listing only:
 *   mov eax,[esp+4]; mov ecx,[esp+8]; mov edx,[0x74C458]; and eax,0xFFFF; shl eax,9;
 *   mov [eax+edx+0x1C4],ecx; ret
 * Exit (single path): EAX = (id & 0xFFFF) << 9 (the OFFSET only, not an address), ECX = value
 * argument, EDX = base (the dword at 0x74C458), EBX/ESI/EDI/EBP untouched, ESP + 4.
 * Memory: exactly one dword store of ECX at offset + base + 0x1C4 (mod 2^32), performed AFTER all
 * three reads, so a store landing on the return slot, either argument slot or the base global
 * cannot change any exit register. There is NO PUSH or POP in the original, so saved-register
 * slot aliasing does not exist here: the alias controls cover the return slot, both argument
 * slots, the base global, a partially overlapping store and the 32-bit address wrap. */

#define C00168160_BASEVA 0x0074C458u

static uint8_t c00168160_before[sizeof memory];

/* Run with the stack at esp. The id argument slot is written first, then the value argument
 * slot, then the base global, then the pre-store pattern at the target (so every alias keeps
 * the LAST written value, the same order is mirrored in the expected image below). */
static void c00168160_run(uint32_t esp, uint32_t id, uint32_t value, uint32_t base)
{
    prepare();
    g_esp = esp;
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = 0;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, id);
    guest_write32(g_esp + 8u, value);
    guest_write32(C00168160_BASEVA, base);
    /* what the original sees at entry */
    const uint32_t seen_id = guest_read32(g_esp + 4u);
    const uint32_t seen_value = guest_read32(g_esp + 8u);
    const uint32_t seen_base = guest_read32(C00168160_BASEVA);
    memcpy(c00168160_before, memory, sizeof memory);
    invoke(0x00168160u, 4u);
    const uint32_t offset = (seen_id & 0xFFFFu) << 9;
    const uint32_t target = offset + seen_base + 0x1C4u;
    CHECK(g_eax == offset);
    CHECK(g_ecx == seen_value);
    CHECK(g_edx == seen_base);
    CHECK(g_ebx == 0xB4B4B4B4u && g_esi == 0x54545454u);
    CHECK(g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
    CHECK(g_esp == esp + 4u && g_df == 0);
    /* expected image: the before image with the four little-endian bytes of the value at target */
    for (unsigned b = 0; b < 4u; b++) {
        c00168160_before[target + b] = (uint8_t)(seen_value >> (8u * b));
    }
    CHECK(memcmp(c00168160_before, memory, sizeof memory) == 0);
    CHECK(guest_read32(target) == seen_value);
}

static void test_t1477_appendix_00168160(void)
{
    /* the base is chosen so the target lands at a given aligned address T for each id */
    const uint32_t ids[] = {0u, 1u, 2u, 0x80u, 0x7FFFu, 0x8000u, 0xFFFFu, 0x10000u, 0x12345u,
                            0xFFFF0003u, 0x80000001u};
    const uint32_t values[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu, 0x3F800000u, 0xCAFEF00Du};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (unsigned j = 0; j < sizeof values / sizeof values[0]; j++) {
            const uint32_t target = 0x00300000u + 8u * (i + 3u * j);
            const uint32_t base = target - 0x1C4u - ((ids[i] & 0xFFFFu) << 9);
            /* neighbours of the target get a pattern so a wrong width or offset shows up */
            guest_write32(target - 4u, 0xA0A0A0A0u);
            guest_write32(target, 0xB0B0B0B0u);
            guest_write32(target + 4u, 0xC0C0C0C0u);
            c00168160_run(0x100u, ids[i], values[j], base);
            CHECK(guest_read32(target - 4u) == 0xA0A0A0A0u);
            CHECK(guest_read32(target + 4u) == 0xC0C0C0C0u);
        }
    }
    /* hand numbers: id 3, base 0x300000 -> eax 0x600, target 0x3007C4 gets 0x11223344 */
    c00168160_run(0x100u, 3u, 0x11223344u, 0x300000u);
    CHECK(g_eax == 0x600u && g_edx == 0x300000u && guest_read32(0x3007C4u) == 0x11223344u);
    /* id masked to zero: offset 0, target = base + 0x1C4 */
    c00168160_run(0x100u, 0x7B310000u, 0x55AA55AAu, 0x310000u);
    CHECK(g_eax == 0u && guest_read32(0x3101C4u) == 0x55AA55AAu);
    /* unaligned base: the store is an unaligned dword write */
    c00168160_run(0x100u, 1u, 0x01020304u, 0x300001u);
    CHECK(g_eax == 0x200u && guest_read32(0x300001u + 0x200u + 0x1C4u) == 0x01020304u);
    /* 32-bit wrap of the target address: base 0xFFFFFE40 + 0x1C4 wraps to 0x4 */
    c00168160_run(0x100u, 0u, 0xE1E2E3E4u, 0xFFFFFE40u);
    CHECK(g_eax == 0u && g_edx == 0xFFFFFE40u && guest_read32(4u) == 0xE1E2E3E4u);

    /* (a) return-slot alias: target == esp, the store replaces the return address and the
     * wrapper still pops 4. The id (0) is read before the store, registers are unaffected. */
    c00168160_run(0x2000u, 0u, 0x0BADF00Du, 0x2000u - 0x1C4u);
    CHECK(guest_read32(0x2000u) == 0x0BADF00Du && guest_read32(0x2004u) == 0u);
    /* (a) argument-slot alias, id slot: the stored value replaces the id AFTER it was read */
    c00168160_run(0x2000u, 0x0005u, 0x7777u, 0x2004u - 0x1C4u - (5u << 9));
    CHECK(g_eax == (5u << 9) && g_ecx == 0x7777u);
    CHECK(guest_read32(0x2004u) == 0x7777u && guest_read32(0x2008u) == 0x7777u);
    /* (a) argument-slot alias, value slot: the value is stored over itself */
    c00168160_run(0x2000u, 2u, 0x13572468u, 0x2008u - 0x1C4u - (2u << 9));
    CHECK(g_ecx == 0x13572468u && guest_read32(0x2008u) == 0x13572468u);
    CHECK(guest_read32(0x2004u) == 2u);
    /* partial overlaps: the dword store straddles the return slot / id slot boundary */
    c00168160_run(0x2000u, 0u, 0x0A0B0C0Du, 0x2002u - 0x1C4u);
    CHECK(guest_read32(0x2000u) == 0x0C0D5678u && guest_read32(0x2004u) == 0x00000A0Bu);
    c00168160_run(0x2000u, 0x0u, 0x0A0B0C0Du, 0x1FFEu - 0x1C4u);
    CHECK(guest_read32(0x1FFCu) == 0x0C0D0000u && guest_read32(0x2000u) == 0x12340A0Bu);
    /* (a) the store targets the base global itself: EDX keeps the OLD base, memory the value */
    c00168160_run(0x100u, 0u, 0x600D600Du, C00168160_BASEVA - 0x1C4u);
    CHECK(g_edx == C00168160_BASEVA - 0x1C4u && guest_read32(C00168160_BASEVA) == 0x600D600Du);
    /* stack on the base global: return slot == base, then the argument slots follow it */
    c00168160_run(C00168160_BASEVA, 1u, 0x99u, 0x300000u);
    c00168160_run(C00168160_BASEVA - 4u, 0x00000009u, 0x77u, 0x00000001u);
}
