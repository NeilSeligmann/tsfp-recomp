/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1634 native tests for the 8 admitted roots of src/game/game_fp_step2b.c (included by
 * test_game_replacements.c after test_game_fp_nan_order.inc, which follows the fp_list002 tests
 * that declare fp2_bits, VecTestXmm, g_xmm0..g_xmm4, vec_begin and vec3_check_gprs).
 * Expectations are hand-derived from the retail disassembly (tmp/dis/all28.txt): GPR outputs,
 * memory, and the raw lanes of every XMM register the original touches (xmm0..xmm7 are all seeded
 * with a nonzero pattern so lane zeroing by movss and lane preservation by ss arithmetic are
 * observable). Float inputs are exactly representable unless a case says otherwise, and every
 * expected bit pattern is worked out from the instruction order of the original. Branch edges
 * covered: both sides of every comiss, equality, unordered (NaN) operands, signed compare of the
 * integer gates, divss by zero and overflow, inexact divides, and operand order of NaN payloads.
 * None of the 8 originals contains a cvttss2si, so there is no truncation case. */

extern __thread VecTestXmm g_xmm5, g_xmm6, g_xmm7;

#define S2_NAN 0x7FC00000u

static VecTestXmm *s2_reg(unsigned number)
{
    VecTestXmm *regs[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    return regs[number];
}

static uint32_t s2_seed(unsigned number, unsigned lane)
{
    return 0xA5A5A500u + number * 0x10u + lane;
}

static void s2_begin(void)
{
    vec_begin();
    for (unsigned number = 0u; number < 8u; number++)
        for (unsigned lane = 0u; lane < 4u; lane++) s2_reg(number)->u[lane] = s2_seed(number, lane);
}

/* Lane 0 is value, lanes 1..3 zero (movss load, or ss arithmetic on a loaded register). */
static void s2_scalar(unsigned number, uint32_t value)
{
    const VecTestXmm *reg = s2_reg(number);
    CHECK(reg->u[0] == value && reg->u[1] == 0u && reg->u[2] == 0u && reg->u[3] == 0u);
}

/* Every register at or above 'first' is untouched except the ones in 'skip_mask'. */
static void s2_untouched_except(unsigned first, unsigned skip_mask)
{
    for (unsigned number = first; number < 8u; number++) {
        if (skip_mask & (1u << number)) continue;
        const VecTestXmm *reg = s2_reg(number);
        CHECK(reg->u[0] == s2_seed(number, 0u) && reg->u[1] == s2_seed(number, 1u) &&
              reg->u[2] == s2_seed(number, 2u) && reg->u[3] == s2_seed(number, 3u));
    }
}

static void s2_untouched_from(unsigned first)
{
    s2_untouched_except(first, 0u);
}

/* 0x000D1E20: cdecl(obj, f1, f3, f4). gate_taken = [79094C] > 0x64 signed: only f3 is stored. */
static void s2_d1e20(uint32_t gate, uint32_t acc, uint32_t f3, uint32_t c1, uint32_t c2,
                     uint32_t b8, int gate_taken, uint32_t x3)
{
    const uint32_t obj = 0x00010100u;
    const uint32_t f1 = fp2_bits(9.0f);
    const uint32_t f4 = fp2_bits(7.5f);
    s2_begin();
    guest_write32(0x0079094Cu, gate);
    guest_write32(0x00478620u, c1);
    guest_write32(0x00475D10u, c2);
    guest_write32(obj + 0xACu, acc);
    guest_write32(g_esp + 4u, obj);
    guest_write32(g_esp + 8u, f1);
    guest_write32(g_esp + 12u, f3);
    guest_write32(g_esp + 16u, f4);
    invoke(0x000D1E20u, 4u);
    vec3_check_gprs(obj, 0x22222222u, 0x33333333u);
    CHECK(guest_read32(obj + 0xB4u) == f1 && guest_read32(obj + 0xB8u) == b8);
    CHECK(guest_read32(obj + 0xBCu) == f4 && guest_read32(obj + 0xACu) == acc);
    s2_scalar(0, f4);
    s2_scalar(1, gate_taken ? f1 : c1);
    if (gate_taken) s2_untouched_except(2u, 1u << 3);
    else s2_scalar(2, c2);
    s2_scalar(3, x3);
    s2_untouched_from(4u);
}

/* 0x00238D90: cdecl(value, out). eax = (flag != 0), xmm0 = divisor, xmm1 = quotient. */
static void s2_238d90(uint32_t flag, uint32_t value, uint32_t div_a, uint32_t div_b,
                      uint32_t expect, uint32_t expect_eax, uint32_t divisor)
{
    const uint32_t out = 0x00010100u;
    s2_begin();
    guest_write32(0x004C0454u, flag);
    guest_write32(0x0049EF8Cu, div_a);
    guest_write32(0x00475DB8u, div_b);
    guest_write32(g_esp + 4u, value);
    guest_write32(g_esp + 8u, out);
    invoke(0x00238D90u, 4u);
    vec3_check_gprs(expect_eax, out, 0x33333333u);
    CHECK(guest_read32(out) == expect && guest_read32(out + 4u) == 0xCCCCCCCCu);
    s2_scalar(0, divisor);
    s2_scalar(1, expect);
    s2_untouched_from(2u);
}

typedef struct S2Phase {
    uint32_t lim, above, wrap, phase, rate, dt, latch0, cnt0;
    uint32_t e_phase, e_latch, e_cnt, e_ecx, e_edx, e_x0, e_x1;
} S2Phase;

static void s2_2daf90(const S2Phase *c)
{
    const uint32_t obj = 0x00010100u;
    s2_begin();
    guest_write32(0x00475CACu, c->lim);
    guest_write32(0x00475C84u, c->above);
    guest_write32(0x00475C88u, c->wrap);
    guest_write32(obj + 0x20u, c->phase);
    guest_write32(obj + 0x24u, c->rate);
    guest_write32(obj + 0x3Cu, c->latch0);
    guest_write32(obj + 0x38u, c->cnt0);
    guest_write32(g_esp + 4u, obj);
    guest_write32(g_esp + 8u, c->dt);
    invoke(0x002DAF90u, 4u);
    vec3_check_gprs(obj, c->e_ecx, c->e_edx);
    CHECK(guest_read32(obj + 0x20u) == c->e_phase && guest_read32(obj + 0x24u) == c->rate);
    CHECK(guest_read32(obj + 0x3Cu) == c->e_latch && guest_read32(obj + 0x38u) == c->e_cnt);
    s2_scalar(0, c->e_x0);
    s2_scalar(1, c->e_x1);
    s2_scalar(2, c->above);
    s2_untouched_from(3u);
}

/* 0x00309120: cdecl(x, y). xmm0 = 1/y, xmm1 = x, xmm2 = y, xmm3 = 1/x. */
static void s2_309120(uint32_t x, uint32_t y, uint32_t inv_x, uint32_t inv_y)
{
    s2_begin();
    guest_write32(0x00475C78u, fp2_bits(1.0f));
    guest_write32(0x00783F2Cu, 0xCCCCCCCCu);
    guest_write32(0x00783F28u, 0xCCCCCCCCu);
    guest_write32(0x00783DD4u, 0xCCCCCCCCu);
    guest_write32(0x00783F20u, 0xCCCCCCCCu);
    guest_write32(g_esp + 4u, x);
    guest_write32(g_esp + 8u, y);
    invoke(0x00309120u, 4u);
    vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
    CHECK(guest_read32(0x00783F2Cu) == x && guest_read32(0x00783F28u) == y);
    CHECK(guest_read32(0x00783DD4u) == inv_x && guest_read32(0x00783F20u) == inv_y);
    s2_scalar(0, inv_y);
    s2_scalar(1, x);
    s2_scalar(2, y);
    s2_scalar(3, inv_x);
    s2_untouched_from(4u);
}

/* 0x00341980: cdecl(slot, state, value, duration). kind 0 = same state, 1 = fade, 2 = instant. */
static void s2_341980(uint32_t old78, uint32_t old80, uint32_t old84, uint32_t old88,
                      uint32_t state, uint32_t value, uint32_t dur, unsigned kind,
                      uint32_t e84, uint32_t e88, uint32_t e_x1, uint32_t e_x2)
{
    const uint32_t slot = 0x00010100u;
    s2_begin();
    guest_write32(0x00475CACu, 0u);
    guest_write32(0x00475C78u, fp2_bits(1.0f));
    guest_write32(slot + 0x78u, old78);
    guest_write32(slot + 0x80u, old80);
    guest_write32(slot + 0x84u, old84);
    guest_write32(slot + 0x88u, old88);
    guest_write32(g_esp + 4u, slot);
    guest_write32(g_esp + 8u, state);
    guest_write32(g_esp + 12u, value);
    guest_write32(g_esp + 16u, dur);
    invoke(0x00341980u, 4u);
    if (kind == 0u) {
        vec3_check_gprs(slot, old78, state);
        CHECK(guest_read32(slot + 0x74u) == 0xCCCCCCCCu && guest_read32(slot + 0x78u) == old78);
        CHECK(guest_read32(slot + 0x7Cu) == 0xCCCCCCCCu && guest_read32(slot + 0x80u) == old80);
        CHECK(guest_read32(slot + 0x84u) == old84 && guest_read32(slot + 0x88u) == old88);
        s2_untouched_from(0u);
        return;
    }
    vec3_check_gprs(slot, value, state);
    CHECK(guest_read32(slot + 0x78u) == state && guest_read32(slot + 0x80u) == value);
    CHECK(guest_read32(slot + 0x84u) == e84 && guest_read32(slot + 0x88u) == e88);
    if (kind == 1u) {
        CHECK(guest_read32(slot + 0x74u) == old78 && guest_read32(slot + 0x7Cu) == old80);
        s2_scalar(0, dur);
        s2_scalar(1, e_x1);
        s2_scalar(2, e_x2);
        s2_untouched_from(3u);
    } else {
        CHECK(guest_read32(slot + 0x74u) == state && guest_read32(slot + 0x7Cu) == value);
        s2_scalar(0, fp2_bits(1.0f));
        s2_untouched_from(1u);
    }
}

/* 0x001543A0 (vec2) and 0x00154470 (vec3): cdecl(out, t, p0, p1, p2, p3).
 * out = p0*u^3 + p1*3u^2t + p2*3ut^2 + p3*t^3, summed as ((p1w + p2w) + p0w) + p3w. */
static void s2_bezier(uint32_t va, unsigned comps, uint32_t t, const uint32_t pts[4][3],
                      const uint32_t *out_expect, const uint32_t regs[6])
{
    const uint32_t out = 0x00010100u;
    const uint32_t base[4] = {0x00010200u, 0x00010240u, 0x00010280u, 0x000102C0u};
    s2_begin();
    guest_write32(0x00475C78u, fp2_bits(1.0f));
    guest_write32(0x00475D18u, fp2_bits(3.0f));
    for (unsigned point = 0u; point < 4u; point++)
        for (unsigned lane = 0u; lane < 3u; lane++) guest_write32(base[point] + 4u * lane, pts[point][lane]);
    guest_write32(g_esp + 4u, out);
    guest_write32(g_esp + 8u, t);
    for (unsigned point = 0u; point < 4u; point++) guest_write32(g_esp + 12u + 4u * point, base[point]);
    invoke(va, 4u);
    vec3_check_gprs(base[1], base[2], base[0]);
    for (unsigned lane = 0u; lane < comps; lane++) CHECK(guest_read32(out + 4u * lane) == out_expect[lane]);
    if (comps < 3u) CHECK(guest_read32(out + 8u) == 0xCCCCCCCCu);
    for (unsigned number = 0u; number < 6u; number++) s2_scalar(number, regs[number]);
    s2_untouched_from(6u);
    for (unsigned point = 0u; point < 4u; point++)
        for (unsigned lane = 0u; lane < 3u; lane++)
            CHECK(guest_read32(base[point] + 4u * lane) == pts[point][lane]);
}

/* Final register values of the two Bezier bodies, from the instruction order in the original.
 * vec2: xmm0 = p3y*t3, xmm1 = 3u^2t, xmm2 = out.y, xmm3 = p3x*t3, xmm4 = t3, xmm5 = u^3.
 * vec3: xmm0 = p3z*t3, xmm1 = 3u^2t, xmm2 = t3, xmm3 = u^3, xmm4 = out.z, xmm5 = p3y*t3. */
static void s2_bezier_regs(unsigned comps, uint32_t b1, uint32_t t3, uint32_t u3,
                           const uint32_t p3[3], const uint32_t *out, uint32_t regs[6])
{
    regs[1] = b1;
    if (comps == 2u) {
        regs[0] = p3[1]; regs[2] = out[1]; regs[3] = p3[0]; regs[4] = t3; regs[5] = u3;
    } else {
        regs[0] = p3[2]; regs[2] = t3; regs[3] = u3; regs[4] = out[2]; regs[5] = p3[1];
    }
}

static void test_t1634_fp_step2b(void)
{
    const uint32_t one = fp2_bits(1.0f);

    /* 0x000D1E20: c1 = [478620] low clamp, c2 = [475D10] high clamp, d = f3 - [obj+0xAC]. */
    /* [79094C] > 0x64 signed: only f3 stored, XMM2 never loaded. */
    s2_d1e20(0x65u, fp2_bits(5.0f), fp2_bits(2.0f), fp2_bits(-1.0f), one, fp2_bits(2.0f), 1, fp2_bits(2.0f));
    /* d = 0 inside [-1, 1]: jbe to the f3 store. 0x64 is not jg. */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(5.0f), fp2_bits(-1.0f), one, fp2_bits(5.0f), 0, fp2_bits(5.0f));
    /* 0xFFFFFFFF is -1 signed, not jg. */
    s2_d1e20(0xFFFFFFFFu, fp2_bits(5.0f), fp2_bits(5.0f), fp2_bits(-1.0f), one, fp2_bits(5.0f), 0, fp2_bits(5.0f));
    /* d = -3 < c1: B8 = AC + c1 = 4, xmm3 = 4. */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(2.0f), fp2_bits(-1.0f), one, fp2_bits(4.0f), 0, fp2_bits(4.0f));
    /* d = 3 > c2: B8 = AC + c2 = 6, xmm3 keeps f3 = 8. */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(8.0f), fp2_bits(-1.0f), one, fp2_bits(6.0f), 0, fp2_bits(8.0f));
    /* d == c1 and d == c2 are inside (ja and jbe both fall to the f3 store). */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(4.0f), fp2_bits(-1.0f), one, fp2_bits(4.0f), 0, fp2_bits(4.0f));
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(6.0f), fp2_bits(-1.0f), one, fp2_bits(6.0f), 0, fp2_bits(6.0f));
    /* d is NaN: both comiss are unordered, ja not taken, jbe taken: f3 (the NaN) stored. */
    s2_d1e20(0x64u, fp2_bits(5.0f), S2_NAN, fp2_bits(-1.0f), one, S2_NAN, 0, S2_NAN);
    /* c1 > d and d > c2 both true (c1 = 2, c2 = -2, d = 0): the second store wins, xmm3 = AC + c1. */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(5.0f), fp2_bits(2.0f), fp2_bits(-2.0f), fp2_bits(3.0f), 0, fp2_bits(7.0f));
    /* c2 NaN with d < c1: first store happens, d > c2 unordered so jbe skips the second. */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(2.0f), fp2_bits(-1.0f), S2_NAN, fp2_bits(4.0f), 0, fp2_bits(4.0f));
    /* c1 NaN with d > c2: ja not taken, the d > c2 test still enters, c1 > d unordered skips. */
    s2_d1e20(0x64u, fp2_bits(5.0f), fp2_bits(8.0f), S2_NAN, one, fp2_bits(6.0f), 0, fp2_bits(8.0f));
    /* Single rounding of AC + c2: 2^24 + 1 ties to even 2^24 (AC = 2^24, f3 = 2^24 + 4, d = 4). */
    s2_d1e20(0x64u, 0x4B800000u, 0x4B800002u, fp2_bits(-1.0f), one, 0x4B800000u, 0, 0x4B800002u);

    /* 0x00238D90: [out] = value / (flag ? [475DB8] : [49EF8C]); eax = flag != 0. */
    s2_238d90(0u, fp2_bits(10.0f), fp2_bits(4.0f), fp2_bits(9.0f), fp2_bits(2.5f), 0u, fp2_bits(4.0f));
    s2_238d90(1u, one, fp2_bits(2.0f), fp2_bits(3.0f), 0x3EAAAAABu, 1u, fp2_bits(3.0f));
    s2_238d90(0x80000000u, one, fp2_bits(2.0f), fp2_bits(7.0f), 0x3E124925u, 1u, fp2_bits(7.0f));
    s2_238d90(0u, one, 0u, fp2_bits(2.0f), 0x7F800000u, 0u, 0u);
    s2_238d90(0u, fp2_bits(-1.0f), 0u, fp2_bits(2.0f), 0xFF800000u, 0u, 0u);
    s2_238d90(0u, one, 0x80000000u, fp2_bits(2.0f), 0xFF800000u, 0u, 0x80000000u);
    s2_238d90(0u, 0u, 0u, fp2_bits(2.0f), 0xFFC00000u, 0u, 0u);
    s2_238d90(0u, 0x7F000000u, fp2_bits(0.5f), fp2_bits(2.0f), 0x7F800000u, 0u, fp2_bits(0.5f));
    s2_238d90(0u, S2_NAN, fp2_bits(2.0f), fp2_bits(2.0f), S2_NAN, 0u, fp2_bits(2.0f));
    s2_238d90(0u, one, 0x7FC00007u, fp2_bits(2.0f), 0x7FC00007u, 0u, 0x7FC00007u);
    s2_238d90(1u, 0x7FC00001u, fp2_bits(2.0f), 0x7FC00002u, 0x7FC00001u, 1u, 0x7FC00002u);
    s2_238d90(1u, 0x7F800003u, fp2_bits(2.0f), fp2_bits(2.0f), 0x7FC00003u, 1u, fp2_bits(2.0f));

    /* 0x002DAF90: phase += rate*dt with latch [obj+0x3C] and counter [obj+0x38]. */
    {
        /* lim L=0, above A=4, wrap B=8, rate*dt = 1*2 = 2 unless a case says otherwise. */
        const uint32_t f0 = 0u, f2 = fp2_bits(2.0f), f4 = fp2_bits(4.0f), f8 = fp2_bits(8.0f);
        const S2Phase cases[] = {
            /* sum -1 <= L: no latch, ret after comiss with A. */
            {f0, f4, f8, fp2_bits(-3.0f), one, f2, 0x77u, 0x55u, fp2_bits(-1.0f), 0x77u, 0x55u, 1u, 0x33333333u, fp2_bits(-1.0f), f2},
            /* sum == L: ZF set, jbe taken, no latch. */
            {f0, f4, f8, fp2_bits(-2.0f), one, f2, 0x77u, 0x55u, 0u, 0x77u, 0x55u, 1u, 0x33333333u, 0u, f2},
            /* L < sum 3 <= A: latch set, ret. */
            {f0, f4, f8, one, one, f2, 0x77u, 0x55u, fp2_bits(3.0f), 1u, 0x55u, 1u, 0x33333333u, fp2_bits(3.0f), f2},
            /* sum == A (4): jbe taken, ret, latch set. */
            {f0, f4, f8, f2, one, f2, 0x77u, 0x55u, f4, 1u, 0x55u, 1u, 0x33333333u, f4, f2},
            /* sum 6 > A, xmm1 + A = 6 == sum (not below), latch 1: counter 0 -> 1 stays. */
            {f0, f4, f8, f4, one, f2, 0x77u, 0u, fp2_bits(6.0f), 1u, 1u, 1u, 1u, fp2_bits(6.0f), fp2_bits(6.0f)},
            /* counter 1 -> 2 (not < 2): reset to 0, ecx = edx = 2. */
            {f0, f4, f8, f4, one, f2, 0x77u, 1u, fp2_bits(6.0f), 1u, 0u, 2u, 2u, fp2_bits(6.0f), fp2_bits(6.0f)},
            /* counter 5 -> 6: reset to 0. */
            {f0, f4, f8, f4, one, f2, 0x77u, 5u, fp2_bits(6.0f), 1u, 0u, 6u, 6u, fp2_bits(6.0f), fp2_bits(6.0f)},
            /* counter -2 -> -1 is jl (signed) 2: kept. */
            {f0, f4, f8, f4, one, f2, 0x77u, 0xFFFFFFFEu, fp2_bits(6.0f), 1u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, fp2_bits(6.0f), fp2_bits(6.0f)},
            /* L=10: sum 6 not above L, latch untouched (0x77 != 1): jne, counter untouched. */
            {fp2_bits(10.0f), f4, f8, f4, one, f2, 0x77u, 0x55u, fp2_bits(6.0f), 0x77u, 0x55u, 1u, 0x33333333u, fp2_bits(6.0f), fp2_bits(6.0f)},
            /* L=10 but latch preset 1: counter 0x55 -> 0x56 >= 2, reset. */
            {fp2_bits(10.0f), f4, f8, f4, one, f2, 1u, 0x55u, fp2_bits(6.0f), 1u, 0u, 0x56u, 0x56u, fp2_bits(6.0f), fp2_bits(6.0f)},
            /* Wrap: phase 10 + 2 = 12 > A, xmm1 + A = 6 < 12: 12 - 8 = 4 stored, xmm1 = B. */
            {f0, f4, f8, fp2_bits(10.0f), one, f2, 0x77u, 0x55u, f4, 1u, 0x55u, 1u, 0x33333333u, f4, f8},
            /* Wrap with sum 12 < B 20 (jb): no store, xmm1 = B. */
            {f0, f4, fp2_bits(20.0f), fp2_bits(10.0f), one, f2, 0x77u, 0x55u, fp2_bits(12.0f), 1u, 0x55u, 1u, 0x33333333u, fp2_bits(12.0f), fp2_bits(20.0f)},
            /* Wrap with sum 12 == B 12: not below, 12 - 12 = +0 stored. */
            {f0, f4, fp2_bits(12.0f), fp2_bits(10.0f), one, f2, 0x77u, 0x55u, 0u, 1u, 0x55u, 1u, 0x33333333u, 0u, fp2_bits(12.0f)},
            /* NaN dt: sum NaN, all compares unordered (jbe taken), phase stores the NaN, no latch. */
            {f0, f4, f8, fp2_bits(10.0f), one, S2_NAN, 0x77u, 0x55u, S2_NAN, 0x77u, 0x55u, 1u, 0x33333333u, S2_NAN, S2_NAN},
            /* NaN phase: rate*dt = 2, sum = 2 + NaN = NaN. */
            {f0, f4, f8, S2_NAN, one, f2, 0x77u, 0x55u, S2_NAN, 0x77u, 0x55u, 1u, 0x33333333u, S2_NAN, f2},
        };
        for (unsigned index = 0u; index < sizeof cases / sizeof cases[0]; index++) s2_2daf90(&cases[index]);
    }

    /* 0x00309120: [783F2C] = x, [783F28] = y, [783DD4] = 1/x, [783F20] = 1/y. */
    s2_309120(fp2_bits(4.0f), fp2_bits(8.0f), fp2_bits(0.25f), fp2_bits(0.125f));
    s2_309120(fp2_bits(3.0f), fp2_bits(7.0f), 0x3EAAAAABu, 0x3E124925u);
    s2_309120(0u, 0x80000000u, 0x7F800000u, 0xFF800000u);
    s2_309120(0x80000000u, 0u, 0xFF800000u, 0x7F800000u);
    /* x = 2^-149 (denormal): 1/x overflows to +inf. y = 2^-127 (denormal): 1/y = 2^127. */
    s2_309120(0x00000001u, 0x00400000u, 0x7F800000u, 0x7F000000u);
    s2_309120(0x7FA00000u, 0x7FC00005u, 0x7FE00000u, 0x7FC00005u);

    /* 0x00341980: slot fade state. Same state: nothing stored, nothing loaded. */
    s2_341980(2u, 0x1111u, fp2_bits(0.25f), fp2_bits(2.0f), 2u, 0x2222u, fp2_bits(4.0f), 0u, 0u, 0u, 0u, 0u);
    /* dur 4 > 0: x1 = 2/4 * (1 - 0.25) = 0.375, [88] = dur. */
    s2_341980(1u, 0x1111u, fp2_bits(0.25f), fp2_bits(2.0f), 2u, 0x2222u, fp2_bits(4.0f), 1u,
              fp2_bits(0.375f), fp2_bits(4.0f), fp2_bits(0.375f), fp2_bits(0.75f));
    /* Inexact divide: (1/3) rounded first = 0x3EAAAAAB, times 0.5 = 0x3E2AAAAB. */
    s2_341980(1u, 0x1111u, fp2_bits(0.5f), one, 2u, 0x2222u, fp2_bits(3.0f), 1u,
              0x3E2AAAABu, fp2_bits(3.0f), 0x3E2AAAABu, fp2_bits(0.5f));
    /* Denormal duration: 1/2^-149 overflows to +inf, times 0.5 stays +inf. */
    s2_341980(1u, 0x1111u, fp2_bits(0.5f), one, 2u, 0x2222u, 1u, 1u,
              0x7F800000u, 1u, 0x7F800000u, fp2_bits(0.5f));
    /* inf * (1 - 1) = inf * 0 is invalid: x86 default NaN. */
    s2_341980(1u, 0x1111u, one, one, 2u, 0x2222u, 1u, 1u, 0xFFC00000u, 1u, 0xFFC00000u, 0u);
    /* dur == 0, negative, -0 and NaN: not above the limit (jbe, unordered sets CF and ZF). */
    s2_341980(1u, 0x1111u, fp2_bits(0.25f), fp2_bits(2.0f), 2u, 0x2222u, 0u, 2u, one, one, 0u, 0u);
    s2_341980(1u, 0x1111u, fp2_bits(0.25f), fp2_bits(2.0f), 2u, 0x2222u, fp2_bits(-1.0f), 2u, one, one, 0u, 0u);
    s2_341980(1u, 0x1111u, fp2_bits(0.25f), fp2_bits(2.0f), 2u, 0x2222u, 0x80000000u, 2u, one, one, 0u, 0u);
    s2_341980(1u, 0x1111u, fp2_bits(0.25f), fp2_bits(2.0f), 2u, 0x2222u, S2_NAN, 2u, one, one, 0u, 0u);

    /* 0x003441A0: delta = target - anchor, then anchor, +0x14, +0x2C, +0x38 sets shift by it. */
    {
        const uint32_t obj = 0x00010100u;
        const uint32_t target = 0x00010300u;
        /* x: target 2^-30, anchor 1.0: delta = fl(2^-30 - 1) = -1.0, anchor = -1 + 1 = +0 (not the
         * target). y: 5 - 2 = 3, anchor 5. z: -1.5 - 0.5 = -2, anchor -1.5. */
        s2_begin();
        guest_write32(obj + 0x20u, one);
        guest_write32(obj + 0x24u, fp2_bits(2.0f));
        guest_write32(obj + 0x28u, fp2_bits(0.5f));
        guest_write32(obj + 0x14u, fp2_bits(10.0f));
        guest_write32(obj + 0x18u, fp2_bits(20.0f));
        guest_write32(obj + 0x1Cu, fp2_bits(30.0f));
        guest_write32(obj + 0x2Cu, fp2_bits(100.0f));
        guest_write32(obj + 0x30u, fp2_bits(200.0f));
        guest_write32(obj + 0x34u, fp2_bits(300.0f));
        guest_write32(obj + 0x38u, fp2_bits(1000.0f));
        guest_write32(obj + 0x3Cu, fp2_bits(2000.0f));
        guest_write32(obj + 0x40u, fp2_bits(3000.0f));
        guest_write32(target, 0x30800000u);
        guest_write32(target + 4u, fp2_bits(5.0f));
        guest_write32(target + 8u, fp2_bits(-1.5f));
        guest_write32(g_esp + 4u, obj);
        guest_write32(g_esp + 8u, target);
        invoke(0x003441A0u, 4u);
        vec3_check_gprs(obj, target, 0x33333333u);
        CHECK(guest_read32(obj + 0x20u) == 0u && guest_read32(obj + 0x24u) == fp2_bits(5.0f));
        CHECK(guest_read32(obj + 0x28u) == fp2_bits(-1.5f));
        CHECK(guest_read32(obj + 0x14u) == fp2_bits(9.0f) && guest_read32(obj + 0x18u) == fp2_bits(23.0f));
        CHECK(guest_read32(obj + 0x1Cu) == fp2_bits(28.0f));
        CHECK(guest_read32(obj + 0x2Cu) == fp2_bits(99.0f) && guest_read32(obj + 0x30u) == fp2_bits(203.0f));
        CHECK(guest_read32(obj + 0x34u) == fp2_bits(298.0f));
        CHECK(guest_read32(obj + 0x38u) == fp2_bits(999.0f) && guest_read32(obj + 0x3Cu) == fp2_bits(2003.0f));
        CHECK(guest_read32(obj + 0x40u) == fp2_bits(2998.0f));
        CHECK(guest_read32(target) == 0x30800000u);
        /* xmm0 = new [40], xmm1 = dy, xmm2 = dz, xmm3 = new [38]. */
        s2_scalar(0, fp2_bits(2998.0f));
        s2_scalar(1, fp2_bits(3.0f));
        s2_scalar(2, fp2_bits(-2.0f));
        s2_scalar(3, fp2_bits(999.0f));
        s2_untouched_from(4u);

        /* NaN payload order: dest operand wins. Delta y = T.y (NaN 2) - anchor y (NaN 3) = NaN 2,
         * new anchor y = NaN 3 + NaN 2 = NaN 3, [18] = NaN 1 + NaN 2 = NaN 1, others take NaN 2. */
        s2_begin();
        guest_write32(obj + 0x20u, fp2_bits(1.0f));
        guest_write32(obj + 0x24u, 0x7FC00003u);
        guest_write32(obj + 0x28u, fp2_bits(1.0f));
        guest_write32(obj + 0x14u, fp2_bits(1.0f));
        guest_write32(obj + 0x18u, 0x7FC00001u);
        guest_write32(obj + 0x1Cu, fp2_bits(1.0f));
        guest_write32(obj + 0x2Cu, fp2_bits(1.0f));
        guest_write32(obj + 0x30u, fp2_bits(4.0f));
        guest_write32(obj + 0x34u, fp2_bits(1.0f));
        guest_write32(obj + 0x38u, fp2_bits(1.0f));
        guest_write32(obj + 0x3Cu, fp2_bits(4.0f));
        guest_write32(obj + 0x40u, fp2_bits(1.0f));
        guest_write32(target, fp2_bits(3.0f));
        guest_write32(target + 4u, 0x7FC00002u);
        guest_write32(target + 8u, fp2_bits(3.0f));
        guest_write32(g_esp + 4u, obj);
        guest_write32(g_esp + 8u, target);
        invoke(0x003441A0u, 4u);
        vec3_check_gprs(obj, target, 0x33333333u);
        CHECK(guest_read32(obj + 0x20u) == fp2_bits(3.0f) && guest_read32(obj + 0x24u) == 0x7FC00003u);
        CHECK(guest_read32(obj + 0x28u) == fp2_bits(3.0f));
        CHECK(guest_read32(obj + 0x14u) == fp2_bits(3.0f) && guest_read32(obj + 0x18u) == 0x7FC00001u);
        CHECK(guest_read32(obj + 0x1Cu) == fp2_bits(3.0f));
        CHECK(guest_read32(obj + 0x2Cu) == fp2_bits(3.0f) && guest_read32(obj + 0x30u) == 0x7FC00002u);
        CHECK(guest_read32(obj + 0x34u) == fp2_bits(3.0f));
        CHECK(guest_read32(obj + 0x38u) == fp2_bits(3.0f) && guest_read32(obj + 0x3Cu) == 0x7FC00002u);
        CHECK(guest_read32(obj + 0x40u) == fp2_bits(3.0f));
        s2_scalar(0, fp2_bits(3.0f));
        s2_scalar(1, 0x7FC00002u);
        s2_scalar(2, fp2_bits(2.0f));
        s2_scalar(3, fp2_bits(3.0f));
        s2_untouched_from(4u);
    }

    /* 0x001543A0 and 0x00154470: cubic Bezier. */
    {
        /* t = 0.125 (all weights exact): u = 0.875, u^3 = 343/512, 3u^2t = 0.287109375,
         * 3ut^2 = 0.041015625, t^3 = 1/512. */
        const uint32_t exact_pts[4][3] = {{0x41000000u, 0x40800000u, 0x40000000u},
                                          {0x41800000u, 0x41400000u, 0x40C00000u},
                                          {0x42000000u, 0x41C00000u, 0x41200000u},
                                          {0x42800000u, 0x42400000u, 0x41600000u}};
        const uint32_t exact_out[3] = {0x41364000u, 0x40E68000u, 0x40600000u};
        const uint32_t exact_p3[3] = {0x3E000000u, 0x3DC00000u, 0x3CE00000u};
        /* t = 0.3f with inexact points (every product and sum rounds in the order of the original). */
        const uint32_t inexact_pts[4][3] = {{0x3F8CCCCDu, 0x400CCCCDu, 0x40533333u},
                                            {0xC0966666u, 0x40BCCCCDu, 0x3DCCCCCDu},
                                            {0x40E9999Au, 0xC101999Au, 0x411B3333u},
                                            {0x412E6666u, 0x4134CCCDu, 0xC14B3333u}};
        const uint32_t inexact_out[3] = {0xBCAF4E70u, 0x40085D62u, 0x402AA64Du};
        const uint32_t inexact_p3[3] = {0x3E96AE7Eu, 0x3E9C3612u, 0xBEAF9098u};
        uint32_t regs[6];
        for (unsigned comps = 2u; comps <= 3u; comps++) {
            const uint32_t va = comps == 2u ? 0x001543A0u : 0x00154470u;
            s2_bezier_regs(comps, 0x3E930000u, 0x3B000000u, 0x3F2B8000u, exact_p3, exact_out, regs);
            s2_bezier(va, comps, 0x3E000000u, exact_pts, exact_out, regs);
            s2_bezier_regs(comps, 0x3EE1CAC0u, 0x3CDD2F1Cu, 0x3EAF9DB2u, inexact_p3, inexact_out, regs);
            s2_bezier(va, comps, 0x3E99999Au, inexact_pts, inexact_out, regs);

            /* t = 0 and t = 1 corners (u = 1 then 0): out = p0 and out = p3. */
            {
                const uint32_t zero_out[3] = {exact_pts[0][0], exact_pts[0][1], exact_pts[0][2]};
                const uint32_t zero_p3[3] = {0u, 0u, 0u};
                const uint32_t one_out[3] = {exact_pts[3][0], exact_pts[3][1], exact_pts[3][2]};
                s2_bezier_regs(comps, 0u, 0u, one, zero_p3, zero_out, regs);
                s2_bezier(va, comps, 0u, exact_pts, zero_out, regs);
                s2_bezier_regs(comps, 0u, one, 0u, exact_pts[3], one_out, regs);
                s2_bezier(va, comps, one, exact_pts, one_out, regs);
            }

            /* NaN in x of all four points: dest-first rule gives p1.x's payload through the
             * ((p1 + p2) + p0) + p3 chain; xmm3 of the vec2 body holds p3.x*t3 = NaN 4. */
            {
                uint32_t pts[4][3];
                uint32_t out[3] = {0x7FC00001u, exact_out[1], exact_out[2]};
                uint32_t p3[3] = {0x7FC00004u, exact_p3[1], exact_p3[2]};
                memcpy(pts, exact_pts, sizeof pts);
                pts[0][0] = 0x7FC00003u;
                pts[1][0] = 0x7FC00001u;
                pts[2][0] = 0x7FC00002u;
                pts[3][0] = 0x7FC00004u;
                s2_bezier_regs(comps, 0x3E930000u, 0x3B000000u, 0x3F2B8000u, p3, out, regs);
                s2_bezier(va, comps, 0x3E000000u, pts, out, regs);
                /* p1 finite: the chain is (p1w + NaN 2) + p0w + NaN 4 = NaN 2; p2.x signaling
                 * input 0x7F800002 is quieted to 0x7FC00002. */
                memcpy(pts, exact_pts, sizeof pts);
                pts[2][0] = 0x7F800002u;
                pts[3][0] = 0x7FC00004u;
                out[0] = 0x7FC00002u;
                s2_bezier_regs(comps, 0x3E930000u, 0x3B000000u, 0x3F2B8000u, p3, out, regs);
                s2_bezier(va, comps, 0x3E000000u, pts, out, regs);
            }
        }
    }
}
