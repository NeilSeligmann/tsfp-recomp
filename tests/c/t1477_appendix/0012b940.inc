/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0012B940, derived from the original instruction listing:
 *   eax=[0x7450D0]; dec eax; jns +5 (SF of the 32-bit result); add eax,0xC8; xmm0=movss [esp+4];
 *   imul eax,eax,0x38 (low 32 bits); movss [eax+0x74253C],xmm0; ret.
 * Exit: eax=(index*0x38) mod 2^32, ecx/edx untouched, xmm0={float,0,0,0}, one dword written at
 * 0x74253C + eax (32-bit address arithmetic). The (counter -> eax) pairs of the table below were
 * obtained by executing the original bytes in Unicorn on named entry states (original-only, never
 * the draft) and agree with the hand derivation (counter 0 and 0xC8 give index 0xC7 -> 0x2B88,
 * 0x80000000 gives index 0x7FFFFFFF -> 0xFFFFFFC8, 0x80000001 is negative -> +0xC8 -> 0x2BC0).
 * No PUSH/POP in the original: the alias controls cover the argument slot (esp+4), the return
 * slot (esp) and the counter as the argument slot. The store address is always = 4 mod 8, so it can
 * hit the argument slot but never the counter at 0x7450D0 (= 0 mod 8) or the return slot at 0x100. */
extern __thread uint32_t c0012b940_xmm0[4] __asm__("g_xmm0");

#define C0012B940_WINDOW_LO 0x742400u
#define C0012B940_WINDOW_HI 0x745200u

static void c0012b940_run(uint32_t counter, uint32_t float_bits)
{
    prepare();
    guest_write32(g_esp + 8u, 0u); /* known slot above the argument (dirty memory when embedded) */
    guest_write32(0x7450D0u, counter);
    guest_write32(g_esp + 4u, float_bits);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) c0012b940_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x0012B940u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
}

static void c0012b940_check_xmm_loaded(uint32_t float_bits)
{
    CHECK(c0012b940_xmm0[0] == float_bits && c0012b940_xmm0[1] == 0u);
    CHECK(c0012b940_xmm0[2] == 0u && c0012b940_xmm0[3] == 0u);
}

static void c0012b940_fill_window(void)
{
    for (uint32_t address = C0012B940_WINDOW_LO; address < C0012B940_WINDOW_HI; address += 4u)
        guest_write32(address, 0xC0C00000u ^ address);
}

/* Every dword of the window except `target` still holds the fill pattern (the counter excepted,
 * which holds `counter`). */
static void c0012b940_check_window(uint32_t target, uint32_t counter)
{
    for (uint32_t address = C0012B940_WINDOW_LO; address < C0012B940_WINDOW_HI; address += 4u) {
        if (address == target) continue;
        if (address == 0x7450D0u) {
            CHECK(guest_read32(address) == counter);
            continue;
        }
        CHECK(guest_read32(address) == (0xC0C00000u ^ address));
    }
}

static void test_t1477_appendix_0012b940(void)
{
    static const uint32_t table[][2] = {
        {0x00000001u, 0x00000000u}, {0x00000002u, 0x00000038u}, {0x00000000u, 0x00002B88u},
        {0x000000C8u, 0x00002B88u}, {0x000000C9u, 0x00002BC0u}, {0x000000CAu, 0x00002BF8u},
        {0xFFFFFFFFu, 0x00002B50u}, {0xFFFFFFFEu, 0x00002B18u}, {0x80000000u, 0xFFFFFFC8u},
        {0x80000001u, 0x00002BC0u}, {0x80000002u, 0x00002BF8u}, {0x7FFFFFFFu, 0xFFFFFF90u},
        {0x40000000u, 0xFFFFFFC8u}, {0x00000100u, 0x000037C8u}, {0x00000003u, 0x00000070u}};
    const uint32_t floats[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00001u, 0xFFFFFFFFu, 0x40490FD0u};
    for (unsigned c = 0; c < 15; ++c) {
        for (unsigned x = 0; x < 6; ++x) {
            const uint32_t counter = table[c][0];
            const uint32_t expected_eax = table[c][1];
            const uint32_t target = 0x74253Cu + expected_eax;
            c0012b940_fill_window();
            c0012b940_run(counter, floats[x]);
            CHECK(g_eax == expected_eax);
            c0012b940_check_xmm_loaded(floats[x]);
            CHECK(guest_read32(target) == floats[x]);
            c0012b940_check_window(target, counter);
            CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == floats[x]);
        }
    }
    /* Index 0x1000 - 1 is outside the window: only the target and the counter are checked. */
    c0012b940_run(0x1000u, 0x40490FD0u);
    CHECK(g_eax == 0x37FC8u && guest_read32(0x74253Cu + 0x37FC8u) == 0x40490FD0u);
    CHECK(guest_read32(0x7450D0u) == 0x1000u);

    /* (a) Argument-slot aliases. The store address is 0x74253C + eax: counter 0x0DB4C880 gives
     * index 0x0DB4C87F, whose 32-bit product 0xFF8BDBC8 lands the store on the argument slot 0x104
     * (a 64-bit product would miss it), and the negative counter 0x8DB4C7B8 reaches the same slot
     * through the +0xC8 path. Both leave the float in place and the other slots untouched. */
    c0012b940_run(0x0DB4C880u, 0x40490FD0u);
    CHECK(g_eax == 0xFF8BDBC8u && guest_read32(0x104u) == 0x40490FD0u);
    c0012b940_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x108u) == 0u);
    c0012b940_run(0x8DB4C7B8u, 0x40490FD0u);
    CHECK(g_eax == 0xFF8BDBC8u && guest_read32(0x104u) == 0x40490FD0u);
    c0012b940_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x108u) == 0u);
    /* The stack placed so that the argument slot is the store target 0x74253C (counter 1). */
    prepare();
    g_esp = 0x742538u;
    guest_write32(g_esp, 0x0D0E0F10u);
    guest_write32(g_esp + 4u, 0x40490FD0u);
    guest_write32(0x7450D0u, 1u);
    invoke(0x0012B940u, 4u);
    CHECK(g_eax == 0u && guest_read32(0x74253Cu) == 0x40490FD0u && guest_read32(0x742538u) == 0x0D0E0F10u);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
    c0012b940_check_xmm_loaded(0x40490FD0u);
    /* The return slot is the store target (esp = 0x74253C): the wrapper still pops 4. */
    prepare();
    g_esp = 0x74253Cu;
    guest_write32(g_esp, 0x0D0E0F10u);
    guest_write32(g_esp + 4u, 0x3F000000u);
    guest_write32(0x7450D0u, 1u);
    invoke(0x0012B940u, 4u);
    CHECK(g_eax == 0u && guest_read32(0x74253Cu) == 0x3F000000u && guest_read32(0x742540u) == 0x3F000000u);
    /* The argument slot IS the counter (esp + 4 = 0x7450D0): the float bits are the counter value
     * 0xC9, so index 0xC8 stores 0xC9 at 0x7450FC and the counter keeps 0xC9. */
    prepare();
    g_esp = 0x7450CCu;
    guest_write32(g_esp, 0x0D0E0F10u);
    guest_write32(0x7450D0u, 0xC9u);
    invoke(0x0012B940u, 4u);
    CHECK(g_eax == 0x2BC0u && guest_read32(0x7450FCu) == 0xC9u && guest_read32(0x7450D0u) == 0xC9u);
    c0012b940_check_xmm_loaded(0xC9u);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
    /* (b) and (c): the original has no PUSH or POP, so no saved-register slot exists. */
}
