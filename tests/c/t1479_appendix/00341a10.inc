/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00341A10 (set state 3 and store a float, legacy vector mode). Expected values
 * were derived by hand from the original instruction listing, not from the draft:
 *   eax = [esp+4]; eax == 0 -> ret (ecx, edx, xmm0 untouched); edx = [eax+4]; ecx = 3;
 *   edx != 3 -> xorps xmm0,xmm0; movss [eax+8], xmm0 (zero dword);
 *   movss xmm0, [esp+8] (loaded AFTER that store; upper lanes zeroed); [eax+4] = ecx; [eax+0xC] = xmm0 low.
 * Exit: eax = object, ecx = 3, edx = old [object+4], xmm0 = {value,0,0,0}; ebx/ebp/esi/edi untouched.
 * PUSH/POP: the original has no push and no pop, so the saved-register aliases (b) and (c) do not
 * exist. Alias (a) is covered: the object is derived from the stack so that its +4/+8/+0xC dwords are
 * the return-address and argument slots, including the case that the zero store hits the value
 * argument slot before the movss load.
 * MUTANT: if (g_edx != g_ecx) { -> if (g_edx == g_ecx) {
 * MUTANT: g_xmm0.u[1] = 0u; -> g_xmm0.u[1] = 0x11u;
 * MUTANT: guest_write32(g_eax + 8u, g_xmm0.u[0]); -> guest_write32(g_eax + 0xCu, g_xmm0.u[0]);
 * MUTANT: guest_write32(g_eax + 4u, g_ecx); -> guest_write32(g_eax + 4u, g_edx);
 * MUTANT: if (g_eax == 0u) { -> if (g_eax == 0u) { g_xmm0.q[0] = 0u;
 * (The first MUTANT line is the integrated mutation. All five were run as scratch mutants and killed. The load-before-store ordering mutant, which reads the value before the clear, is killed by the alias cases.) */

/* Alias onto the runtime XMM slot by symbol name so no type clashes with other declarations
 * of g_xmm0 when this file is embedded in tests/c/test_game_replacements.c. */
extern __thread uint32_t t1479a_00341a10_xmm0[4] __asm__("g_xmm0");

#define T1479_00341A10_OBJ 0x300000u

static void t1479_00341a10_run(uint32_t top, uint32_t ret, uint32_t object, uint32_t value)
{
    prepare();
    g_esp = top;
    guest_write32(top, ret);
    guest_write32(top + 4u, object);
    guest_write32(top + 8u, value);
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) {
        t1479a_00341a10_xmm0[lane] = 0xDEADBE00u + lane;
    }
    invoke(0x00341A10u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
}

static void t1479_00341a10_xmm(uint32_t lane0)
{
    CHECK(t1479a_00341a10_xmm0[0] == lane0);
    CHECK(t1479a_00341a10_xmm0[1] == 0u);
    CHECK(t1479a_00341a10_xmm0[2] == 0u);
    CHECK(t1479a_00341a10_xmm0[3] == 0u);
}

static void t1479_00341a10_fill(uint32_t object)
{
    for (uint32_t offset = 0; offset < 0x20u; offset += 4u) {
        guest_write32(object + offset, 0xC0C00000u + offset);
    }
}

static void test_t1479_appendix_00341a10(void)
{
    const uint32_t object = T1479_00341A10_OBJ;
    const uint32_t values[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00000u, 0xFFFFFFFFu, 0x7F800000u,
                               0x00000001u};

    /* null object: eax = 0, ecx/edx untouched, XMM0 untouched, nothing written */
    for (unsigned k = 0; k < 3; k++) {
        for (uint32_t offset = 0; offset < 0x20u; offset += 4u) {
            guest_write32(offset, 0xA0A00000u + offset);
        }
        t1479_00341a10_run(0x100u, 0x12345678u, 0u, values[k]);
        CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
        for (unsigned lane = 0; lane < 4; ++lane) {
            CHECK(t1479a_00341a10_xmm0[lane] == 0xDEADBE00u + lane);
        }
        for (uint32_t offset = 0; offset < 0x20u; offset += 4u) {
            CHECK(guest_read32(offset) == 0xA0A00000u + offset);
        }
        CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == 0u);
        CHECK(guest_read32(0x108u) == values[k]);
    }

    /* old state != 3: +8 is cleared, +4 becomes 3, +0xC gets the value, xmm0 = {value,0,0,0} */
    {
        const uint32_t states[] = {0u, 1u, 2u, 4u, 0xFFFFFFFFu, 0x80000003u, 0x00000103u, 0x00010003u,
                                   0x80000000u, 7u};
        for (unsigned k = 0; k < 10; k++) {
            t1479_00341a10_fill(object);
            guest_write32(object + 4u, states[k]);
            t1479_00341a10_run(0x100u, 0x12345678u, object, values[k % 7u]);
            CHECK(g_eax == object && g_ecx == 3u && g_edx == states[k]);
            CHECK(guest_read32(object + 4u) == 3u);
            CHECK(guest_read32(object + 8u) == 0u);
            CHECK(guest_read32(object + 0xCu) == values[k % 7u]);
            CHECK(guest_read32(object + 0u) == 0xC0C00000u);
            CHECK(guest_read32(object + 0x10u) == 0xC0C00010u);
            CHECK(guest_read32(object + 0x14u) == 0xC0C00014u);
            t1479_00341a10_xmm(values[k % 7u]);
            CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == object);
            CHECK(guest_read32(0x108u) == values[k % 7u]);
        }
    }

    /* old state == 3: +8 is NOT cleared (it keeps its value), +4 stays 3, +0xC gets the value */
    for (unsigned k = 0; k < 7; k++) {
        t1479_00341a10_fill(object);
        guest_write32(object + 4u, 3u);
        t1479_00341a10_run(0x100u, 0x12345678u, object, values[k]);
        CHECK(g_eax == object && g_ecx == 3u && g_edx == 3u);
        CHECK(guest_read32(object + 4u) == 3u);
        CHECK(guest_read32(object + 8u) == 0xC0C00008u);
        CHECK(guest_read32(object + 0xCu) == values[k]);
        CHECK(guest_read32(object + 0u) == 0xC0C00000u);
        CHECK(guest_read32(object + 0x10u) == 0xC0C00010u);
        t1479_00341a10_xmm(values[k]);
    }

    /* the state compare is a full 32-bit equality: 0x100000003 style patterns that differ above bit 2 */
    for (uint32_t state = 0x00000003u; state != 0u; state <<= 4) {
        t1479_00341a10_fill(object);
        guest_write32(object + 4u, state);
        t1479_00341a10_run(0x100u, 0x12345678u, object, 0x40490FDBu);
        CHECK(g_edx == state);
        CHECK(guest_read32(object + 8u) == (state == 3u ? 0xC0C00008u : 0u));
        CHECK(guest_read32(object + 0xCu) == 0x40490FDBu);
        if (state == 0x30000000u) {
            break;
        }
    }

    /* alias (a): object = the stack top. [object+4] is the object argument slot itself (= 0x100, not 3),
     * so the +8 store zeroes the value argument slot BEFORE the movss load: xmm0 = 0 and [top+0xC] = 0 */
    t1479_00341a10_run(0x100u, 0x12345678u, 0x100u, 0x40000000u);
    CHECK(g_eax == 0x100u && g_ecx == 3u && g_edx == 0x100u);
    CHECK(guest_read32(0x100u) == 0x12345678u);
    CHECK(guest_read32(0x104u) == 3u);
    CHECK(guest_read32(0x108u) == 0u);
    CHECK(guest_read32(0x10Cu) == 0u);
    t1479_00341a10_xmm(0u);
    /* object = top-4: +4 is the return slot (old state 0x12345678 != 3), +8 the object slot (cleared),
     * +0xC the value slot (loaded after the clear, then rewritten with itself) */
    t1479_00341a10_run(0x100u, 0x12345678u, 0xFCu, 0x40400000u);
    CHECK(g_eax == 0xFCu && g_ecx == 3u && g_edx == 0x12345678u);
    CHECK(guest_read32(0x100u) == 3u);
    CHECK(guest_read32(0x104u) == 0u);
    CHECK(guest_read32(0x108u) == 0x40400000u);
    t1479_00341a10_xmm(0x40400000u);
    /* same with the return slot equal to 3: nothing is cleared, the object slot survives */
    t1479_00341a10_run(0x100u, 3u, 0xFCu, 0x40400000u);
    CHECK(g_eax == 0xFCu && g_ecx == 3u && g_edx == 3u);
    CHECK(guest_read32(0x100u) == 3u);
    CHECK(guest_read32(0x104u) == 0xFCu);
    CHECK(guest_read32(0x108u) == 0x40400000u);
    t1479_00341a10_xmm(0x40400000u);
    /* object = top-8: +8 is the return slot (cleared), +0xC the object slot (gets the value),
     * +4 the dword below the return address (set to 3) */
    guest_write32(0xFCu, 0x00000005u);
    t1479_00341a10_run(0x100u, 0x12345678u, 0xF8u, 0x41200000u);
    CHECK(g_eax == 0xF8u && g_edx == 5u && g_ecx == 3u);
    CHECK(guest_read32(0xFCu) == 3u);
    CHECK(guest_read32(0x100u) == 0u);
    CHECK(guest_read32(0x104u) == 0x41200000u);
    CHECK(guest_read32(0x108u) == 0x41200000u);
    t1479_00341a10_xmm(0x41200000u);
    /* object = top-0xC: +0xC is the return slot */
    guest_write32(0xF8u, 0x00000009u);
    t1479_00341a10_run(0x100u, 0x12345678u, 0xF4u, 0x41A00000u);
    CHECK(g_eax == 0xF4u && g_edx == 9u);
    CHECK(guest_read32(0xF8u) == 3u && guest_read32(0xFCu) == 0u);
    CHECK(guest_read32(0x100u) == 0x41A00000u);
    CHECK(guest_read32(0x104u) == 0xF4u);
    /* object = top+4 (the object argument slot): +4 is the value slot (state), +8 and +0xC above it */
    guest_write32(0x10Cu, 0xAAAA0001u);
    guest_write32(0x110u, 0xAAAA0002u);
    t1479_00341a10_run(0x100u, 0x12345678u, 0x104u, 0x00000007u);
    CHECK(g_eax == 0x104u && g_edx == 7u && g_ecx == 3u);
    CHECK(guest_read32(0x108u) == 3u);
    CHECK(guest_read32(0x10Cu) == 0u);
    CHECK(guest_read32(0x110u) == 7u);
    t1479_00341a10_xmm(7u);
    /* object = top+4 with value 3: the compare sees state 3, no clear */
    guest_write32(0x10Cu, 0xAAAA0001u);
    guest_write32(0x110u, 0xAAAA0002u);
    t1479_00341a10_run(0x100u, 0x12345678u, 0x104u, 0x00000003u);
    CHECK(g_edx == 3u);
    CHECK(guest_read32(0x108u) == 3u);
    CHECK(guest_read32(0x10Cu) == 0xAAAA0001u);
    CHECK(guest_read32(0x110u) == 3u);
    t1479_00341a10_xmm(3u);
    /* 32-bit address wrap: object 0xFFFFFFFC puts +4 at address 0, +8 at 4, +0xC at 8 */
    guest_write32(0u, 0x77u);
    guest_write32(4u, 0x88u);
    guest_write32(8u, 0x99u);
    t1479_00341a10_run(0x100u, 0x12345678u, 0xFFFFFFFCu, 0x42480000u);
    CHECK(g_eax == 0xFFFFFFFCu && g_edx == 0x77u && g_ecx == 3u);
    CHECK(guest_read32(0u) == 3u);
    CHECK(guest_read32(4u) == 0u);
    CHECK(guest_read32(8u) == 0x42480000u);
    t1479_00341a10_xmm(0x42480000u);
    memset(&memory[0], 0, 0x20);
}
