/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00177930, derived from the original instruction listing:
 *   eax=[esp+4]; ecx=[eax+0x7C]; xmm0=movss [esp+8]; movss [ecx+0x8FC],xmm0; ret.
 * There is no null check. Exit: eax=object, ecx=[object+0x7C] as read (never modified), edx
 * untouched, xmm0={float,0,0,0}, one dword written at ecx+0x8FC (32-bit address arithmetic).
 * No PUSH/POP in the original, so there is no saved-register slot: the alias controls cover the
 * argument slots (esp+4, esp+8), the return slot (esp), the child pointer being read from an
 * argument slot and the store landing on the child-pointer source. The alias expectations were
 * obtained by executing the original bytes in Unicorn on the named entry states (original-only,
 * never the draft) and agree with the read-before-store order of the listing. */
extern __thread uint32_t c00177930_xmm0[4] __asm__("g_xmm0");

static void c00177930_run(uint32_t object, uint32_t float_bits)
{
    prepare();
    guest_write32(g_esp + 4u, object);
    guest_write32(g_esp + 8u, float_bits);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) c00177930_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x00177930u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_edx == 0x33333333u);
}

static void c00177930_check_xmm_loaded(uint32_t float_bits)
{
    CHECK(c00177930_xmm0[0] == float_bits && c00177930_xmm0[1] == 0u);
    CHECK(c00177930_xmm0[2] == 0u && c00177930_xmm0[3] == 0u);
}

static void test_t1477_appendix_00177930(void)
{
    const uint32_t objects[] = {0x20000u, 0x2F000u};
    const uint32_t children[] = {0x40000u, 0x50004u, 0x60000u, 0x7E0000u};
    const uint32_t floats[] = {0u, 0x3F800000u, 0x80000000u, 0x7FC00001u, 0xFFFFFFFFu, 0x00000001u,
                               0x40490FD0u};
    for (unsigned o = 0; o < 2; ++o) {
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned x = 0; x < 7; ++x) {
                const uint32_t object = objects[o];
                const uint32_t child = children[c];
                for (uint32_t offset = 0; offset < 0x100u; offset += 4u)
                    guest_write32(object + offset, 0xC0C00000u + offset);
                for (uint32_t offset = 0x8E0u; offset < 0x910u; offset += 4u)
                    guest_write32(child + offset, 0xD0D00000u + offset);
                guest_write32(object + 0x7Cu, child);
                c00177930_run(object, floats[x]);
                CHECK(g_eax == object && g_ecx == child);
                c00177930_check_xmm_loaded(floats[x]);
                CHECK(guest_read32(child + 0x8FCu) == floats[x]);
                for (uint32_t offset = 0x8E0u; offset < 0x910u; offset += 4u) {
                    if (offset == 0x8FCu) continue;
                    CHECK(guest_read32(child + offset) == 0xD0D00000u + offset);
                }
                for (uint32_t offset = 0; offset < 0x100u; offset += 4u) {
                    if (offset == 0x7Cu) CHECK(guest_read32(object + offset) == child);
                    else CHECK(guest_read32(object + offset) == 0xC0C00000u + offset);
                }
                CHECK(guest_read32(0x100u) == 0x12345678u);
                CHECK(guest_read32(0x104u) == object && guest_read32(0x108u) == floats[x]);
            }
        }
    }

    /* (a) Argument-slot aliases: the child pointer makes the store land on a stack slot. */
    guest_write32(0x2007Cu, 0u - 0x7F8u);              /* child + 0x8FC = 0x104, the object slot */
    c00177930_run(0x20000u, 0x40490FD0u);
    CHECK(g_eax == 0x20000u && g_ecx == 0xFFFFF808u);
    c00177930_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x104u) == 0x40490FD0u && guest_read32(0x108u) == 0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x12345678u);
    guest_write32(0x2007Cu, 0u - 0x7F4u);              /* child + 0x8FC = 0x108, the float slot */
    c00177930_run(0x20000u, 0x40490FD0u);
    CHECK(g_eax == 0x20000u && g_ecx == 0xFFFFF80Cu);
    c00177930_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x108u) == 0x40490FD0u && guest_read32(0x104u) == 0x20000u);
    guest_write32(0x2007Cu, 0u - 0x7FCu);              /* child + 0x8FC = 0x100, the return slot */
    c00177930_run(0x20000u, 0x40490FD0u);
    CHECK(g_eax == 0x20000u && g_ecx == 0xFFFFF804u);
    c00177930_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x100u) == 0x40490FD0u && guest_read32(0x104u) == 0x20000u);
    /* The child pointer is read from an argument slot. Object 0x88: [obj+0x7C] is the object slot
     * 0x104 itself, so the child equals the object (0x88) and the store lands at 0x984. */
    c00177930_run(0x88u, 0x40490FD0u);
    CHECK(g_eax == 0x88u && g_ecx == 0x88u);
    c00177930_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x984u) == 0x40490FD0u && guest_read32(0x104u) == 0x88u);
    /* Object 0x8C: [obj+0x7C] is the float slot 0x108, the child is the float bits 0x1000. */
    c00177930_run(0x8Cu, 0x00001000u);
    CHECK(g_eax == 0x8Cu && g_ecx == 0x1000u);
    c00177930_check_xmm_loaded(0x00001000u);
    CHECK(guest_read32(0x18FCu) == 0x1000u && guest_read32(0x108u) == 0x1000u);
    /* Object 0x84: [obj+0x7C] is the return slot, set to 0x5000, so the child is 0x5000. */
    prepare();
    guest_write32(g_esp + 4u, 0x84u);
    guest_write32(g_esp + 8u, 0x40490FD0u);
    guest_write32(g_esp, 0x5000u);
    invoke(0x00177930u, 4u);
    CHECK(g_eax == 0x84u && g_ecx == 0x5000u);
    c00177930_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x58FCu) == 0x40490FD0u && guest_read32(0x100u) == 0x5000u);
    /* The store lands on the child-pointer source [object+0x7C]: ecx holds the value read before. */
    guest_write32(0x2007Cu, 0x20000u + 0x7Cu - 0x8FCu);
    c00177930_run(0x20000u, 0x40490FD0u);
    CHECK(g_eax == 0x20000u && g_ecx == 0x1F780u);
    c00177930_check_xmm_loaded(0x40490FD0u);
    CHECK(guest_read32(0x2007Cu) == 0x40490FD0u);
    /* (b) and (c): the original has no PUSH or POP, so no saved-register slot exists. */
    /* Address arithmetic wraps at 32 bits: object 0xFFFFFF84 reads its child at address 0. */
    guest_write32(0u, 0x5000u);
    guest_write32(0x58FCu, 0u);
    c00177930_run(0xFFFFFF84u, 0x77665544u);
    CHECK(g_eax == 0xFFFFFF84u && g_ecx == 0x5000u);
    c00177930_check_xmm_loaded(0x77665544u);
    CHECK(guest_read32(0x58FCu) == 0x77665544u);
    /* Child 0xFFFFF708 + 0x8FC wraps to 4. */
    guest_write32(0x2007Cu, 0xFFFFF708u);
    guest_write32(4u, 0u);
    c00177930_run(0x20000u, 0x11223344u);
    CHECK(g_eax == 0x20000u && g_ecx == 0xFFFFF708u);
    CHECK(guest_read32(4u) == 0x11223344u && guest_read32(0x100u) == 0x12345678u);
}
