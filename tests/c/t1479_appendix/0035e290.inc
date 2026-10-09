/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0035E290 (cdecl, 0 stack args). Expected values derived from the ORIGINAL listing:
 *   mov ecx,[0x76BBB0] ; xor eax,eax ; test ecx,ecx ; setne al ; ret
 * so ecx = the full dword, eax = 1 when any of its 32 bits is set else 0, edx and ebx/esi/edi/ebp untouched,
 * no writes, the only memory access is the dword read of 0x76BBB0.
 * No guest PUSH or POP in the original, so there is no saved-register slot to alias (cases b, c do not
 * exist). Alias (a): the return-address slot IS the global (esp = 0x76BBB0) and the global sits on the
 * first-argument slot (esp = 0x76BBAC).
 * MUTANT: g_eax = (g_ecx != 0u) ? 1u : 0u; -> g_eax = ((g_ecx & 0xFFu) != 0u) ? 1u : 0u;
 * MUTANT: g_eax = (g_ecx != 0u) ? 1u : 0u; -> g_eax = ((int32_t)g_ecx > 0) ? 1u : 0u;
 */

#define T1479_35E290_GLOBAL 0x0076BBB0u

static void t1479_35e290_run(uint32_t esp, uint32_t value, uint32_t edx_in)
{
    const uint32_t eax_out = value != 0u ? 1u : 0u;
    prepare();
    g_esp = esp;
    guest_write32(0x76BBACu, 0xA1A1A1A1u);
    guest_write32(0x76BBB4u, 0xA2A2A2A2u);
    guest_write32(esp, 0x12345678u);
    guest_write32(T1479_35E290_GLOBAL, value);
    g_edx = edx_in;
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    invoke(0x0035E290u, 4u);
    CHECK(g_eax == eax_out);
    CHECK(g_ecx == value);
    CHECK(g_edx == edx_in);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(guest_read32(T1479_35E290_GLOBAL) == value);
    if (esp == 0x100u) {
        CHECK(guest_read32(0x76BBACu) == 0xA1A1A1A1u);
        CHECK(guest_read32(0x76BBB4u) == 0xA2A2A2A2u);
        CHECK(guest_read32(0x100u) == 0x12345678u);
    }
}

static void test_t1479_appendix_0035e290(void)
{
    static const uint32_t values[] = {
        0x00000000u, 0x00000001u, 0x00000002u, 0x000000FFu, 0x00000100u, 0x0000FF00u, 0x00010000u,
        0x01000000u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFF000000u, 0xFFFFFFFFu, 0x12345678u,
    };
    static const uint32_t edx_values[] = {0x33333333u, 0u, 0xFFFFFFFFu, 0x80000000u};
    static const uint32_t stacks[] = {0x100u, 0x7FFFF0u, 0x0076BBB0u, 0x0076BBACu};
    for (unsigned v = 0; v < sizeof values / sizeof values[0]; v++) {
        for (unsigned e = 0; e < 4; e++) {
            for (unsigned s = 0; s < 4; s++) {
                t1479_35e290_run(stacks[s], values[v], edx_values[e]);
            }
        }
    }
    /* return slot is the global: the return address itself is read as the value */
    prepare();
    g_esp = 0x0076BBB0u;
    guest_write32(0x76BBB0u, 0u);
    invoke(0x0035E290u, 4u);
    CHECK(g_eax == 0u && g_ecx == 0u && g_edx == 0x33333333u);
    CHECK(g_esp == 0x0076BBB4u);
    prepare();
    g_esp = 0x0076BBB0u;
    guest_write32(0x76BBB0u, 0x00000100u);
    invoke(0x0035E290u, 4u);
    CHECK(g_eax == 1u && g_ecx == 0x100u && g_edx == 0x33333333u);
    CHECK(g_esp == 0x0076BBB4u);
}
