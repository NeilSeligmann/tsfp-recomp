/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0010EB50, derived from the original instruction listing:
 *   eax=[esp+4]; test eax,eax; je ret; xmm0=movss [esp+8]; movss [eax+0x70],xmm0; ret.
 * Exit non-null: eax=object, ecx/edx untouched, xmm0={float,0,0,0}, one dword written at +0x70.
 * Exit null: eax=0, ecx/edx/xmm0 untouched, nothing read or written.
 * No PUSH/POP in the original, so there is no saved-register slot: the alias controls cover the
 * argument slots (esp+4, esp+8) and the return slot (esp). Alias expectations follow the
 * read-before-store order of the listing (the float is read, then stored, nothing else reads). */
extern __thread uint32_t c0010eb50_xmm0[4] __asm__("g_xmm0");
static const uint32_t c0010eb50_field = 0x70u;

static void c0010eb50_run(uint32_t object, uint32_t float_bits)
{
    prepare();
    guest_write32(g_esp + 4u, object);
    guest_write32(g_esp + 8u, float_bits);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) c0010eb50_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x0010EB50u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
}

static void c0010eb50_check_xmm_loaded(uint32_t float_bits)
{
    CHECK(c0010eb50_xmm0[0] == float_bits && c0010eb50_xmm0[1] == 0u);
    CHECK(c0010eb50_xmm0[2] == 0u && c0010eb50_xmm0[3] == 0u);
}

static void test_t1477_appendix_0010eb50(void)
{
    const uint32_t objects[] = {0x20000u, 0x2FFF8u, 0x300000u, 0x7F0000u, 0x1FFF4u};
    const uint32_t floats[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00001u, 0xFFFFFFFFu, 0x00000001u,
                               0x40490FD0u};
    for (unsigned o = 0; o < 5; ++o) {
        for (unsigned x = 0; x < 7; ++x) {
            const uint32_t object = objects[o];
            for (uint32_t offset = 0; offset < 0x90u; offset += 4u)
                guest_write32(object + offset, 0xC0C00000u + offset);
            c0010eb50_run(object, floats[x]);
            CHECK(g_eax == object);
            c0010eb50_check_xmm_loaded(floats[x]);
            CHECK(guest_read32(object + c0010eb50_field) == floats[x]);
            for (uint32_t offset = 0; offset < 0x90u; offset += 4u) {
                if (offset == c0010eb50_field) continue;
                CHECK(guest_read32(object + offset) == 0xC0C00000u + offset);
            }
            CHECK(guest_read32(0x100u) == 0x12345678u);
            CHECK(guest_read32(0x104u) == object && guest_read32(0x108u) == floats[x]);
        }
    }
    /* Null object: nothing read or written, ecx/edx/xmm0 keep their entry values, eax = 0. */
    for (unsigned x = 0; x < 7; ++x) {
        guest_write32(c0010eb50_field, 0x0BADF00Du);
        c0010eb50_run(0u, floats[x]);
        CHECK(g_eax == 0u);
        for (unsigned lane = 0; lane < 4; ++lane) CHECK(c0010eb50_xmm0[lane] == 0xDEADBE00u + lane);
        CHECK(guest_read32(c0010eb50_field) == 0x0BADF00Du);
        CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == 0u);
        CHECK(guest_read32(0x108u) == floats[x]);
    }

    /* (a) Argument-slot aliases (esp = 0x100 return slot, 0x104 object slot, 0x108 float slot). */
    /* The store lands on the object argument slot: eax already holds the original pointer. */
    c0010eb50_run(0x104u - c0010eb50_field, 0x40490FD0u);
    CHECK(g_eax == 0x104u - c0010eb50_field);
    c0010eb50_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x104u) == 0x40490FD0u && guest_read32(0x108u) == 0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x12345678u);
    /* The store lands on the float argument slot: the same bits are rewritten. */
    c0010eb50_run(0x108u - c0010eb50_field, 0x40490FD0u);
    CHECK(g_eax == 0x108u - c0010eb50_field);
    c0010eb50_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x108u) == 0x40490FD0u && guest_read32(0x104u) == 0x108u - c0010eb50_field);
    /* The store lands on the return slot, the wrapper still pops 4. */
    c0010eb50_run(0x100u - c0010eb50_field, 0x40490FD0u);
    CHECK(g_eax == 0x100u - c0010eb50_field);
    c0010eb50_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x40490FD0u);
    CHECK(guest_read32(0x104u) == 0x100u - c0010eb50_field && guest_read32(0x108u) == 0x40490FD0u);
    /* The store lands on the object argument slot again, with different float bits. */
    c0010eb50_run(0x100u - c0010eb50_field + 4u, 0x01020304u);
    CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == 0x01020304u);
    CHECK(guest_read32(0x108u) == 0x01020304u);
    /* (b) and (c): the original has no PUSH or POP, so no saved-register slot exists. */
    /* Moved stack inside an object: the new return slot is the store target. */
    prepare();
    g_esp = 0x3000u;
    guest_write32(g_esp, 0x0D0E0F10u);
    guest_write32(g_esp + 4u, 0x3000u - c0010eb50_field);
    guest_write32(g_esp + 8u, 0x3F000000u);
    invoke(0x0010EB50u, 4u);
    CHECK(g_eax == 0x3000u - c0010eb50_field && guest_read32(0x3000u) == 0x3F000000u);
    CHECK(g_ecx == 0x22222222u);
    /* Address arithmetic wraps at 32 bits: object -field stores at address 0. */
    guest_write32(0u, 0xAAAAAAAAu);
    guest_write32(4u, 0xBBBBBBBBu);
    c0010eb50_run(0u - c0010eb50_field, 0x77665544u);
    CHECK(g_eax == 0u - c0010eb50_field);
    c0010eb50_check_xmm_loaded(0x77665544u);
    CHECK(guest_read32(0u) == 0x77665544u && guest_read32(4u) == 0xBBBBBBBBu);
}
