/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x001B62D0, derived from the original instruction listing:
 *   eax=[esp+4]; ecx=[eax+0x34]; edx=[eax+0xF8]; xmm0=movss [esp+8]; ecx|=8; [eax+0x34]=ecx;
 *   ecx=[eax+0xE8]; cmp ecx,edx; [eax+0xF0]=xmm0 low; if equal [eax+0xF8]=0.
 * Exit: eax=object, ecx=[obj+0xE8], edx=old [obj+0xF8] (never the cleared value),
 * xmm0={float,0,0,0} (the draft only owns the g_xmm0 slot, so other XMM registers are not checked). */
extern __thread uint32_t t1478a_001b62d0_xmm0[4] __asm__("g_xmm0");

static void t1478a_001b62d0_run(uint32_t object, uint32_t float_bits)
{
    prepare();
    guest_write32(g_esp + 4u, object);
    guest_write32(g_esp + 8u, float_bits);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) t1478a_001b62d0_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x001B62D0u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_eax == object);
    CHECK(t1478a_001b62d0_xmm0[0] == float_bits && t1478a_001b62d0_xmm0[1] == 0u);
    CHECK(t1478a_001b62d0_xmm0[2] == 0u && t1478a_001b62d0_xmm0[3] == 0u);
}

static void test_t1478_appendix_001b62d0(void)
{
    const uint32_t objects[] = {0x20000u, 0x2FFF4u};
    const uint32_t flag_values[] = {0u, 8u, 0x7u, 0x80000000u, 0xFFFFFFFFu, 0x12345670u};
    const uint32_t floats[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00001u, 0xFFFFFFFFu};
    /* (e8, f8) pairs: equal, differ by one bit, differ by sign bit, both zero, both all-ones */
    const uint32_t pairs[][2] = {{5u, 5u}, {5u, 4u}, {0x80000000u, 0u}, {0u, 0u},
                                 {0xFFFFFFFFu, 0xFFFFFFFFu}, {0u, 0x80000000u}, {1u, 0x10000u}};
    for (unsigned o = 0; o < 2; ++o) {
        for (unsigned f = 0; f < 6; ++f) {
            for (unsigned x = 0; x < 5; ++x) {
                for (unsigned p = 0; p < 7; ++p) {
                    const uint32_t object = objects[o];
                    for (uint32_t offset = 0x30; offset < 0x100; offset += 4u)
                        guest_write32(object + offset, 0xC0C00000u + offset);
                    guest_write32(object + 0x34u, flag_values[f]);
                    guest_write32(object + 0xE8u, pairs[p][0]);
                    guest_write32(object + 0xF8u, pairs[p][1]);
                    t1478a_001b62d0_run(object, floats[x]);
                    const int equal = pairs[p][0] == pairs[p][1];
                    CHECK(g_ecx == pairs[p][0] && g_edx == pairs[p][1]);
                    CHECK(guest_read32(object + 0x34u) == (flag_values[f] | 8u));
                    CHECK(guest_read32(object + 0xE8u) == pairs[p][0]);
                    CHECK(guest_read32(object + 0xF0u) == floats[x]);
                    CHECK(guest_read32(object + 0xF8u) == (equal ? 0u : pairs[p][1]));
                    /* every other dword of the scanned window is untouched */
                    for (uint32_t offset = 0x30; offset < 0x100; offset += 4u) {
                        if (offset == 0x34u || offset == 0xE8u || offset == 0xF0u || offset == 0xF8u)
                            continue;
                        CHECK(guest_read32(object + offset) == 0xC0C00000u + offset);
                    }
                    CHECK(guest_read32(0x100u) == 0x12345678u);
                }
            }
        }
    }
    /* Flag dword aliasing the float argument slot (esp+8 = 0x108): the float was read before the
     * flag store, so +0xF0 receives the original float bits, while ecx came from the slot. */
    prepare();
    guest_write32(0xD4u + 0xE8u, 7u);
    guest_write32(0xD4u + 0xF8u, 7u);
    guest_write32(0xD4u + 0xF0u, 0xEEEEEEEEu);
    t1478a_001b62d0_run(0xD4u, 0x40490FD0u);
    CHECK(guest_read32(0x108u) == (0x40490FD0u | 8u));
    CHECK(guest_read32(0xD4u + 0xF0u) == 0x40490FD0u);
    CHECK(g_ecx == 7u && g_edx == 7u && guest_read32(0xD4u + 0xF8u) == 0u);
    /* +0xE8 read happens after the flag store: an object whose +0x34 is the dword that +0xE8 of
     * ANOTHER layout would read is impossible, so use the stack: object 0x1C puts +0xE8 on the
     * object-argument slot (esp+4 = 0x104) and +0xF8 on 0x114. ecx = the object pointer. */
    guest_write32(0x1Cu + 0xF8u, 0x1Cu);
    guest_write32(0x1Cu + 0x34u, 0x3u);
    t1478a_001b62d0_run(0x1Cu, 0x3F000000u);
    CHECK(g_ecx == 0x1Cu && g_edx == 0x1Cu && guest_read32(0x1Cu + 0xF8u) == 0u);
    CHECK(guest_read32(0x1Cu + 0x34u) == 0xBu && guest_read32(0x1Cu + 0xF0u) == 0x3F000000u);
    /* Address arithmetic wraps at 32 bits: object 0xFFFFFFCC puts +0x34 at 0, +0xE8 at 0xB4,
     * +0xF0 at 0xBC and +0xF8 at 0xC4 (equal values, so the clear store happens). */
    guest_write32(0x0u, 0x100u);
    guest_write32(0xB4u, 0x4242u);
    guest_write32(0xC4u, 0x4242u);
    t1478a_001b62d0_run(0xFFFFFFCCu, 0x11223344u);
    CHECK(guest_read32(0x0u) == 0x108u && guest_read32(0xBCu) == 0x11223344u);
    CHECK(guest_read32(0xC4u) == 0u && g_ecx == 0x4242u && g_edx == 0x4242u);
}
