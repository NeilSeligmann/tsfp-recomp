/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00349390 (legacy vector mode, cdecl, 3 stack args). Expected values derived from
 * the ORIGINAL instruction listing (args: dest=[esp+4], A=[esp+8], B=[esp+0xC]):
 *   eax=dest ; ecx=A ; edx=[A] ; xorps xmm0,xmm0 ; [eax+0xC]=edx ; edx=[A+4] ; [eax+0x10]=edx ;
 *   ecx=[A+8] ; [eax+0x14]=ecx ; ecx=[esp+0xC] (B, read AFTER the three stores) ; edx=[B] ; [eax+0x18]=edx ;
 *   edx=[B+4] ; [eax+0x1C]=edx ; ecx=[B+8] ; [eax+0x20]=ecx ; movss [eax+0x38],xmm0 ; [eax+8]=1 ; ret.
 * Exit: eax=dest, ecx=[B+8], edx=[B+4] (as read before the +0x1C store), xmm0={0,0,0,0}, others untouched.
 * No guest PUSH or POP in the original, so there is no saved-register slot to alias (cases b, c do not
 * exist). Alias (a) is covered: dest on the argument and return slots (the stores rewrite the B argument
 * slot and the return-address slot), A overlapping dest (the copy reads values it just wrote) and B
 * overlapping dest (the second copy reads values the first copy wrote), plus 32 bit address wrap.
 * MUTANT: guest_write32(g_eax + 0x38u, g_xmm0.u[0]); -> guest_write32(g_eax + 0x34u, g_xmm0.u[0]);
 * MUTANT: guest_write32(g_eax + 0x08u, 1u); -> guest_write32(g_eax + 0x08u, 0u);
 */

extern __thread uint32_t t1479_349390_xmm0[4] __asm__("g_xmm0");

#define T1479_349390_DEST 0x20000u
#define T1479_349390_A 0x21000u
#define T1479_349390_B 0x22000u
#define T1479_349390_WIN 0x20000u /* 0x20000..0x22100 scratch window */
#define T1479_349390_WINSZ 0x2100u

static const uint32_t t1479_349390_dest_garbage = 0xC0C00000u;

static void t1479_349390_reset(void)
{
    memset(&memory[T1479_349390_WIN], 0, T1479_349390_WINSZ);
    memset(&memory[0xA0], 0, 0x180);
    for (uint32_t offset = 0; offset < 0x60; offset += 4u) {
        guest_write32(T1479_349390_DEST + offset, t1479_349390_dest_garbage + offset);
    }
}

static void t1479_349390_run(uint32_t esp, uint32_t dest, uint32_t src_a, uint32_t src_b)
{
    prepare();
    g_esp = esp;
    guest_write32(esp, 0x12345678u);
    guest_write32(esp + 4u, dest);
    guest_write32(esp + 8u, src_a);
    guest_write32(esp + 12u, src_b);
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) t1479_349390_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x00349390u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_eax == dest);
    CHECK(t1479_349390_xmm0[0] == 0u && t1479_349390_xmm0[1] == 0u);
    CHECK(t1479_349390_xmm0[2] == 0u && t1479_349390_xmm0[3] == 0u);
}

static void test_t1479_appendix_00349390(void)
{
    /* basic copy, value classes chosen so a float conversion or a wrong word would show */
    static const uint32_t patterns[][6] = {
        {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u, 0x66666666u},
        {0x00000000u, 0x80000000u, 0x7FC00001u, 0xFFFFFFFFu, 0x3F800000u, 0x7F800000u},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u, 0x00000005u, 0x00000006u},
    };
    for (unsigned p = 0; p < 4; p++) {
        t1479_349390_reset();
        for (unsigned k = 0; k < 3; k++) {
            guest_write32(T1479_349390_A + 4u * k, patterns[p][k]);
            guest_write32(T1479_349390_B + 4u * k, patterns[p][3 + k]);
        }
        guest_write32(T1479_349390_A + 0x0Cu, 0xAAAA0001u); /* just past the vectors: must not be used */
        guest_write32(T1479_349390_B + 0x0Cu, 0xBBBB0002u);
        guest_write32(T1479_349390_A - 4u, 0xAAAA0003u);
        guest_write32(T1479_349390_B - 4u, 0xBBBB0004u);
        t1479_349390_run(0x100u, T1479_349390_DEST, T1479_349390_A, T1479_349390_B);
        CHECK(g_ecx == patterns[p][5]);
        CHECK(g_edx == patterns[p][4]);
        CHECK(guest_read32(T1479_349390_DEST + 0x0Cu) == patterns[p][0]);
        CHECK(guest_read32(T1479_349390_DEST + 0x10u) == patterns[p][1]);
        CHECK(guest_read32(T1479_349390_DEST + 0x14u) == patterns[p][2]);
        CHECK(guest_read32(T1479_349390_DEST + 0x18u) == patterns[p][3]);
        CHECK(guest_read32(T1479_349390_DEST + 0x1Cu) == patterns[p][4]);
        CHECK(guest_read32(T1479_349390_DEST + 0x20u) == patterns[p][5]);
        CHECK(guest_read32(T1479_349390_DEST + 0x38u) == 0u);
        CHECK(guest_read32(T1479_349390_DEST + 0x08u) == 1u);
        /* untouched neighbours of the record, the sources and the stack */
        const uint32_t untouched[] = {0x00u, 0x04u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u, 0x3Cu, 0x40u, 0x44u, 0x5Cu};
        for (unsigned k = 0; k < sizeof untouched / sizeof untouched[0]; k++) {
            CHECK(guest_read32(T1479_349390_DEST + untouched[k]) == t1479_349390_dest_garbage + untouched[k]);
        }
        for (unsigned k = 0; k < 3; k++) {
            CHECK(guest_read32(T1479_349390_A + 4u * k) == patterns[p][k]);
            CHECK(guest_read32(T1479_349390_B + 4u * k) == patterns[p][3 + k]);
        }
        CHECK(guest_read32(T1479_349390_A + 0x0Cu) == 0xAAAA0001u && guest_read32(T1479_349390_B + 0x0Cu) == 0xBBBB0002u);
        CHECK(guest_read32(T1479_349390_A - 4u) == 0xAAAA0003u && guest_read32(T1479_349390_B - 4u) == 0xBBBB0004u);
        CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == T1479_349390_DEST);
        CHECK(guest_read32(0x108u) == T1479_349390_A && guest_read32(0x10Cu) == T1479_349390_B);
    }

    /* same source for both vectors, and dest outside the usual window with a high stack */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0x01020304u);
    guest_write32(T1479_349390_A + 4u, 0x05060708u);
    guest_write32(T1479_349390_A + 8u, 0x090A0B0Cu);
    t1479_349390_run(0x7FFFF0u - 0x10u, 0x300000u, T1479_349390_A, T1479_349390_A);
    CHECK(g_ecx == 0x090A0B0Cu && g_edx == 0x05060708u);
    CHECK(guest_read32(0x30000Cu) == 0x01020304u && guest_read32(0x300018u) == 0x01020304u);
    CHECK(guest_read32(0x300010u) == 0x05060708u && guest_read32(0x30001Cu) == 0x05060708u);
    CHECK(guest_read32(0x300014u) == 0x090A0B0Cu && guest_read32(0x300020u) == 0x090A0B0Cu);
    CHECK(guest_read32(0x300038u) == 0u && guest_read32(0x300008u) == 1u);

    /* (a) dest = 0xF8 sits under the stack: stores hit 0x104 (dest slot, A0), 0x108 (A slot, A1),
     * 0x10C (B slot, A2), 0x110 .., and [dest+8] = 0x100 is the RETURN SLOT (becomes 1). The B argument
     * is read after the A stores, so the B pointer becomes A[2]. */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0xA0A0A0A0u);
    guest_write32(T1479_349390_A + 4u, 0xA1A1A1A1u);
    guest_write32(T1479_349390_A + 8u, T1479_349390_B + 0x40u); /* A[2] doubles as the B pointer */
    guest_write32(T1479_349390_B + 0x40u, 0xB0B0B0B0u);
    guest_write32(T1479_349390_B + 0x44u, 0xB1B1B1B1u);
    guest_write32(T1479_349390_B + 0x48u, 0xB2B2B2B2u);
    t1479_349390_run(0x100u, 0xF8u, T1479_349390_A, 0x7777777u /* replaced by the A[2] store */);
    CHECK(guest_read32(0x100u) == 1u);                      /* return slot overwritten by [dest+8] = 1 */
    CHECK(guest_read32(0x104u) == 0xA0A0A0A0u);             /* dest slot := A[0] */
    CHECK(guest_read32(0x108u) == 0xA1A1A1A1u);             /* A slot := A[1] */
    CHECK(guest_read32(0x10Cu) == T1479_349390_B + 0x40u);  /* B slot := A[2] */
    CHECK(guest_read32(0xF8u + 0x18u) == 0xB0B0B0B0u);      /* 0x110 */
    CHECK(guest_read32(0xF8u + 0x1Cu) == 0xB1B1B1B1u);
    CHECK(guest_read32(0xF8u + 0x20u) == 0xB2B2B2B2u);
    CHECK(guest_read32(0xF8u + 0x38u) == 0u);
    CHECK(g_ecx == 0xB2B2B2B2u && g_edx == 0xB1B1B1B1u);

    /* (a) dest = 0xFC: [dest+0xC] = 0x108 (A slot := A0), [dest+0x10] = 0x10C (B slot := A1) so B = A1,
     * and [dest+8] = 0x104 (the dest slot, after it was read) := 1 */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0xA0A0A0A0u);
    guest_write32(T1479_349390_A + 4u, T1479_349390_B + 0x80u);
    guest_write32(T1479_349390_A + 8u, 0xA2A2A2A2u);
    guest_write32(T1479_349390_B + 0x80u, 0xC0C0C0C0u);
    guest_write32(T1479_349390_B + 0x84u, 0xC1C1C1C1u);
    guest_write32(T1479_349390_B + 0x88u, 0xC2C2C2C2u);
    t1479_349390_run(0x100u, 0xFCu, T1479_349390_A, 0x5555555u);
    CHECK(guest_read32(0x104u) == 1u);
    CHECK(guest_read32(0x108u) == 0xA0A0A0A0u);
    CHECK(guest_read32(0x10Cu) == T1479_349390_B + 0x80u);
    CHECK(guest_read32(0x110u) == 0xA2A2A2A2u);
    CHECK(guest_read32(0x114u) == 0xC0C0C0C0u && guest_read32(0x118u) == 0xC1C1C1C1u);
    CHECK(guest_read32(0x11Cu) == 0xC2C2C2C2u);
    CHECK(g_ecx == 0xC2C2C2C2u && g_edx == 0xC1C1C1C1u);
    CHECK(guest_read32(0x100u) == 0x12345678u);

    /* (a) dest = 0x104 + ... : dest+0x14 = B slot exactly: dest = 0xF8 handled, dest = 0xE4+... below: the
     * [dest+0x38] store (movss) hits the B slot: dest = 0x10C - 0x38 = 0xD4 stores 0 into the B slot AFTER
     * B was read, [dest+8] = 0xDC, and [dest+0x14] = 0xE8. The B pointer is untouched when read. */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0x71u);
    guest_write32(T1479_349390_A + 4u, 0x72u);
    guest_write32(T1479_349390_A + 8u, 0x73u);
    guest_write32(T1479_349390_B, 0x81u);
    guest_write32(T1479_349390_B + 4u, 0x82u);
    guest_write32(T1479_349390_B + 8u, 0x83u);
    t1479_349390_run(0x100u, 0xD4u, T1479_349390_A, T1479_349390_B);
    CHECK(guest_read32(0x10Cu) == 0u);                      /* movss store after the B read */
    CHECK(guest_read32(0xD4u + 0x18u) == 0x81u && guest_read32(0xD4u + 0x1Cu) == 0x82u);
    CHECK(guest_read32(0xD4u + 0x20u) == 0x83u && guest_read32(0xD4u + 0x08u) == 1u);
    CHECK(g_ecx == 0x83u && g_edx == 0x82u);

    /* A overlapping dest: A = dest + 8, so the first store (+0xC = A[1]) feeds the next read of A[1] */
    t1479_349390_reset();
    guest_write32(T1479_349390_DEST + 8u, 0x0A0A0A0Au);
    guest_write32(T1479_349390_DEST + 0x0Cu, 0x0B0B0B0Bu);
    guest_write32(T1479_349390_DEST + 0x10u, 0x0C0C0C0Cu);
    guest_write32(T1479_349390_B, 0xE0E0E0E0u);
    guest_write32(T1479_349390_B + 4u, 0xE1E1E1E1u);
    guest_write32(T1479_349390_B + 8u, 0xE2E2E2E2u);
    t1479_349390_run(0x100u, T1479_349390_DEST, T1479_349390_DEST + 8u, T1479_349390_B);
    /* A0 = [dest+8] = 0x0A0A0A0A is copied to +0xC; A1 is then [dest+0xC] = A0, copied to +0x10; A2 =
     * [dest+0x10] = A0 copied to +0x14. Finally [dest+8] := 1. */
    CHECK(guest_read32(T1479_349390_DEST + 0x0Cu) == 0x0A0A0A0Au);
    CHECK(guest_read32(T1479_349390_DEST + 0x10u) == 0x0A0A0A0Au);
    CHECK(guest_read32(T1479_349390_DEST + 0x14u) == 0x0A0A0A0Au);
    CHECK(guest_read32(T1479_349390_DEST + 0x08u) == 1u);
    CHECK(g_ecx == 0xE2E2E2E2u && g_edx == 0xE1E1E1E1u);

    /* A = dest + 4: A2 = [dest+0xC] is read AFTER the +0xC store of A0 = [dest+4] */
    t1479_349390_reset();
    guest_write32(T1479_349390_DEST + 4u, 0x1D1D1D1Du);
    guest_write32(T1479_349390_DEST + 8u, 0x2D2D2D2Du);
    guest_write32(T1479_349390_DEST + 0x0Cu, 0x3D3D3D3Du);
    guest_write32(T1479_349390_B, 1u);
    guest_write32(T1479_349390_B + 4u, 2u);
    guest_write32(T1479_349390_B + 8u, 3u);
    t1479_349390_run(0x100u, T1479_349390_DEST, T1479_349390_DEST + 4u, T1479_349390_B);
    CHECK(guest_read32(T1479_349390_DEST + 0x0Cu) == 0x1D1D1D1Du);
    CHECK(guest_read32(T1479_349390_DEST + 0x10u) == 0x2D2D2D2Du);
    CHECK(guest_read32(T1479_349390_DEST + 0x14u) == 0x1D1D1D1Du); /* A2 read the just-stored A0 */
    CHECK(guest_read32(T1479_349390_DEST + 0x04u) == 0x1D1D1D1Du && guest_read32(T1479_349390_DEST + 0x08u) == 1u);
    CHECK(g_ecx == 3u && g_edx == 2u);

    /* B overlapping dest: B = dest + 0x10 reads values the A copy wrote (B0 = A1, B1 = A2) and B2
     * is the dword just stored at +0x18 */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0xF0F0F0F0u);
    guest_write32(T1479_349390_A + 4u, 0xF1F1F1F1u);
    guest_write32(T1479_349390_A + 8u, 0xF2F2F2F2u);
    t1479_349390_run(0x100u, T1479_349390_DEST, T1479_349390_A, T1479_349390_DEST + 0x10u);
    CHECK(guest_read32(T1479_349390_DEST + 0x18u) == 0xF1F1F1F1u);
    CHECK(guest_read32(T1479_349390_DEST + 0x1Cu) == 0xF2F2F2F2u);
    CHECK(guest_read32(T1479_349390_DEST + 0x20u) == 0xF1F1F1F1u); /* B2 = [dest+0x18] just stored */
    CHECK(g_ecx == 0xF1F1F1F1u && g_edx == 0xF2F2F2F2u);

    /* B = dest + 4: B0 = [dest+4] (garbage), B1 = [dest+8] (garbage; +8 is only set to 1 at the end),
     * B2 = [dest+0xC] = A0 stored first */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0x91u);
    guest_write32(T1479_349390_A + 4u, 0x92u);
    guest_write32(T1479_349390_A + 8u, 0x93u);
    t1479_349390_run(0x100u, T1479_349390_DEST, T1479_349390_A, T1479_349390_DEST + 4u);
    CHECK(guest_read32(T1479_349390_DEST + 0x18u) == t1479_349390_dest_garbage + 4u);
    CHECK(guest_read32(T1479_349390_DEST + 0x1Cu) == t1479_349390_dest_garbage + 8u);
    CHECK(guest_read32(T1479_349390_DEST + 0x20u) == 0x91u);
    CHECK(g_ecx == 0x91u && g_edx == t1479_349390_dest_garbage + 8u);

    /* dest == A == B: reads 0..8, writes at +0xC.. so nothing overlaps */
    t1479_349390_reset();
    t1479_349390_run(0x100u, T1479_349390_DEST, T1479_349390_DEST, T1479_349390_DEST);
    CHECK(guest_read32(T1479_349390_DEST + 0x0Cu) == t1479_349390_dest_garbage + 0u);
    CHECK(guest_read32(T1479_349390_DEST + 0x14u) == t1479_349390_dest_garbage + 8u);
    CHECK(guest_read32(T1479_349390_DEST + 0x18u) == t1479_349390_dest_garbage + 0u);
    CHECK(guest_read32(T1479_349390_DEST + 0x20u) == t1479_349390_dest_garbage + 8u);

    /* 32 bit address wrap: dest = 0xFFFFFFFC puts +8 at 4, +0xC at 8, .., +0x20 at 0x1C, +0x38 at 0x34 */
    t1479_349390_reset();
    guest_write32(T1479_349390_A, 0x61u);
    guest_write32(T1479_349390_A + 4u, 0x62u);
    guest_write32(T1479_349390_A + 8u, 0x63u);
    guest_write32(T1479_349390_B, 0x64u);
    guest_write32(T1479_349390_B + 4u, 0x65u);
    guest_write32(T1479_349390_B + 8u, 0x66u);
    guest_write32(0x34u, 0xEEEEEEEEu);
    t1479_349390_run(0x100u, 0xFFFFFFFCu, T1479_349390_A, T1479_349390_B);
    CHECK(guest_read32(0x4u) == 1u && guest_read32(0x8u) == 0x61u && guest_read32(0xCu) == 0x62u);
    CHECK(guest_read32(0x10u) == 0x63u && guest_read32(0x14u) == 0x64u && guest_read32(0x18u) == 0x65u);
    CHECK(guest_read32(0x1Cu) == 0x66u && guest_read32(0x34u) == 0u);
    CHECK(g_ecx == 0x66u && g_edx == 0x65u);
}
