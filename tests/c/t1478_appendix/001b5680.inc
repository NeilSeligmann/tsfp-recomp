/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x001B5680, derived from the original instruction listing:
 *   eax = [esp+4]; ecx = [esp+8]; xorps xmm0,xmm0; [eax+0x44] = ecx; [eax+0x48] = low dword of xmm0
 * Exit: eax = arg0, ecx = arg1, edx untouched, xmm0 = 0 in all lanes, nothing else changed. */
/* Alias onto the runtime XMM slot by symbol name so no type clashes with other declarations
 * of g_xmm0 when this file is embedded in tests/c/test_game_replacements.c. */
extern __thread uint32_t t1478a_001b5680_xmm0[4] __asm__("g_xmm0");

static void t1478a_001b5680_run(uint32_t object, uint32_t value)
{
    prepare();
    guest_write32(g_esp + 4u, object);
    guest_write32(g_esp + 8u, value);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) t1478a_001b5680_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x001B5680u, 4u);
}

static void test_t1478_appendix_001b5680(void)
{
    const uint32_t objects[] = {0x20000u, 0x20004u, 0x7FFFFu, 0x300000u};
    const uint32_t values[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu, 0x3F800000u};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 5; ++j) {
            const uint32_t object = objects[i];
            for (uint32_t offset = 0x3C; offset < 0x58; offset += 4u)
                guest_write32(object + offset, 0xC0C00000u + offset);
            t1478a_001b5680_run(object, values[j]);
            CHECK(g_eax == object && g_ecx == values[j] && g_edx == 0x33333333u);
            CHECK(guest_read32(object + 0x44u) == values[j]);
            CHECK(guest_read32(object + 0x48u) == 0u);
            /* neighbours of the two written dwords are untouched */
            CHECK(guest_read32(object + 0x3Cu) == 0xC0C00000u + 0x3Cu);
            CHECK(guest_read32(object + 0x40u) == 0xC0C00000u + 0x40u);
            CHECK(guest_read32(object + 0x4Cu) == 0xC0C00000u + 0x4Cu);
            CHECK(guest_read32(object + 0x50u) == 0xC0C00000u + 0x50u);
            CHECK(guest_read32(object + 0x54u) == 0xC0C00000u + 0x54u);
            for (unsigned lane = 0; lane < 4; ++lane) CHECK(t1478a_001b5680_xmm0[lane] == 0u);
            CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
            CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
            CHECK(guest_read32(0x100u) == 0x12345678u);   /* return slot untouched */
        }
    }
    /* Object whose +0x44/+0x48 land on the argument slots (esp+4 = 0x104, esp+8 = 0x108): both
     * arguments were read first, so exit registers hold the original arguments, while the stack
     * slots now hold the stored value and zero. */
    t1478a_001b5680_run(0xC0u, 0xCAFEF00Du);
    CHECK(g_eax == 0xC0u && g_ecx == 0xCAFEF00Du && g_edx == 0x33333333u);
    CHECK(guest_read32(0x104u) == 0xCAFEF00Du && guest_read32(0x108u) == 0u);
    for (unsigned lane = 0; lane < 4; ++lane) CHECK(t1478a_001b5680_xmm0[lane] == 0u);
    /* Object whose +0x44 lands on the return slot: [esp] is overwritten, wrapper pop still +4. */
    t1478a_001b5680_run(0xBCu, 0x0BADF00Du);
    CHECK(g_eax == 0xBCu && g_ecx == 0x0BADF00Du);
    CHECK(guest_read32(0x100u) == 0x0BADF00Du && guest_read32(0x104u) == 0u);
    /* Address arithmetic wraps at 32 bits: object 0xFFFFFFBC writes at 0x00 and 0x04. */
    t1478a_001b5680_run(0xFFFFFFBCu, 0x77665544u);
    CHECK(g_eax == 0xFFFFFFBCu && guest_read32(0u) == 0x77665544u && guest_read32(4u) == 0u);
}
