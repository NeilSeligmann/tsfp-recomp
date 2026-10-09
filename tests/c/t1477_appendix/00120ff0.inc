/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00120FF0, derived from the original instruction listing:
 *   ecx=[esp+4]; eax=[ecx+4]; test ah,0x20 (bit 0x2000 of eax); je ret;
 *   xorps xmm0,xmm0; or eax,0x4000; [ecx+4]=eax; movss [ecx+0xCC],xmm0 (0.0f); ret.
 * There is no null check. Exit, bit clear: eax=flags unmodified, ecx=object, edx untouched, xmm0
 * untouched, nothing written. Exit, bit set: eax=flags|0x4000, ecx=object, edx untouched, xmm0 = 0 in
 * all four lanes, [obj+4]=eax then [obj+0xCC]=0.
 * No PUSH/POP in the original, so there is no saved-register slot: the alias controls cover the
 * argument slot (esp+4) and the return slot (esp) as the flags dword and as the zeroed dword. The
 * moved-stack expectations were obtained by executing the original bytes in Unicorn on the named
 * entry states (original-only, never the draft) and agree with the listing's store order. */
extern __thread uint32_t c00120ff0_xmm0[4] __asm__("g_xmm0");

static void c00120ff0_setup(uint32_t esp)
{
    prepare();
    g_esp = esp;
    guest_write32(g_esp, 0x0D0E0F10u);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) c00120ff0_xmm0[lane] = 0xDEADBE00u + lane;
}

static void c00120ff0_finish(void)
{
    invoke(0x00120FF0u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_edx == 0x33333333u);
}

static void c00120ff0_check_xmm_zero(void)
{
    for (unsigned lane = 0; lane < 4; ++lane) CHECK(c00120ff0_xmm0[lane] == 0u);
}

static void c00120ff0_check_xmm_untouched(void)
{
    for (unsigned lane = 0; lane < 4; ++lane) CHECK(c00120ff0_xmm0[lane] == 0xDEADBE00u + lane);
}

static void test_t1477_appendix_00120ff0(void)
{
    const uint32_t objects[] = {0x20000u, 0x2FF34u, 0x7F0000u};
    /* (flags in, flags out): out = in | 0x4000 when bit 0x2000 is set, otherwise unchanged */
    const uint32_t set_pairs[][2] = {
        {0x00002000u, 0x00006000u}, {0x00002001u, 0x00006001u}, {0x00006000u, 0x00006000u},
        {0x0000A000u, 0x0000E000u}, {0xFFFFFFFFu, 0xFFFFFFFFu}, {0x12342000u, 0x12346000u},
        {0x80002000u, 0x80006000u}, {0x00003FFFu, 0x00007FFFu}, {0xFFFFA000u, 0xFFFFE000u},
        {0x00002FFFu, 0x00006FFFu}};
    const uint32_t clear_flags[] = {0x00000000u, 0x00001FFFu, 0x00004000u, 0x00008000u,
                                    0x0000DFFFu, 0xFFFFDFFFu, 0x80000000u, 0x12345678u,
                                    0x00001000u, 0xFFFFC000u};
    for (unsigned o = 0; o < 3; ++o) {
        const uint32_t object = objects[o];
        for (unsigned f = 0; f < 10; ++f) {
            for (uint32_t offset = 0; offset < 0xD0u; offset += 4u)
                guest_write32(object + offset, 0xC0C00000u + offset);
            guest_write32(object + 4u, set_pairs[f][0]);
            c00120ff0_setup(0x100u);
            guest_write32(g_esp + 4u, object);
            c00120ff0_finish();
            CHECK(g_eax == set_pairs[f][1] && g_ecx == object);
            c00120ff0_check_xmm_zero();
            CHECK(guest_read32(object + 4u) == set_pairs[f][1] && guest_read32(object + 0xCCu) == 0u);
            for (uint32_t offset = 0; offset < 0xD0u; offset += 4u) {
                if (offset == 4u || offset == 0xCCu) continue;
                CHECK(guest_read32(object + offset) == 0xC0C00000u + offset);
            }
            CHECK(guest_read32(0x100u) == 0x0D0E0F10u && guest_read32(0x104u) == object);
        }
        for (unsigned f = 0; f < 10; ++f) {
            for (uint32_t offset = 0; offset < 0xD0u; offset += 4u)
                guest_write32(object + offset, 0xC0C00000u + offset);
            guest_write32(object + 4u, clear_flags[f]);
            c00120ff0_setup(0x100u);
            guest_write32(g_esp + 4u, object);
            c00120ff0_finish();
            CHECK(g_eax == clear_flags[f] && g_ecx == object);
            c00120ff0_check_xmm_untouched();
            CHECK(guest_read32(object + 4u) == clear_flags[f]);
            for (uint32_t offset = 0; offset < 0xD0u; offset += 4u) {
                if (offset == 4u) continue;
                CHECK(guest_read32(object + offset) == 0xC0C00000u + offset);
            }
            CHECK(guest_read32(0x100u) == 0x0D0E0F10u && guest_read32(0x104u) == object);
        }
    }

    /* No null check: object 0 reads and (bit set) writes the dwords at addresses 4 and 0xCC. */
    guest_write32(4u, 0x00002002u);
    guest_write32(0xCCu, 0x77u);
    c00120ff0_setup(0x100u);
    guest_write32(g_esp + 4u, 0u);
    c00120ff0_finish();
    CHECK(g_eax == 0x00006002u && g_ecx == 0u);
    c00120ff0_check_xmm_zero();
    CHECK(guest_read32(4u) == 0x00006002u && guest_read32(0xCCu) == 0u);
    guest_write32(4u, 0x00001002u);
    guest_write32(0xCCu, 0x77u);
    c00120ff0_setup(0x100u);
    guest_write32(g_esp + 4u, 0u);
    c00120ff0_finish();
    CHECK(g_eax == 0x00001002u && g_ecx == 0u);
    c00120ff0_check_xmm_untouched();
    CHECK(guest_read32(4u) == 0x00001002u && guest_read32(0xCCu) == 0x77u);

    /* (a) Argument-slot and return-slot aliases on a moved stack. */
    /* Stack at the object (esp = object): the object argument slot IS the flags dword at +4 and holds
     * the object itself, 0x2000, whose bit 0x2000 is set. ecx = 0x2000, eax = 0x6000, the flags
     * store rewrites the argument slot. */
    guest_write32(0x20CCu, 0x77u);
    c00120ff0_setup(0x2000u);
    guest_write32(g_esp + 4u, 0x2000u);
    c00120ff0_finish();
    CHECK(g_eax == 0x6000u && g_ecx == 0x2000u);
    c00120ff0_check_xmm_zero();
    CHECK(guest_read32(0x2004u) == 0x6000u && guest_read32(0x20CCu) == 0u);
    /* Stack at object + 0xC8: the object argument slot IS the zeroed dword at +0xCC. The object is
     * read before the zero store, so ecx keeps the object. */
    guest_write32(0x20004u, 0x2000u);
    c00120ff0_setup(0x200C8u);
    guest_write32(g_esp + 4u, 0x20000u);
    guest_write32(0x20004u, 0x2000u);
    c00120ff0_finish();
    CHECK(g_eax == 0x6000u && g_ecx == 0x20000u);
    c00120ff0_check_xmm_zero();
    CHECK(guest_read32(0x20004u) == 0x6000u && guest_read32(0x200CCu) == 0u);
    CHECK(guest_read32(0x200C8u) == 0x0D0E0F10u);
    /* Stack at object + 0xCC: the return slot IS the zeroed dword, the wrapper still pops 4. */
    c00120ff0_setup(0x200CCu);
    guest_write32(g_esp + 4u, 0x20000u);
    guest_write32(0x20004u, 0x2000u);
    c00120ff0_finish();
    CHECK(g_eax == 0x6000u && g_ecx == 0x20000u);
    CHECK(guest_read32(0x200CCu) == 0u && guest_read32(0x200D0u) == 0x20000u);
    /* Stack at object + 4: the return slot IS the flags dword (0x2000), rewritten to 0x6000. */
    c00120ff0_setup(0x20004u);
    guest_write32(g_esp, 0x2000u);
    guest_write32(g_esp + 4u, 0x20000u);
    guest_write32(0x200CCu, 0x77u);
    c00120ff0_finish();
    CHECK(g_eax == 0x6000u && g_ecx == 0x20000u);
    CHECK(guest_read32(0x20004u) == 0x6000u && guest_read32(0x200CCu) == 0u);
    CHECK(guest_read32(0x20008u) == 0x20000u);
    /* (b) and (c): the original has no PUSH or POP, so no saved-register slot exists. */
    /* Address arithmetic wraps at 32 bits: object 0xFFFFFFFC has +4 at address 0 and +0xCC at 0xC8. */
    guest_write32(0u, 0x00002000u);
    guest_write32(0xC8u, 0x77u);
    c00120ff0_setup(0x100u);
    guest_write32(g_esp + 4u, 0xFFFFFFFCu);
    c00120ff0_finish();
    CHECK(g_eax == 0x6000u && g_ecx == 0xFFFFFFFCu);
    c00120ff0_check_xmm_zero();
    CHECK(guest_read32(0u) == 0x6000u && guest_read32(0xC8u) == 0u);
}
