/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00350C60 (cdecl, 0 stack args). Expected values derived from the ORIGINAL listing:
 *   mov eax,[0x4C0454] ; xor ecx,ecx ; test eax,eax ; setl cl ; dec ecx ; and eax,ecx ; ret
 * so eax = value when value >= 0 (signed) else 0, ecx = 0xFFFFFFFF when value >= 0 else 0, edx and
 * ebx/esi/edi/ebp untouched, the only memory access is the read of 0x4C0454 (no writes).
 * No guest PUSH or POP in the original, so there is no saved-register slot to alias (cases b, c do not
 * exist). Alias (a): the return-address slot IS the global (esp = 0x4C0454) and the global sits on the
 * "first argument" slot (esp = 0x4C0450); the function reads the dword either way and pops 4.
 * MUTANT: ((int32_t)g_eax < 0) ? 0u : 0xFFFFFFFFu -> (g_eax > 0x80000000u) ? 0u : 0xFFFFFFFFu
 * MUTANT: ((int32_t)g_eax < 0) ? 0u : 0xFFFFFFFFu -> ((int32_t)g_eax <= 0) ? 0u : 0xFFFFFFFFu
 */

#define T1479_350C60_GLOBAL 0x004C0454u

static void t1479_350c60_run(uint32_t esp, uint32_t value, uint32_t edx_in, uint32_t eax_out, uint32_t ecx_out)
{
    prepare();
    g_esp = esp;
    guest_write32(0x4C0450u, 0xA1A1A1A1u);
    guest_write32(0x4C0458u, 0xA2A2A2A2u);
    guest_write32(esp, 0x12345678u);
    guest_write32(T1479_350C60_GLOBAL, value);
    g_edx = edx_in;
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    invoke(0x00350C60u, 4u);
    CHECK(g_eax == eax_out);
    CHECK(g_ecx == ecx_out);
    CHECK(g_edx == edx_in);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(guest_read32(T1479_350C60_GLOBAL) == value);
    if (esp == 0x100u) {
        CHECK(guest_read32(0x4C0450u) == 0xA1A1A1A1u);
        CHECK(guest_read32(0x4C0458u) == 0xA2A2A2A2u);
        CHECK(guest_read32(0x100u) == 0x12345678u);
    }
}

static void test_t1479_appendix_00350c60(void)
{
    static const struct { uint32_t value, eax, ecx; } rows[] = {
        {0x00000000u, 0x00000000u, 0xFFFFFFFFu}, /* zero is not negative: ecx all ones */
        {0x00000001u, 0x00000001u, 0xFFFFFFFFu},
        {0x7FFFFFFEu, 0x7FFFFFFEu, 0xFFFFFFFFu},
        {0x7FFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu}, /* largest signed value */
        {0x80000000u, 0x00000000u, 0x00000000u}, /* smallest signed value */
        {0x80000001u, 0x00000000u, 0x00000000u},
        {0xC0000000u, 0x00000000u, 0x00000000u},
        {0xFFFFFFFEu, 0x00000000u, 0x00000000u},
        {0xFFFFFFFFu, 0x00000000u, 0x00000000u}, /* the "none loaded" value -1 */
        {0x12345678u, 0x12345678u, 0xFFFFFFFFu},
        {0x00010000u, 0x00010000u, 0xFFFFFFFFu},
        {0x0000FFFFu, 0x0000FFFFu, 0xFFFFFFFFu},
        {0xF0000000u, 0x00000000u, 0x00000000u},
    };
    static const uint32_t edx_values[] = {0x33333333u, 0u, 0xFFFFFFFFu, 0x80000000u};
    static const uint32_t stacks[] = {0x100u, 0x7FFFF0u, 0x004C0454u, 0x004C0450u};
    for (unsigned row = 0; row < sizeof rows / sizeof rows[0]; row++) {
        for (unsigned e = 0; e < 4; e++) {
            for (unsigned s = 0; s < 4; s++) {
                t1479_350c60_run(stacks[s], rows[row].value, edx_values[e], rows[row].eax, rows[row].ecx);
            }
        }
    }
    /* the global sits on the argument slot: esp = 0x4C0450 puts the return address at 0x4C0450 and the
     * global on the first (unused) argument slot, the return slot is not the global there */
    t1479_350c60_run(0x004C0450u, 0xFFFFFFFFu, 0x33333333u, 0u, 0u);
    CHECK(guest_read32(0x4C0450u) == 0x12345678u);
    /* return slot is the global: the function reads the return address as the value */
    prepare();
    g_esp = 0x004C0454u;
    guest_write32(0x4C0454u, 0x80000000u);
    invoke(0x00350C60u, 4u);
    CHECK(g_eax == 0u && g_ecx == 0u && g_edx == 0x33333333u);
    CHECK(g_esp == 0x004C0458u);
    prepare();
    g_esp = 0x004C0454u;
    guest_write32(0x4C0454u, 0x7FFFFFFFu);
    invoke(0x00350C60u, 4u);
    CHECK(g_eax == 0x7FFFFFFFu && g_ecx == 0xFFFFFFFFu && g_edx == 0x33333333u);
    CHECK(g_esp == 0x004C0458u);
}
