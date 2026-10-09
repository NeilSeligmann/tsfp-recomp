/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1623 native tests for the 7 admitted roots of src/game/game_fp_list001.c (included by
 * test_game_replacements.c after test_game_vec_list001.inc, which declares VecTestXmm, the
 * g_xmm0 extern, vec_begin and the sentinel lane pattern).
 * Expectations are hand-derived from the retail disassembly (GPR outputs, memory, raw XMM0 and
 * XMM1 lanes) with exact binary32 values, not by running the replacement against itself. XMM0
 * and XMM1 are seeded with nonzero lane patterns by fp_begin so lane zeroing by `movss xmm,
 * m32` and the full clear by `xorps` are observable. binary32 bits used: 0.0 0x00000000,
 * -0.0 0x80000000, 1.0 0x3F800000, 2.0 0x40000000, 3.0 0x40400000, 4.0 0x40800000,
 * 5.0 0x40A00000, 6.0 0x40C00000, -1.0 0xBF800000, qNaN 0x7FC00000. */

extern __thread VecTestXmm g_xmm1;

#define FP_F0 0x00000000u
#define FP_NEG0 0x80000000u
#define FP_F1 0x3F800000u
#define FP_F2 0x40000000u
#define FP_F3 0x40400000u
#define FP_F4 0x40800000u
#define FP_F5 0x40A00000u
#define FP_F6 0x40C00000u
#define FP_NEG1 0xBF800000u
#define FP_NAN 0x7FC00000u

static void fp_begin(void)
{
    vec_begin();
    for (unsigned lane = 0u; lane < 4u; lane++) g_xmm1.u[lane] = 0xB6B6B600u + lane;
}

static void fp_check_gprs(uint32_t eax, uint32_t ecx, uint32_t edx)
{
    CHECK(g_eax == eax && g_ecx == ecx && g_edx == edx);
    CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u && g_esi == 0x51515151u &&
          g_edi == 0xD1D1D1D1u);
}

/* xmm0 / xmm1 hold `low` in lane 0 and zeros above. */
static void fp_check_xmm0(uint32_t low)
{
    CHECK(g_xmm0.u[0] == low && g_xmm0.u[1] == 0u && g_xmm0.u[2] == 0u && g_xmm0.u[3] == 0u);
}

static void fp_check_xmm1(uint32_t low)
{
    CHECK(g_xmm1.u[0] == low && g_xmm1.u[1] == 0u && g_xmm1.u[2] == 0u && g_xmm1.u[3] == 0u);
}

static void fp_check_xmm1_untouched(void)
{
    CHECK(g_xmm1.u[0] == 0xB6B6B600u && g_xmm1.u[1] == 0xB6B6B601u &&
          g_xmm1.u[2] == 0xB6B6B602u && g_xmm1.u[3] == 0xB6B6B603u);
}

static void fp_check_xmm0_untouched(void)
{
    CHECK(g_xmm0.u[0] == 0xA5A5A500u && g_xmm0.u[1] == 0xA5A5A501u &&
          g_xmm0.u[2] == 0xA5A5A502u && g_xmm0.u[3] == 0xA5A5A503u);
}

static void fp_call_1(uint32_t va, uint32_t argument)
{
    guest_write32(g_esp + 4u, argument);
    invoke(va, 4u);
}

/* Two-argument call that restores the entry stack first, so one test can call it repeatedly. */
static void fp_call_2(uint32_t va, uint32_t first, uint32_t second)
{
    g_esp = 0x100u;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, first);
    guest_write32(g_esp + 8u, second);
    invoke(va, 4u);
}

static void test_t1623_fp_list001(void)
{
    /* 46280: [4D183C] = [4D1848] * [4D1840]; [4D184C] = [4D1850] = arg; EAX = arg. */
    fp_begin();
    guest_write32(0x004D1848u, FP_F2);
    guest_write32(0x004D1840u, FP_F3);
    fp_call_1(0x00046280u, 0xCAFE0001u);
    fp_check_gprs(0xCAFE0001u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F6);
    fp_check_xmm1_untouched();
    CHECK(guest_read32(0x004D183Cu) == FP_F6);
    CHECK(guest_read32(0x004D184Cu) == 0xCAFE0001u && guest_read32(0x004D1850u) == 0xCAFE0001u);
    /* a NaN operand stays a NaN, -0.0 * 3.0 = -0.0 */
    fp_begin();
    guest_write32(0x004D1848u, FP_NEG0);
    guest_write32(0x004D1840u, FP_F3);
    fp_call_1(0x00046280u, 7u);
    fp_check_xmm0(FP_NEG0);
    CHECK(guest_read32(0x004D183Cu) == FP_NEG0);
    fp_begin();
    guest_write32(0x004D1848u, FP_NAN);
    guest_write32(0x004D1840u, FP_F3);
    fp_call_1(0x00046280u, 7u);
    fp_check_xmm0(FP_NAN);

    /* 5A150: EAX = 0 only when [obj+0x198] == [475CAC] (ordered), else 1. */
    fp_begin();
    guest_write32(0x00020198u, FP_F1);
    guest_write32(0x00475CACu, FP_F1);
    fp_call_1(0x0005A150u, 0x00020000u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F1);
    fp_check_xmm1_untouched();
    fp_begin();
    guest_write32(0x00020198u, FP_F2);
    fp_call_1(0x0005A150u, 0x00020000u);
    fp_check_gprs(1u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F2);
    fp_begin();
    guest_write32(0x00020198u, FP_NAN);
    fp_call_1(0x0005A150u, 0x00020000u);
    fp_check_gprs(1u, 0x22222222u, 0x33333333u);
    fp_begin();
    guest_write32(0x00020198u, FP_NEG0);
    guest_write32(0x00475CACu, FP_F0);
    fp_call_1(0x0005A150u, 0x00020000u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_NEG0);

    /* 5A1E0: o = [obj+4]; v = [o+0x198]. 1 when (v > 0 and [o+0x12C] == key) or
     * (0 >= v and [o+0xD4] == key). xmm1 = v, xmm0 = 0. */
    fp_begin();
    guest_write32(0x00020004u, 0x00021000u);
    guest_write32(0x00021198u, FP_F1);
    guest_write32(0x0002112Cu, 0x55u);
    guest_write32(0x000210D4u, 0x66u);
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x55u);
    fp_check_gprs(1u, 0x55u, 0x33333333u);
    fp_check_xmm1(FP_F1);
    fp_check_xmm0(FP_F0);
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x66u); /* positive v: only the 0x12C key counts */
    fp_check_gprs(0u, 0x66u, 0x33333333u);
    guest_write32(0x00021198u, FP_F0); /* v == 0: second test, 0 >= 0 */
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x66u);
    fp_check_gprs(1u, 0x66u, 0x33333333u);
    fp_check_xmm1(FP_F0);
    guest_write32(0x0002112Cu, 0x77u); /* v == 0 is not > 0: a 0x12C key match must not count */
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x77u);
    fp_check_gprs(0u, 0x77u, 0x33333333u);
    guest_write32(0x0002112Cu, 0x55u);
    guest_write32(0x00021198u, FP_NEG1); /* negative v, key matches 0x12C only: not found */
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x55u);
    fp_check_gprs(0u, 0x55u, 0x33333333u);
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x66u);
    fp_check_gprs(1u, 0x66u, 0x33333333u);
    guest_write32(0x00021198u, FP_NAN); /* NaN: neither branch, 0 >= NaN is false */
    guest_write32(0x0002112Cu, 0x66u);
    fp_call_2(0x0005A1E0u, 0x00020000u, 0x66u);
    fp_check_gprs(0u, 0x66u, 0x33333333u);
    fp_check_xmm1(FP_NAN);

    /* 73530: [obj+0x84] = [obj+0x88]; [obj+0xC0] = [obj+0x88] * [7BA188]; EAX = obj. */
    fp_begin();
    guest_write32(0x00022088u, FP_F3);
    guest_write32(0x007BA188u, FP_F2);
    fp_call_1(0x00073530u, 0x00022000u);
    fp_check_gprs(0x00022000u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F6);
    CHECK(guest_read32(0x00022084u) == FP_F3 && guest_read32(0x000220C0u) == FP_F6);
    fp_begin();
    guest_write32(0x00022088u, FP_NEG1);
    guest_write32(0x007BA188u, FP_F0);
    fp_call_1(0x00073530u, 0x00022000u);
    fp_check_xmm0(FP_NEG0); /* -1.0 * 0.0 = -0.0 */
    CHECK(guest_read32(0x000220C0u) == FP_NEG0);

    /* A4CA0: 1 when x >= y and y + d > x (x = [obj+0xAC], y = [obj+0x218], d = [475CE8]). */
    fp_begin();
    guest_write32(0x000230ACu, FP_F5);
    guest_write32(0x00023218u, FP_F4);
    guest_write32(0x00475CE8u, FP_F2);
    fp_call_1(0x000A4CA0u, 0x00023000u);
    fp_check_gprs(1u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F6);
    fp_begin();
    guest_write32(0x00475CE8u, FP_F1); /* 4 + 1 > 5 is false */
    fp_call_1(0x000A4CA0u, 0x00023000u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F5);
    fp_begin();
    guest_write32(0x000230ACu, FP_F3); /* x < y: first compare fails, xmm0 keeps x */
    fp_call_1(0x000A4CA0u, 0x00023000u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_F3);
    fp_begin();
    guest_write32(0x000230ACu, FP_NAN); /* unordered: jb is taken */
    fp_call_1(0x000A4CA0u, 0x00023000u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    fp_check_xmm0(FP_NAN);

    /* AEE20: o = [actor+0x7C]. x = [o+0xAC] > [47D5A8]: x -= [4E794C], [([actor+4])+0x30] |= 1,
     * EAX = [actor+4]. Otherwise [o+4] &= ~0x20 and EAX = o. ECX = actor. */
    fp_begin();
    guest_write32(0x0002407Cu, 0x00025000u);
    guest_write32(0x00024004u, 0x00026000u);
    guest_write32(0x000250ACu, FP_F5);
    guest_write32(0x0047D5A8u, FP_F2);
    guest_write32(0x004E794Cu, FP_F1);
    guest_write32(0x00026030u, 0x10u);
    guest_write32(0x00025004u, 0xFFu);
    fp_call_1(0x000AEE20u, 0x00024000u);
    fp_check_gprs(0x00026000u, 0x00024000u, 0x33333333u);
    fp_check_xmm0(FP_F4);
    CHECK(guest_read32(0x000250ACu) == FP_F4 && guest_read32(0x00026030u) == 0x11u);
    CHECK(guest_read32(0x00025004u) == 0xFFu);
    fp_begin();
    guest_write32(0x000250ACu, FP_F2); /* equal to the threshold: not above */
    fp_call_1(0x000AEE20u, 0x00024000u);
    fp_check_gprs(0x00025000u, 0x00024000u, 0x33333333u);
    fp_check_xmm0(FP_F2);
    CHECK(guest_read32(0x000250ACu) == FP_F2 && guest_read32(0x00025004u) == 0xDFu);
    CHECK(guest_read32(0x00026030u) == 0x11u);
    fp_begin();
    guest_write32(0x00025004u, 0xFFu);
    guest_write32(0x000250ACu, FP_NAN); /* unordered: jbe is taken */
    fp_call_1(0x000AEE20u, 0x00024000u);
    fp_check_gprs(0x00025000u, 0x00024000u, 0x33333333u);
    CHECK(guest_read32(0x00025004u) == 0xDFu);

    /* C9BC0: 0 when [733108] == 0 (xmm0 untouched), else 1 when 0 >= [7330BC] (xmm0 cleared). */
    fp_begin();
    guest_write32(0x00733108u, 0u);
    guest_write32(0x007330BCu, FP_NEG1);
    invoke(0x000C9BC0u, 4u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    fp_check_xmm0_untouched();
    fp_begin();
    guest_write32(0x00733108u, 0x1234u);
    guest_write32(0x007330BCu, FP_NEG1);
    invoke(0x000C9BC0u, 4u);
    fp_check_gprs(1u, 0x22222222u, 0x33333333u);
    CHECK(g_xmm0.q[0] == 0u && g_xmm0.q[1] == 0u);
    fp_begin();
    guest_write32(0x00733108u, 0x1234u);
    guest_write32(0x007330BCu, FP_NEG0); /* 0 >= -0 */
    invoke(0x000C9BC0u, 4u);
    fp_check_gprs(1u, 0x22222222u, 0x33333333u);
    fp_begin();
    guest_write32(0x00733108u, 0x1234u);
    guest_write32(0x007330BCu, FP_F1);
    invoke(0x000C9BC0u, 4u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
    CHECK(g_xmm0.q[0] == 0u && g_xmm0.q[1] == 0u);
    fp_begin();
    guest_write32(0x00733108u, 0x1234u);
    guest_write32(0x007330BCu, FP_NAN); /* unordered: jb is taken */
    invoke(0x000C9BC0u, 4u);
    fp_check_gprs(0u, 0x22222222u, 0x33333333u);
}
