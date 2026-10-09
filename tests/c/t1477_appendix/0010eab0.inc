/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0010EAB0, derived from the original instruction listing:
 *   eax=[esp+4]; test eax,eax; je ret; ecx=[eax+4]; xmm0=movss [esp+8]; and ecx,~2; or ecx,4;
 *   movss [eax+0x84],xmm0; [eax+4]=ecx; ret.
 * Exit non-null: eax=object, ecx=(flags&~2)|4, edx untouched, xmm0={float,0,0,0}.
 * Exit null: eax=0, ecx/edx/xmm0 untouched, nothing read or written.
 * No PUSH/POP in the original, so there is no saved-register slot: the alias controls cover the
 * argument slots (esp+4, esp+8) and the return slot (esp). The alias expectations were obtained
 * by executing the original bytes in Unicorn on the named entry states (original-only, never the
 * draft) and agree with the read-before-store order of the listing. */
extern __thread uint32_t c0010eab0_xmm0[4] __asm__("g_xmm0");

static void c0010eab0_run(uint32_t object, uint32_t float_bits)
{
    prepare();
    guest_write32(g_esp + 4u, object);
    guest_write32(g_esp + 8u, float_bits);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) c0010eab0_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x0010EAB0u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_edx == 0x33333333u);
}

static void c0010eab0_check_xmm_loaded(uint32_t float_bits)
{
    CHECK(c0010eab0_xmm0[0] == float_bits && c0010eab0_xmm0[1] == 0u);
    CHECK(c0010eab0_xmm0[2] == 0u && c0010eab0_xmm0[3] == 0u);
}

static void test_t1477_appendix_0010eab0(void)
{
    const uint32_t objects[] = {0x20000u, 0x2FFF8u, 0x300000u, 0x7F0000u};
    /* (flags in, flags out): bit 1 cleared and bit 2 set, every other bit kept */
    const uint32_t flag_pairs[][2] = {
        {0x00000000u, 0x00000004u}, {0x00000001u, 0x00000005u}, {0x00000002u, 0x00000004u},
        {0x00000003u, 0x00000005u}, {0x00000004u, 0x00000004u}, {0x00000006u, 0x00000004u},
        {0x0000000Au, 0x0000000Cu}, {0x80000000u, 0x80000004u}, {0xFFFFFFFFu, 0xFFFFFFFDu},
        {0xFFFFFFFBu, 0xFFFFFFFDu}, {0x12345670u, 0x12345674u}, {0x12345672u, 0x12345674u},
        {0x7FFFFFFDu, 0x7FFFFFFDu}};
    const uint32_t floats[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00001u, 0xFFFFFFFFu, 0x00000001u};
    for (unsigned o = 0; o < 4; ++o) {
        for (unsigned f = 0; f < 13; ++f) {
            for (unsigned x = 0; x < 6; ++x) {
                const uint32_t object = objects[o];
                for (uint32_t offset = 0; offset < 0x90u; offset += 4u)
                    guest_write32(object + offset, 0xC0C00000u + offset);
                guest_write32(object + 4u, flag_pairs[f][0]);
                c0010eab0_run(object, floats[x]);
                CHECK(g_eax == object);
                CHECK(g_ecx == flag_pairs[f][1]);
                c0010eab0_check_xmm_loaded(floats[x]);
                CHECK(guest_read32(object + 4u) == flag_pairs[f][1]);
                CHECK(guest_read32(object + 0x84u) == floats[x]);
                /* every other dword of the object window is untouched */
                for (uint32_t offset = 0; offset < 0x90u; offset += 4u) {
                    if (offset == 4u || offset == 0x84u) continue;
                    CHECK(guest_read32(object + offset) == 0xC0C00000u + offset);
                }
                CHECK(guest_read32(0x100u) == 0x12345678u);
                CHECK(guest_read32(0x104u) == object && guest_read32(0x108u) == floats[x]);
            }
        }
    }
    /* Null object: nothing read or written, ecx/edx/xmm0 keep their entry values, eax = 0. */
    for (unsigned x = 0; x < 6; ++x) {
        for (uint32_t offset = 0; offset < 0x90u; offset += 4u)
            guest_write32(offset + 0x200u, 0xC0C00000u + offset);
        guest_write32(4u, 0x0000000Au);
        guest_write32(0x84u, 0x0BADF00Du);
        c0010eab0_run(0u, floats[x]);
        CHECK(g_eax == 0u && g_ecx == 0x22222222u);
        for (unsigned lane = 0; lane < 4; ++lane) CHECK(c0010eab0_xmm0[lane] == 0xDEADBE00u + lane);
        CHECK(guest_read32(4u) == 0x0000000Au && guest_read32(0x84u) == 0x0BADF00Du);
        CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == 0u);
        CHECK(guest_read32(0x108u) == floats[x]);
    }

    /* (a) Argument-slot aliases (esp+4 = 0x104, esp+8 = 0x108, esp = 0x100). */
    /* object 0x80: flags dword is 0x84, the float store (+0x84) lands on the object argument slot. */
    prepare();
    guest_write32(0x84u, 0x1234567Bu);
    c0010eab0_run(0x80u, 0x40490FD0u);
    CHECK(g_eax == 0x80u && g_ecx == 0x1234567Du);
    c0010eab0_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x84u) == 0x1234567Du && guest_read32(0x104u) == 0x40490FD0u);
    CHECK(guest_read32(0x108u) == 0x40490FD0u && guest_read32(0x100u) == 0x12345678u);
    /* object 0x84: the float store lands on the float argument slot (same value rewritten). */
    prepare();
    guest_write32(0x88u, 0x1234567Bu);
    c0010eab0_run(0x84u, 0x40490FD0u);
    CHECK(g_eax == 0x84u && g_ecx == 0x1234567Du);
    c0010eab0_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x88u) == 0x1234567Du && guest_read32(0x108u) == 0x40490FD0u);
    CHECK(guest_read32(0x104u) == 0x84u);
    /* object 0x7C: the float store lands on the return slot, flags dword at 0x80. */
    prepare();
    guest_write32(0x80u, 0x7u);
    c0010eab0_run(0x7Cu, 0x40490FD0u);
    CHECK(g_eax == 0x7Cu && g_ecx == 0x5u);
    c0010eab0_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x40490FD0u && guest_read32(0x80u) == 0x5u);
    /* object 0xFC: the flags store (+4) lands on the return slot, the float store at 0x180. */
    prepare();
    c0010eab0_run(0xFCu, 0x40490FD0u);   /* the return slot holds prepare()'s 0x12345678 */
    CHECK(g_eax == 0xFCu && g_ecx == 0x1234567Cu);
    CHECK(guest_read32(0x100u) == 0x1234567Cu && guest_read32(0x180u) == 0x40490FD0u);
    CHECK(guest_read32(0x104u) == 0xFCu && guest_read32(0x108u) == 0x40490FD0u);
    /* object 0x100: the flags dword IS the object argument slot, which holds 0x100, so the flags are
     * read from the stack: ecx = (0x100 & ~2) | 4 = 0x104 and the flags store rewrites that slot. */
    prepare();
    c0010eab0_run(0x100u, 0x40490FD0u);
    CHECK(g_eax == 0x100u && g_ecx == 0x104u);
    c0010eab0_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x104u) == 0x104u && guest_read32(0x184u) == 0x40490FD0u);
    /* object 0x104: the flags dword is the float argument slot, so the flags are the float bits:
     * ecx = (0x40490FD0 & ~2) | 4 = 0x40490FD4, xmm0 keeps the original float, and the flags store
     * overwrites the float slot after both reads. */
    prepare();
    c0010eab0_run(0x104u, 0x40490FD0u);
    CHECK(g_eax == 0x104u && g_ecx == 0x40490FD4u);
    c0010eab0_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x108u) == 0x40490FD4u && guest_read32(0x188u) == 0x40490FD0u);
    /* Same with a float whose bit 1 is set: ecx clears bit 1 and sets bit 2. */
    prepare();
    c0010eab0_run(0x104u, 0x40490FD2u);
    CHECK(g_ecx == 0x40490FD4u && guest_read32(0x188u) == 0x40490FD2u);
    c0010eab0_check_xmm_loaded(0x40490FD2u);
    /* (b) and (c): the original has no PUSH or POP, so no saved-register slot exists. */
    /* A moved stack: arguments sit inside the object, flags dword of the object is the return slot
     * of the new frame (esp = object + 4 - 4). The reads still precede the stores. */
    prepare();
    g_esp = 0x3000u;
    guest_write32(g_esp, 0x0D0E0F10u);
    guest_write32(g_esp + 4u, 0x3000u - 4u + 0u);   /* object whose +4 is the return slot 0x3000 */
    guest_write32(g_esp + 8u, 0x3F000000u);
    invoke(0x0010EAB0u, 4u);
    CHECK(g_eax == 0x2FFCu && g_ecx == ((0x0D0E0F10u & 0xFFFFFFFDu) | 4u));
    CHECK(guest_read32(0x3000u) == g_ecx && guest_read32(0x2FFCu + 0x84u) == 0x3F000000u);
    /* Address arithmetic wraps at 32 bits: object 0xFFFFFFFC puts +4 at 0 and +0x84 at 0x80. */
    prepare();
    guest_write32(0u, 0x2u);
    guest_write32(0x80u, 0xAAAAAAAAu);
    c0010eab0_run(0xFFFFFFFCu, 0x77665544u);
    CHECK(g_eax == 0xFFFFFFFCu && g_ecx == 4u);
    c0010eab0_check_xmm_loaded(0x77665544u);
    CHECK(guest_read32(0u) == 4u && guest_read32(0x80u) == 0x77665544u);
}
