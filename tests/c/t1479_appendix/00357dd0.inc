/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x00357DD0 (legacy vector mode, cdecl, 4 stack args). Expected values derived from the
 * ORIGINAL instruction listing (args: owner=[esp+4], record=[esp+8], tag=[esp+0xC], vec=[esp+0x10]):
 *   eax=record ; test eax,eax ; je END ;  ecx=owner ; xorps xmm0,xmm0 ; [eax+0x158]=ecx ; or [ecx+0x28],0x10000 ;
 *   ecx=[esp+0xC] ; [eax+0x15C]=ecx ; ecx=[esp+0x10] ; movss [eax+0x64],[eax+0x68],[eax+0x6C] = xmm0 ;
 *   edx=[ecx] ; [eax+0x1C]=edx ; edx=[ecx+4] ; [eax+0x20]=edx ; ecx=[ecx+8] ; [eax+0x24]=ecx ;
 *   ecx=[eax+0xA8] ; test ecx,ecx ; je END ;
 *   edx=[eax+0x1C] ; [ecx+0x64]=edx ; ecx=[eax+0xA8] ; edx=[eax+0x20] ; [ecx+0x68]=edx ;
 *   ecx=[eax+0xA8] ; edx=[eax+0x24] ; [ecx+0x6C]=edx ; END: ret
 * Exit: record==0 -> eax=0, ecx/edx/xmm0 untouched, no memory access (owner and vec are not dereferenced);
 * [record+0xA8]==0 -> eax=record, ecx=0, edx=[vec+4] as read, xmm0=0; else eax=record, ecx=[record+0xA8] (re-read),
 * edx=[record+0x24] (re-read), xmm0={0,0,0,0}. ebx/esi/edi/ebp untouched. Both je tests are equality, so
 * there is no signed/unsigned boundary, values 1, 0xFFFFFFFF and 0xFFFFFF9C cover non-null extremes of the
 * pointer P (address arithmetic wraps at 32 bits).
 * No guest PUSH or POP in the original, so there is no saved-register slot to alias (cases b, c do not
 * exist). Alias (a) is covered by many cases: owner on the tag slot (the or changes the tag that is read
 * later), owner+0x28 on [record+0x158] and [record+0x15C], record+0x158 on the vec slot, tag slot and
 * record slot, record+0x1C..0x24 on the return and argument slots, record+0x64..0x6C (movss) on the tag and
 * vec slots, vec overlapping record+0x1C (the copy reads what it just wrote), P overlapping the record
 * (P+0x64 == record+0xA8 re-pointing the mirror, chain, P==record, P+0x64 == record+0x20) and P on the stack.
 * MUTANT: g_edx = guest_read32(g_eax + 0x20u); -> g_edx = guest_read32(g_ecx + 0x20u);
 * MUTANT: g_edx = guest_read32(g_eax + 0x24u); -> g_edx = guest_read32(g_ecx + 0x24u);
 */

extern __thread uint32_t t1479_357dd0_xmm0[4] __asm__("g_xmm0");

#define T1479_357DD0_O 0x20000u
#define T1479_357DD0_R 0x21000u
#define T1479_357DD0_V 0x22000u
#define T1479_357DD0_P 0x23000u
#define T1479_357DD0_Q 0x24000u
#define T1479_357DD0_W 0x25000u
#define T1479_357DD0_STK 0x40000u

static void t1479_357dd0_reset(void)
{
    memset(&memory[0], 0, 0x220);
    memset(&memory[0x20000], 0, 0x6000);
    memset(&memory[0x3FD00], 0, 0x500);
    for (uint32_t off = 0; off < 0x180; off += 4u) guest_write32(T1479_357DD0_R + off, 0xC0C00000u + off);
    guest_write32(T1479_357DD0_R + 0xA8u, 0u); /* linked pointer null unless a case sets it */
    for (uint32_t off = 0; off < 0x60; off += 4u) guest_write32(T1479_357DD0_O + off, 0xD0D00000u + off);
    for (uint32_t off = 0; off < 0x80; off += 4u) guest_write32(T1479_357DD0_P + off, 0xE0E00000u + off);
}

static void t1479_357dd0_run(uint32_t esp, uint32_t owner, uint32_t record, uint32_t tag, uint32_t vec)
{
    prepare();
    g_esp = esp;
    guest_write32(esp, 0x12345678u);
    guest_write32(esp + 4u, owner);
    guest_write32(esp + 8u, record);
    guest_write32(esp + 12u, tag);
    guest_write32(esp + 16u, vec);
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    for (unsigned lane = 0; lane < 4; ++lane) t1479_357dd0_xmm0[lane] = 0xDEADBE00u + lane;
    invoke(0x00357DD0u, 4u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
}

static void t1479_357dd0_xmm_cleared(void)
{
    CHECK(t1479_357dd0_xmm0[0] == 0u && t1479_357dd0_xmm0[1] == 0u);
    CHECK(t1479_357dd0_xmm0[2] == 0u && t1479_357dd0_xmm0[3] == 0u);
}

static void t1479_357dd0_xmm_kept(void)
{
    CHECK(t1479_357dd0_xmm0[0] == 0xDEADBE00u && t1479_357dd0_xmm0[1] == 0xDEADBE01u);
    CHECK(t1479_357dd0_xmm0[2] == 0xDEADBE02u && t1479_357dd0_xmm0[3] == 0xDEADBE03u);
}

/* the record dwords the function may change; every other dword of the record window keeps its garbage */
static int t1479_357dd0_is_record_field(uint32_t off)
{
    return off == 0x1Cu || off == 0x20u || off == 0x24u || off == 0x64u || off == 0x68u || off == 0x6Cu ||
           off == 0x158u || off == 0x15Cu || off == 0xA8u;
}

static void t1479_357dd0_check_standard(uint32_t owner_old28, uint32_t tag, const uint32_t v[3])
{
    const uint32_t R = T1479_357DD0_R;
    CHECK(guest_read32(R + 0x158u) == T1479_357DD0_O);
    CHECK(guest_read32(T1479_357DD0_O + 0x28u) == (owner_old28 | 0x10000u));
    CHECK(guest_read32(R + 0x15Cu) == tag);
    CHECK(guest_read32(R + 0x64u) == 0u && guest_read32(R + 0x68u) == 0u && guest_read32(R + 0x6Cu) == 0u);
    CHECK(guest_read32(R + 0x1Cu) == v[0] && guest_read32(R + 0x20u) == v[1] && guest_read32(R + 0x24u) == v[2]);
    for (uint32_t off = 0; off < 0x180; off += 4u) {
        if (!t1479_357dd0_is_record_field(off)) CHECK(guest_read32(R + off) == 0xC0C00000u + off);
    }
    for (uint32_t off = 0; off < 0x60; off += 4u) {
        if (off != 0x28u) CHECK(guest_read32(T1479_357DD0_O + off) == 0xD0D00000u + off);
    }
    CHECK(guest_read32(0x100u) == 0x12345678u && guest_read32(0x104u) == T1479_357DD0_O);
    CHECK(guest_read32(0x108u) == R && guest_read32(0x10Cu) == tag && guest_read32(0x110u) == T1479_357DD0_V);
    CHECK(guest_read32(T1479_357DD0_V) == v[0] && guest_read32(T1479_357DD0_V + 4u) == v[1]);
    CHECK(guest_read32(T1479_357DD0_V + 8u) == v[2]);
}

static void test_t1479_appendix_00357dd0(void)
{
    const uint32_t R = T1479_357DD0_R;
    const uint32_t O = T1479_357DD0_O;
    const uint32_t V = T1479_357DD0_V;
    const uint32_t P = T1479_357DD0_P;
    const uint32_t S = T1479_357DD0_STK;

    /* record == 0: nothing is read or written, eax = 0, ecx/edx/xmm0 kept, owner and vec never dereferenced */
    static const uint32_t null_owner[] = {0x20000u, 0xFFFFFFFFu, 0u};
    for (unsigned k = 0; k < 3; k++) {
        t1479_357dd0_reset();
        t1479_357dd0_run(0x100u, null_owner[k], 0u, 0x1234u, k == 0 ? V : 0xFFFFFFFFu);
        CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
        t1479_357dd0_xmm_kept();
        CHECK(guest_read32(O + 0x28u) == 0xD0D00028u);
        for (uint32_t off = 0; off < 0x180; off += 4u) CHECK(guest_read32(R + off) == (off == 0xA8u ? 0u : 0xC0C00000u + off));
        CHECK(guest_read32(0x104u) == null_owner[k] && guest_read32(0x108u) == 0u);
        CHECK(guest_read32(0x28u) == 0u);
    }

    /* standard path, linked pointer null: eax = record, ecx = 0, edx = v[1], xmm0 cleared */
    static const uint32_t tags[] = {0u, 0x1234u, 0xFFFFFFFFu, 0x80000000u};
    static const uint32_t olds[] = {0u, 0xFFFFu, 0x10000u, 0xFFFFFFFFu, 0x80000000u};
    static const uint32_t vecs[3][3] = {
        {0x11111111u, 0x22222222u, 0x33333333u},
        {0x00000000u, 0x80000000u, 0x7FC00001u},
        {0xFFFFFFFFu, 0x3F800000u, 0xFF800000u},
    };
    for (unsigned t = 0; t < 4; t++) {
        for (unsigned o = 0; o < 5; o++) {
            for (unsigned v = 0; v < 3; v++) {
                t1479_357dd0_reset();
                guest_write32(O + 0x28u, olds[o]);
                for (unsigned k = 0; k < 3; k++) guest_write32(V + 4u * k, vecs[v][k]);
                guest_write32(R + 0xA8u, 0u);
                t1479_357dd0_run(0x100u, O, R, tags[t], V);
                CHECK(g_eax == R && g_ecx == 0u && g_edx == vecs[v][1]);
                t1479_357dd0_xmm_cleared();
                t1479_357dd0_check_standard(olds[o], tags[t], vecs[v]);
                CHECK(guest_read32(R + 0xA8u) == 0u);
            }
        }
    }

    /* linked pointer non-null: mirror the vector at P+0x64..0x6C, ecx = P, edx = v[2] */
    static const uint32_t pointers[] = {0x23000u, 0x1u, 0xFFFFFFFFu, 0xFFFFFF9Cu, 0x4C0000u};
    for (unsigned q = 0; q < 5; q++) {
        for (unsigned v = 0; v < 3; v++) {
            const uint32_t linked = pointers[q];
            t1479_357dd0_reset();
            guest_write32(O + 0x28u, 0x4u);
            for (unsigned k = 0; k < 3; k++) guest_write32(V + 4u * k, vecs[v][k]);
            guest_write32(R + 0xA8u, linked);
            t1479_357dd0_run(0x100u, O, R, 0x77u, V);
            CHECK(g_eax == R && g_ecx == linked && g_edx == vecs[v][2]);
            t1479_357dd0_xmm_cleared();
            CHECK(guest_read32(linked + 0x64u) == vecs[v][0]);
            CHECK(guest_read32(linked + 0x68u) == vecs[v][1]);
            CHECK(guest_read32(linked + 0x6Cu) == vecs[v][2]);
            CHECK(guest_read32(R + 0xA8u) == linked);
            CHECK(guest_read32(R + 0x1Cu) == vecs[v][0] && guest_read32(R + 0x24u) == vecs[v][2]);
            CHECK(guest_read32(R + 0x64u) == 0u && guest_read32(R + 0x68u) == 0u && guest_read32(R + 0x6Cu) == 0u);
            CHECK(guest_read32(R + 0x158u) == O && guest_read32(R + 0x15Cu) == 0x77u);
            CHECK(guest_read32(O + 0x28u) == 0x10004u);
            if (linked == P) {
                for (uint32_t off = 0; off < 0x80; off += 4u) {
                    if (off != 0x64u && off != 0x68u && off != 0x6Cu) CHECK(guest_read32(P + off) == 0xE0E00000u + off);
                }
            }
        }
    }

    /* (a) owner on the tag argument slot: owner+0x28 == esp+0xC, the or changes the tag read afterwards */
    t1479_357dd0_reset();
    guest_write32(V, 1u);
    guest_write32(V + 4u, 2u);
    guest_write32(V + 8u, 3u);
    t1479_357dd0_run(0x100u, 0xE4u, R, 0x1234u, V);
    CHECK(guest_read32(0x10Cu) == 0x11234u);
    CHECK(guest_read32(R + 0x15Cu) == 0x11234u && guest_read32(R + 0x158u) == 0xE4u);
    CHECK(g_eax == R && g_ecx == 0u && g_edx == 2u);

    /* owner + 0x28 == record + 0x158: the or applies to the owner pointer just stored there */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    guest_write32(V, 1u);
    guest_write32(V + 4u, 2u);
    guest_write32(V + 8u, 3u);
    t1479_357dd0_run(0x100u, R + 0x130u, R, 0x55u, V);
    CHECK(guest_read32(R + 0x158u) == ((R + 0x130u) | 0x10000u));
    CHECK(guest_read32(R + 0x15Cu) == 0x55u);
    CHECK(g_eax == R && g_ecx == 0u && g_edx == 2u);

    /* owner + 0x28 == record + 0x15C: the or lands first, then the tag store replaces it */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    guest_write32(R + 0x15Cu, 0x00F00000u);
    guest_write32(V, 1u);
    guest_write32(V + 4u, 2u);
    guest_write32(V + 8u, 3u);
    t1479_357dd0_run(0x100u, R + 0x134u, R, 0x66u, V);
    CHECK(guest_read32(R + 0x158u) == R + 0x134u);
    CHECK(guest_read32(R + 0x15Cu) == 0x66u);

    /* owner == record */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    guest_write32(V, 7u);
    guest_write32(V + 4u, 8u);
    guest_write32(V + 8u, 9u);
    t1479_357dd0_run(0x100u, R, R, 0x44u, V);
    CHECK(guest_read32(R + 0x158u) == R && guest_read32(R + 0x28u) == (0xC0C00028u | 0x10000u));
    CHECK(guest_read32(R + 0x1Cu) == 7u && guest_read32(R + 0x24u) == 9u && g_edx == 8u && g_ecx == 0u);

    /* stack at S: ret S, owner S+4, record S+8, tag S+0xC, vec S+0x10. record + 0x158 == vec slot:
     * the owner is stored into the vec slot before it is read, so the vector comes from the owner */
    t1479_357dd0_reset();
    guest_write32(O, 0xA0A0A0A0u);
    guest_write32(O + 4u, 0xA1A1A1A1u);
    guest_write32(O + 8u, 0xA2A2A2A2u);
    guest_write32(S - 0x148u + 0xA8u, 0u);
    t1479_357dd0_run(S, O, S - 0x148u, 0x99u, V);
    CHECK(guest_read32(S + 0x10u) == O);
    CHECK(guest_read32(S - 0x148u + 0x1Cu) == 0xA0A0A0A0u);
    CHECK(guest_read32(S - 0x148u + 0x20u) == 0xA1A1A1A1u);
    CHECK(guest_read32(S - 0x148u + 0x24u) == 0xA2A2A2A2u);
    CHECK(guest_read32(S + 0x14u) == 0x99u && guest_read32(O + 0x28u) == (0xD0D00028u | 0x10000u));
    CHECK(g_eax == S - 0x148u && g_ecx == 0u && g_edx == 0xA1A1A1A1u);
    t1479_357dd0_xmm_cleared();

    /* record + 0x158 == tag slot: the tag read sees the owner, which is stored to the vec slot
     * (record + 0x15C), so the vector pointer is the owner too */
    t1479_357dd0_reset();
    guest_write32(O, 0xB0B0B0B0u);
    guest_write32(O + 4u, 0xB1B1B1B1u);
    guest_write32(O + 8u, 0xB2B2B2B2u);
    guest_write32(S - 0x14Cu + 0xA8u, 0u);
    t1479_357dd0_run(S, O, S - 0x14Cu, 0x98u, V);
    CHECK(guest_read32(S + 0xCu) == O && guest_read32(S + 0x10u) == O);
    CHECK(guest_read32(S - 0x14Cu + 0x1Cu) == 0xB0B0B0B0u && guest_read32(S - 0x14Cu + 0x24u) == 0xB2B2B2B2u);
    CHECK(g_eax == S - 0x14Cu && g_ecx == 0u && g_edx == 0xB1B1B1B1u);

    /* record + 0x158 == record slot: the record argument is overwritten with the owner after it was read */
    t1479_357dd0_reset();
    guest_write32(V, 0xC0u);
    guest_write32(V + 4u, 0xC1u);
    guest_write32(V + 8u, 0xC2u);
    guest_write32(S - 0x150u + 0xA8u, 0u);
    t1479_357dd0_run(S, O, S - 0x150u, 0x97u, V);
    CHECK(guest_read32(S + 8u) == O && guest_read32(S + 0xCu) == 0x97u);
    CHECK(guest_read32(S - 0x150u + 0x1Cu) == 0xC0u && g_eax == S - 0x150u && g_edx == 0xC1u);

    /* record + 0x1C..0x24 on the return and argument slots: stored after they were read */
    t1479_357dd0_reset();
    guest_write32(V, 0xD0u);
    guest_write32(V + 4u, 0xD1u);
    guest_write32(V + 8u, 0xD2u);
    guest_write32(P + 0x60u, 0u);
    guest_write32(S - 0x1Cu + 0xA8u, P);
    t1479_357dd0_run(S, O, S - 0x1Cu, 0x96u, V);
    CHECK(guest_read32(S) == 0xD0u && guest_read32(S + 4u) == 0xD1u && guest_read32(S + 8u) == 0xD2u);
    CHECK(guest_read32(P + 0x64u) == 0xD0u && guest_read32(P + 0x68u) == 0xD1u && guest_read32(P + 0x6Cu) == 0xD2u);
    CHECK(g_eax == S - 0x1Cu && g_ecx == P && g_edx == 0xD2u);

    /* movss stores on the tag and vec slots (record + 0x64 == tag slot, 0x68 == vec slot, 0x6C == S+0x14):
     * both arguments were read before, so the copy is unaffected */
    t1479_357dd0_reset();
    guest_write32(V, 0xE0u);
    guest_write32(V + 4u, 0xE1u);
    guest_write32(V + 8u, 0xE2u);
    guest_write32(S - 0x58u + 0xA8u, 0u);
    guest_write32(S + 0x14u, 0x7777u);
    t1479_357dd0_run(S, O, S - 0x58u, 0x95u, V);
    CHECK(guest_read32(S + 0xCu) == 0u && guest_read32(S + 0x10u) == 0u && guest_read32(S + 0x14u) == 0u);
    CHECK(guest_read32(S - 0x58u + 0x15Cu) == 0x95u);
    CHECK(guest_read32(S - 0x58u + 0x1Cu) == 0xE0u && guest_read32(S - 0x58u + 0x24u) == 0xE2u && g_edx == 0xE1u);

    /* vec overlaps record + 0x18: every copied dword equals the first one */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    guest_write32(R + 0x18u, 0xABCD0018u);
    guest_write32(R + 0x1Cu, 0x11u);
    guest_write32(R + 0x20u, 0x22u);
    t1479_357dd0_run(0x100u, O, R, 0x33u, R + 0x18u);
    CHECK(guest_read32(R + 0x1Cu) == 0xABCD0018u && guest_read32(R + 0x20u) == 0xABCD0018u);
    CHECK(guest_read32(R + 0x24u) == 0xABCD0018u && g_edx == 0xABCD0018u && g_ecx == 0u);
    CHECK(guest_read32(R + 0x18u) == 0xABCD0018u);

    /* vec overlaps record + 0x14: the third dword is read after the first store */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    guest_write32(R + 0x14u, 0x0A140014u);
    guest_write32(R + 0x18u, 0x0B180018u);
    guest_write32(R + 0x1Cu, 0x0C1C001Cu);
    t1479_357dd0_run(0x100u, O, R, 0x33u, R + 0x14u);
    CHECK(guest_read32(R + 0x1Cu) == 0x0A140014u && guest_read32(R + 0x20u) == 0x0B180018u);
    CHECK(guest_read32(R + 0x24u) == 0x0A140014u && g_edx == 0x0B180018u);

    /* vec overlaps record + 0x20: plain shifted copy, no earlier-written dword is re-read */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    guest_write32(R + 0x20u, 0x20202020u);
    guest_write32(R + 0x24u, 0x24242424u);
    guest_write32(R + 0x28u, 0x28282828u);
    t1479_357dd0_run(0x100u, O, R, 0x33u, R + 0x20u);
    CHECK(guest_read32(R + 0x1Cu) == 0x20202020u && guest_read32(R + 0x20u) == 0x24242424u);
    CHECK(guest_read32(R + 0x24u) == 0x28282828u && guest_read32(R + 0x28u) == 0x28282828u);

    /* P = record + 0x44: P + 0x64 == record + 0xA8, so the first mirror store re-points the pointer to Q (= v[0]),
     * the following mirror stores go to Q + 0x68 and Q + 0x6C, never to P + 0x68 */
    t1479_357dd0_reset();
    guest_write32(V, T1479_357DD0_Q);
    guest_write32(V + 4u, 0x5151u);
    guest_write32(V + 8u, 0x5252u);
    guest_write32(R + 0xA8u, R + 0x44u);
    guest_write32(R + 0xACu, 0x5A5A5A5Au);
    t1479_357dd0_run(0x100u, O, R, 0x33u, V);
    CHECK(guest_read32(R + 0xA8u) == T1479_357DD0_Q);
    CHECK(guest_read32(T1479_357DD0_Q + 0x68u) == 0x5151u && guest_read32(T1479_357DD0_Q + 0x6Cu) == 0x5252u);
    CHECK(guest_read32(T1479_357DD0_Q + 0x64u) == 0u);
    CHECK(guest_read32(R + 0xACu) == 0x5A5A5A5Au && guest_read32(R + 0xB0u) == 0xC0C000B0u);
    CHECK(g_eax == R && g_ecx == T1479_357DD0_Q && g_edx == 0x5252u);

    /* chain: v[0] = record + 0x40 and v[1] = W; the second mirror store (Q + 0x68 == record + 0xA8) re-points
     * the pointer again to W, the third goes to W + 0x6C */
    t1479_357dd0_reset();
    guest_write32(V, R + 0x40u);
    guest_write32(V + 4u, T1479_357DD0_W);
    guest_write32(V + 8u, 0x6262u);
    guest_write32(R + 0xA8u, R + 0x44u);
    t1479_357dd0_run(0x100u, O, R, 0x33u, V);
    CHECK(guest_read32(R + 0xA8u) == T1479_357DD0_W);
    CHECK(guest_read32(T1479_357DD0_W + 0x6Cu) == 0x6262u && guest_read32(T1479_357DD0_W + 0x68u) == 0u);
    CHECK(guest_read32(T1479_357DD0_W + 0x64u) == 0u);
    CHECK(g_ecx == T1479_357DD0_W && g_edx == 0x6262u && g_eax == R);

    /* P == record: the mirror stores overwrite the zeroed floats with the vector */
    t1479_357dd0_reset();
    guest_write32(V, 0x71u);
    guest_write32(V + 4u, 0x72u);
    guest_write32(V + 8u, 0x73u);
    guest_write32(R + 0xA8u, R);
    t1479_357dd0_run(0x100u, O, R, 0x33u, V);
    CHECK(guest_read32(R + 0x64u) == 0x71u && guest_read32(R + 0x68u) == 0x72u && guest_read32(R + 0x6Cu) == 0x73u);
    CHECK(g_ecx == R && g_edx == 0x73u);

    /* P = record - 0x44: P + 0x64 == record + 0x20, so the later source re-reads see the stored v[0] */
    t1479_357dd0_reset();
    guest_write32(V, 0x81u);
    guest_write32(V + 4u, 0x82u);
    guest_write32(V + 8u, 0x83u);
    guest_write32(R + 0xA8u, R - 0x44u);
    t1479_357dd0_run(0x100u, O, R, 0x33u, V);
    CHECK(guest_read32(R + 0x1Cu) == 0x81u && guest_read32(R + 0x20u) == 0x81u);
    CHECK(guest_read32(R + 0x24u) == 0x81u && guest_read32(R + 0x28u) == 0x81u);
    CHECK(g_ecx == R - 0x44u && g_edx == 0x81u);

    /* P = record - 0x48: the mirror stores rewrite the same values into the vector fields */
    t1479_357dd0_reset();
    guest_write32(V, 0x91u);
    guest_write32(V + 4u, 0x92u);
    guest_write32(V + 8u, 0x93u);
    guest_write32(R + 0xA8u, R - 0x48u);
    t1479_357dd0_run(0x100u, O, R, 0x33u, V);
    CHECK(guest_read32(R + 0x1Cu) == 0x91u && guest_read32(R + 0x20u) == 0x92u && guest_read32(R + 0x24u) == 0x93u);
    CHECK(g_ecx == R - 0x48u && g_edx == 0x93u);

    /* P on the stack: P + 0x64 == return slot, P + 0x68 == owner slot, P + 0x6C == record slot (after use) */
    t1479_357dd0_reset();
    guest_write32(V, 0xF0u);
    guest_write32(V + 4u, 0xF1u);
    guest_write32(V + 8u, 0xF2u);
    guest_write32(R + 0xA8u, 0x9Cu);
    t1479_357dd0_run(0x100u, O, R, 0x33u, V);
    CHECK(guest_read32(0x100u) == 0xF0u && guest_read32(0x104u) == 0xF1u && guest_read32(0x108u) == 0xF2u);
    CHECK(g_eax == R && g_ecx == 0x9Cu && g_edx == 0xF2u);

    /* vec pointer on the argument slots themselves: the copy gets owner, record, tag */
    t1479_357dd0_reset();
    guest_write32(R + 0xA8u, 0u);
    t1479_357dd0_run(0x100u, O, R, 0x4242u, 0x104u);
    CHECK(guest_read32(R + 0x1Cu) == O && guest_read32(R + 0x20u) == R && guest_read32(R + 0x24u) == 0x4242u);
    CHECK(g_edx == R && g_ecx == 0u);
}
