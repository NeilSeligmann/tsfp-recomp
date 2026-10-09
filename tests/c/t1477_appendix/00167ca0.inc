/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent native control for 0x00167CA0, derived from the original listing only:
 *   eax=[esp+4]; ecx=[0x74C458]; eax&=0xFFFF; eax<<=9; eax+=ecx; ecx=[esp+8];
 *   edx=[ecx];   [eax+0x140]=edx;
 *   edx=[ecx+4]; [eax+0x144]=edx;
 *   ecx=[ecx+8]; [eax+0x148]=ecx;   ret
 * Exit (single path): EAX = entry = ((id & 0xFFFF) << 9) + base (NOT +0x140), ECX = the dword
 * [src+8] as read AFTER the stores to +0x140 and +0x144, EDX = [src+4] read after the first
 * store, EBX/ESI/EDI/EBP untouched, ESP + 4 (plain cdecl ret). Memory: exactly the three stores,
 * in order. There is NO PUSH or POP in the original, so saved-register slot aliasing does not
 * exist here. Alias controls: source overlapping the destination (read/store interleaving),
 * destination on the return slot and on both argument slots, the source pointer ON the stack
 * (reading the return slot, id slot and the pointer slot itself), and the base global.
 * The generic cases compute the expectation with a byte-exact shadow image in the listing
 * order. The named overlap cases are EXPECTED VALUES OBSERVED from executing the ORIGINAL
 * bytes in Unicorn 2.1.4 on the named entry states (original-only, never the draft; script
 * tmp/c96/t167ca0.py and t167ca0b.py, untracked). */

#define C00167ca0_BASEVA 0x0074C458u

static uint8_t c00167ca0_shadow[sizeof memory];

static uint32_t c00167ca0_rd(uint32_t address)
{
    return (uint32_t)c00167ca0_shadow[address] | ((uint32_t)c00167ca0_shadow[address + 1u] << 8) |
           ((uint32_t)c00167ca0_shadow[address + 2u] << 16) |
           ((uint32_t)c00167ca0_shadow[address + 3u] << 24);
}

static void c00167ca0_wr(uint32_t address, uint32_t value)
{
    for (unsigned b = 0; b < 4u; b++) {
        c00167ca0_shadow[address + b] = (uint8_t)(value >> (8u * b));
    }
}

/* The caller presets the source data. The helper writes the return slot, id slot and pointer slot
 * (so a source that overlaps them sees them), then the base global, then models the original. */
static void c00167ca0_run(uint32_t esp, uint32_t id, uint32_t src, uint32_t base)
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
    guest_write32(g_esp + 8u, src);
    guest_write32(C00167ca0_BASEVA, base);
    memcpy(c00167ca0_shadow, memory, sizeof memory);
    /* the listing, step by step, on the shadow image */
    uint32_t model_eax = c00167ca0_rd(esp + 4u);
    uint32_t model_ecx = c00167ca0_rd(C00167ca0_BASEVA);
    model_eax = ((model_eax & 0xFFFFu) << 9) + model_ecx;
    model_ecx = c00167ca0_rd(esp + 8u);
    uint32_t model_edx = c00167ca0_rd(model_ecx);
    c00167ca0_wr(model_eax + 0x140u, model_edx);
    model_edx = c00167ca0_rd(model_ecx + 4u);
    c00167ca0_wr(model_eax + 0x144u, model_edx);
    model_ecx = c00167ca0_rd(model_ecx + 8u);
    c00167ca0_wr(model_eax + 0x148u, model_ecx);
    invoke(0x00167CA0u, 4u);
    CHECK(g_eax == model_eax);
    CHECK(g_ecx == model_ecx);
    CHECK(g_edx == model_edx);
    CHECK(g_ebx == 0xB4B4B4B4u && g_esi == 0x54545454u);
    CHECK(g_edi == 0xD4D4D4D4u && g_ebp == 0xBDBDBDBDu);
    CHECK(g_esp == esp + 4u && g_df == 0);
    CHECK(memcmp(c00167ca0_shadow, memory, sizeof memory) == 0);
}

/* Named state: entry E for id 3, base derived, source at src with 5 dwords vals[]. */
static void c00167ca0_named(uint32_t esp, uint32_t id, uint32_t src, const uint32_t *vals,
                            unsigned count, uint32_t entry, uint32_t expected_ecx,
                            uint32_t expected_edx)
{
    for (unsigned i = 0; i < count; i++) {
        guest_write32(src + 4u * i, vals[i]);
    }
    c00167ca0_run(esp, id, src, entry - ((id & 0xFFFFu) << 9));
    CHECK(g_eax == entry);
    CHECK(g_ecx == expected_ecx);
    CHECK(g_edx == expected_edx);
}

static void test_t1477_appendix_00167ca0(void)
{
    /* generic: ids, source values, entries; masks, scale and base all vary */
    const uint32_t ids[] = {0u, 1u, 0x7Fu, 0x8000u, 0xFFFFu, 0x10000u, 0x12345u, 0xFFFF0003u};
    const uint32_t patterns[][3] = {{0u, 0u, 0u}, {1u, 2u, 3u}, {0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu},
                                    {0x3F800000u, 0xC0000000u, 0x00000001u}};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (unsigned j = 0; j < 4u; j++) {
            const uint32_t src = 0x20000u + 0x40u * (i + 8u * j);
            const uint32_t entry = 0x300000u + 0x400u * (i + 8u * j);
            for (unsigned k = 0; k < 3u; k++) {
                guest_write32(src + 4u * k, patterns[j][k]);
                guest_write32(entry + 0x140u + 4u * k, 0xE0E0E0E0u + k); /* destination preset */
            }
            guest_write32(entry + 0x13Cu, 0xEFEFEFEFu); /* neighbours of the vec3 */
            guest_write32(entry + 0x14Cu, 0xEEEEEEEEu);
            c00167ca0_run(0x100u, ids[i], src, entry - ((ids[i] & 0xFFFFu) << 9));
            CHECK(g_eax == entry);
            CHECK(g_ecx == patterns[j][2] && g_edx == patterns[j][1]);
            for (unsigned k = 0; k < 3u; k++) {
                CHECK(guest_read32(entry + 0x140u + 4u * k) == patterns[j][k]);
                CHECK(guest_read32(src + 4u * k) == patterns[j][k]); /* source unchanged */
            }
            CHECK(guest_read32(entry + 0x13Cu) == 0xEFEFEFEFu);
            CHECK(guest_read32(entry + 0x14Cu) == 0xEEEEEEEEu);
        }
    }
    /* 32-bit wrap of the base: entry address wraps to 0x3000 for id 0xFFFF */
    guest_write32(0x20000u, 7u);
    guest_write32(0x20004u, 8u);
    guest_write32(0x20008u, 9u);
    c00167ca0_run(0x100u, 0xFFFFu, 0x20000u, 0x3000u - (0xFFFFu << 9));
    CHECK(g_eax == 0x3000u && guest_read32(0x3140u) == 7u && guest_read32(0x3144u) == 8u &&
          guest_read32(0x3148u) == 9u);
    /* neither the entry itself nor the bytes below +0x140 are touched */
    CHECK(guest_read32(0x3000u) == 0u && guest_read32(0x313Cu) == 0u);

    /* (a) source/destination overlap, named states from the ORIGINAL (id 3, entry 0x300600,
     * destination vec3 at 0x300740..0x300748). */
    {
        const uint32_t v5[5] = {0xA1u, 0xB2u, 0xC3u, 0xD4u, 0xE5u};
        const uint32_t entry = 0x300600u;
        /* src == dst */
        c00167ca0_named(0x100u, 3u, 0x300740u, v5, 3u, entry, 0xC3u, 0xB2u);
        CHECK(guest_read32(0x300740u) == 0xA1u && guest_read32(0x300744u) == 0xB2u &&
              guest_read32(0x300748u) == 0xC3u);
        /* src == dst + 4: src[0] is dst[1], the second store overwrites it after it was read */
        c00167ca0_named(0x100u, 3u, 0x300744u, v5, 4u, entry, 0xC3u, 0xB2u);
        CHECK(guest_read32(0x300740u) == 0xA1u && guest_read32(0x300744u) == 0xB2u &&
              guest_read32(0x300748u) == 0xC3u);
        /* src == dst - 4: the first store overwrites src[1], so the second read sees A1 */
        c00167ca0_named(0x100u, 3u, 0x30073Cu, v5, 4u, entry, 0xA1u, 0xA1u);
        CHECK(guest_read32(0x300740u) == 0xA1u && guest_read32(0x300744u) == 0xA1u &&
              guest_read32(0x300748u) == 0xA1u);
        /* src == dst - 8: src[2] is overwritten by the first store, ECX reads A1 not C3 */
        c00167ca0_named(0x100u, 3u, 0x300738u, v5, 5u, entry, 0xA1u, 0xB2u);
        CHECK(guest_read32(0x300740u) == 0xA1u && guest_read32(0x300744u) == 0xB2u &&
              guest_read32(0x300748u) == 0xA1u);
        /* src == dst + 8: the second store overwrites nothing read later except src[0] */
        c00167ca0_named(0x100u, 3u, 0x300748u, v5, 5u, entry, 0xC3u, 0xB2u);
        CHECK(guest_read32(0x300740u) == 0xA1u && guest_read32(0x300744u) == 0xB2u &&
              guest_read32(0x300748u) == 0xC3u);
        /* src == dst + 12: disjoint */
        c00167ca0_named(0x100u, 3u, 0x30074Cu, v5, 5u, entry, 0xC3u, 0xB2u);
    }
    /* (a) destination on the stack slots, ORIGINAL observed: id 3, source 0x20000 {A1,B2,C3}, esp 0x2000 */
    {
        const uint32_t v3[3] = {0xA1u, 0xB2u, 0xC3u};
        /* dst0 == return slot: ret=A1, id slot=B2, pointer slot=C3, wrapper still pops 4 */
        c00167ca0_named(0x2000u, 3u, 0x20000u, v3, 3u, 0x2000u - 0x140u, 0xC3u, 0xB2u);
        CHECK(guest_read32(0x2000u) == 0xA1u && guest_read32(0x2004u) == 0xB2u &&
              guest_read32(0x2008u) == 0xC3u && guest_read32(0x200Cu) == 0u);
        /* dst0 == id slot: id slot=A1, pointer slot=B2, esp+12=C3, return slot kept */
        c00167ca0_named(0x2000u, 3u, 0x20000u, v3, 3u, 0x2004u - 0x140u, 0xC3u, 0xB2u);
        CHECK(guest_read32(0x2000u) == 0x12345678u && guest_read32(0x2004u) == 0xA1u &&
              guest_read32(0x2008u) == 0xB2u && guest_read32(0x200Cu) == 0xC3u);
        /* dst0 == pointer slot: the pointer was already read, id slot keeps 3 */
        c00167ca0_named(0x2000u, 3u, 0x20000u, v3, 3u, 0x2008u - 0x140u, 0xC3u, 0xB2u);
        CHECK(guest_read32(0x2000u) == 0x12345678u && guest_read32(0x2004u) == 3u &&
              guest_read32(0x2008u) == 0xA1u && guest_read32(0x200Cu) == 0xB2u &&
              guest_read32(0x2010u) == 0xC3u);
    }
    /* (a) source pointer ON the stack, ORIGINAL observed, esp 0x2000: src == esp reads
     * {return slot 0x12345678, id 3, pointer 0x2000}. */
    {
        c00167ca0_named(0x2000u, 3u, 0x2000u, NULL, 0u, 0x300600u, 0x2000u, 3u);
        CHECK(guest_read32(0x300740u) == 0x12345678u && guest_read32(0x300744u) == 3u &&
              guest_read32(0x300748u) == 0x2000u);
        /* src == esp and dst0 == id slot: every later read sees the returned-slot value
         * copied by the previous store, ECX and EDX both end up 0x12345678 */
        c00167ca0_named(0x2000u, 3u, 0x2000u, NULL, 0u, 0x2004u - 0x140u, 0x12345678u,
                        0x12345678u);
        CHECK(guest_read32(0x2000u) == 0x12345678u && guest_read32(0x2004u) == 0x12345678u &&
              guest_read32(0x2008u) == 0x12345678u && guest_read32(0x200Cu) == 0x12345678u);
        /* src == esp + 4 and dst0 == return slot: ECX is the dword after the pointer slot */
        {
            guest_write32(0x200Cu, 0xE5u);
            c00167ca0_named(0x2000u, 3u, 0x2004u, NULL, 0u, 0x2000u - 0x140u, 0xE5u, 0x2004u);
            CHECK(guest_read32(0x2000u) == 3u && guest_read32(0x2004u) == 0x2004u &&
                  guest_read32(0x2008u) == 0xE5u && guest_read32(0x200Cu) == 0xE5u);
        }
    }
    /* (a) the vec3 destination covers the base global: the base is read before the stores */
    {
        const uint32_t vb[3] = {0x111u, 0x222u, 0x333u};
        c00167ca0_named(0x100u, 0u, 0x20100u, vb, 3u, C00167ca0_BASEVA - 0x140u, 0x333u, 0x222u);
        CHECK(guest_read32(C00167ca0_BASEVA) == 0x111u);
        c00167ca0_named(0x100u, 0u, 0x20100u, vb, 3u, C00167ca0_BASEVA - 0x148u, 0x333u, 0x222u);
        CHECK(guest_read32(C00167ca0_BASEVA) == 0x333u);
    }
    /* stack ON the base global: the return slot is the base; id/pointer slots follow it */
    guest_write32(0x20200u, 5u);
    guest_write32(0x20204u, 6u);
    guest_write32(0x20208u, 7u);
    c00167ca0_run(C00167ca0_BASEVA, 0u, 0x20200u, 0x300000u);
    c00167ca0_run(C00167ca0_BASEVA - 4u, 0x80u, 0x20200u, 0x400000u);
}
