/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1623 half 2 native tests for the 3 admitted roots of src/game/game_fp_list002.c (included by test_game_replacements.c
 * after test_game_vec_list003.inc, which declares VecTestXmm, g_xmm0, vec_begin and
 * vec3_check_gprs). Expectations are hand-derived from the retail disassembly: GPR outputs,
 * memory, and the raw lanes of every XMM register the original touches. Each XMM register is
 * seeded with a nonzero pattern so lane zeroing (movss load, xorps) and lane preservation
 * (addss, subss, mulss, cvtsi2ss write lane 0 only) are observable. Float inputs are exactly
 * representable and every expected value is computed by hand. */

extern __thread VecTestXmm g_xmm1, g_xmm2, g_xmm3, g_xmm4;

static uint32_t fp2_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static VecTestXmm *fp2_reg(unsigned number)
{
    VecTestXmm *regs[5] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4};
    return regs[number];
}

static uint32_t fp2_seed(unsigned number, unsigned lane)
{
    return 0xA5A5A500u + number * 0x10u + lane;
}

static void fp2_begin(void)
{
    vec_begin();
    for (unsigned number = 0u; number < 5u; number++)
        for (unsigned lane = 0u; lane < 4u; lane++)
            fp2_reg(number)->u[lane] = fp2_seed(number, lane);
}

/* movss load or arithmetic after a movss load: lane 0 = value, lanes 1..3 zero. */
static void fp2_scalar(unsigned number, uint32_t value)
{
    const VecTestXmm *reg = fp2_reg(number);
    CHECK(reg->u[0] == value && reg->u[1] == 0u && reg->u[2] == 0u && reg->u[3] == 0u);
}

/* xorps: all 128 bits clear. */
static void fp2_zero(unsigned number)
{
    fp2_scalar(number, 0u);
}

/* Register never written by the original. */
static void fp2_untouched(unsigned number)
{
    const VecTestXmm *reg = fp2_reg(number);
    CHECK(reg->u[0] == fp2_seed(number, 0u) && reg->u[1] == fp2_seed(number, 1u) &&
          reg->u[2] == fp2_seed(number, 2u) && reg->u[3] == fp2_seed(number, 3u));
}

static void fp2_check_untouched_from(unsigned first)
{
    for (unsigned number = first; number < 5u; number++) fp2_untouched(number);
}

#define FP2_NAN 0x7FC00000u

static void test_t1623_fp_list002(void)
{
    /* 0x120220: cdecl(object, a, b). */
    const uint32_t hit_object = 0x00020100u;
    fp2_begin();
    guest_write32(g_esp + 4u, hit_object);
    guest_write32(g_esp + 8u, fp2_bits(2.0f));
    guest_write32(g_esp + 12u, fp2_bits(3.0f));
    guest_write32(hit_object + 4u, 1u);
    guest_write32(hit_object + 0x9Cu, 0xCCCCCCCCu);
    invoke(0x00120220u, 4u);
    vec3_check_gprs(hit_object, 0x201u, 0x33333333u);
    fp2_zero(0);
    fp2_check_untouched_from(1u);
    CHECK(guest_read32(hit_object + 0xA0u) == fp2_bits(2.0f) && guest_read32(hit_object + 0xA4u) == fp2_bits(3.0f));
    CHECK(guest_read32(hit_object + 4u) == 0x201u && guest_read32(hit_object + 0x9Cu) == 0u);
    /* flag 0x200 set and the stored float larger: nothing is written, XMM0/XMM1 loaded. */
    fp2_begin();
    guest_write32(g_esp + 4u, hit_object);
    guest_write32(g_esp + 8u, fp2_bits(2.0f));
    guest_write32(g_esp + 12u, fp2_bits(7.0f));
    guest_write32(hit_object + 4u, 0x200u);
    guest_write32(hit_object + 0x9Cu, 0xCCCCCCCCu);
    guest_write32(hit_object + 0xA0u, fp2_bits(5.0f));
    guest_write32(hit_object + 0xA4u, 0xDDDDDDDDu);
    invoke(0x00120220u, 4u);
    vec3_check_gprs(hit_object, 0x200u, 0x33333333u);
    fp2_scalar(0, fp2_bits(2.0f));
    fp2_scalar(1, fp2_bits(5.0f));
    fp2_check_untouched_from(2u);
    CHECK(guest_read32(hit_object + 0xA0u) == fp2_bits(5.0f) && guest_read32(hit_object + 0xA4u) == 0xDDDDDDDDu);
    CHECK(guest_read32(hit_object + 4u) == 0x200u && guest_read32(hit_object + 0x9Cu) == 0xCCCCCCCCu);
    /* flag set, stored float equal or smaller or a NaN argument: replaced. */
    const uint32_t hit_stored[3] = {fp2_bits(2.0f), fp2_bits(1.0f), fp2_bits(5.0f)};
    const uint32_t hit_arg[3] = {fp2_bits(2.0f), fp2_bits(2.0f), FP2_NAN};
    for (unsigned index = 0u; index < 3u; index++) {
        fp2_begin();
        guest_write32(g_esp + 4u, hit_object);
        guest_write32(g_esp + 8u, hit_arg[index]);
        guest_write32(g_esp + 12u, fp2_bits(7.0f));
        guest_write32(hit_object + 4u, 0x200u);
        guest_write32(hit_object + 0xA0u, hit_stored[index]);
        invoke(0x00120220u, 4u);
        vec3_check_gprs(hit_object, 0x200u, 0x33333333u);
        fp2_zero(0);
        fp2_scalar(1, hit_stored[index]);
        CHECK(guest_read32(hit_object + 0xA0u) == hit_arg[index] && guest_read32(hit_object + 0xA4u) == fp2_bits(7.0f));
        CHECK(guest_read32(hit_object + 4u) == 0x200u && guest_read32(hit_object + 0x9Cu) == 0u);
    }
    /* stored float exactly 1 ulp above a: still larger, nothing is written. */
    fp2_begin();
    guest_write32(g_esp + 4u, hit_object);
    guest_write32(g_esp + 8u, fp2_bits(2.0f));
    guest_write32(g_esp + 12u, fp2_bits(7.0f));
    guest_write32(hit_object + 4u, 0x200u);
    guest_write32(hit_object + 0x9Cu, 0xCCCCCCCCu);
    guest_write32(hit_object + 0xA0u, fp2_bits(2.0f) + 1u);
    guest_write32(hit_object + 0xA4u, 0xDDDDDDDDu);
    invoke(0x00120220u, 4u);
    vec3_check_gprs(hit_object, 0x200u, 0x33333333u);
    fp2_scalar(0, fp2_bits(2.0f));
    fp2_scalar(1, fp2_bits(2.0f) + 1u);
    CHECK(guest_read32(hit_object + 0xA0u) == fp2_bits(2.0f) + 1u && guest_read32(hit_object + 0xA4u) == 0xDDDDDDDDu);
    CHECK(guest_read32(hit_object + 0x9Cu) == 0xCCCCCCCCu);

    /* 0x151F20: out = M * (v - t), t at +0x30..0x38. M rows (m0,m4,m8), (m10,m14,m18), (m20,m24,m28). */
    const uint32_t matrix = 0x00020500u;
    const uint32_t vector = 0x00020600u;
    const float matrix_values[10] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 0.0f};
    const uint32_t matrix_offsets[9] = {0x00u, 0x04u, 0x08u, 0x10u, 0x14u, 0x18u, 0x20u, 0x24u, 0x28u};
    fp2_begin();
    for (unsigned index = 0u; index < 9u; index++)
        guest_write32(matrix + matrix_offsets[index], fp2_bits(matrix_values[index]));
    guest_write32(matrix + 0x30u, fp2_bits(1.0f));
    guest_write32(matrix + 0x34u, fp2_bits(2.0f));
    guest_write32(matrix + 0x38u, fp2_bits(3.0f));
    guest_write32(vector, fp2_bits(5.0f));
    guest_write32(vector + 4u, fp2_bits(6.0f));
    guest_write32(vector + 8u, fp2_bits(7.0f));
    guest_write32(g_esp + 4u, matrix);
    guest_write32(g_esp + 8u, vector);
    invoke(0x00151F20u, 4u);
    vec3_check_gprs(matrix, vector, 0x33333333u);
    CHECK(guest_read32(vector) == fp2_bits(24.0f) && guest_read32(vector + 4u) == fp2_bits(60.0f));
    CHECK(guest_read32(vector + 8u) == fp2_bits(96.0f));
    fp2_scalar(0, fp2_bits(4.0f));
    fp2_scalar(1, fp2_bits(28.0f));
    fp2_scalar(2, fp2_bits(32.0f));
    fp2_scalar(3, fp2_bits(96.0f));
    fp2_scalar(4, fp2_bits(16.0f));
    /* Negative translation component: operand order of the subtractions. */
    fp2_begin();
    guest_write32(matrix + 0x30u, fp2_bits(-1.0f));
    guest_write32(vector, fp2_bits(5.0f));
    guest_write32(vector + 4u, fp2_bits(6.0f));
    guest_write32(vector + 8u, fp2_bits(7.0f));
    guest_write32(g_esp + 4u, matrix);
    guest_write32(g_esp + 8u, vector);
    invoke(0x00151F20u, 4u);
    /* d = (6, 4, 4): out0 = 3*4 + 2*4 + 1*6 = 26, out1 = 6*4 + 5*4 + 4*6 = 68, out2 = 9*4 + 8*4 + 7*6 = 110. */
    CHECK(guest_read32(vector) == fp2_bits(26.0f) && guest_read32(vector + 4u) == fp2_bits(68.0f));
    CHECK(guest_read32(vector + 8u) == fp2_bits(110.0f));
    fp2_scalar(0, fp2_bits(6.0f));
    /* Rounding order, out0 = (m8*d2 + m4*d1) + m0*d0 with d = (1, 1, 1), m8 = 1, m4 = m0 = 2^-24:
     * (1 + 2^-24) rounds to 1 (tie to even) twice, so out0 = 1.0. Another association gives
     * 1 + 2^-23 = 0x3F800001. */
    fp2_begin();
    for (unsigned index = 0u; index < 9u; index++) guest_write32(matrix + matrix_offsets[index], 0u);
    guest_write32(matrix, 0x33800000u);
    guest_write32(matrix + 4u, 0x33800000u);
    guest_write32(matrix + 8u, fp2_bits(1.0f));
    guest_write32(matrix + 0x30u, 0u);
    guest_write32(matrix + 0x34u, 0u);
    guest_write32(matrix + 0x38u, 0u);
    guest_write32(vector, fp2_bits(1.0f));
    guest_write32(vector + 4u, fp2_bits(1.0f));
    guest_write32(vector + 8u, fp2_bits(1.0f));
    guest_write32(g_esp + 4u, matrix);
    guest_write32(g_esp + 8u, vector);
    invoke(0x00151F20u, 4u);
    CHECK(guest_read32(vector) == 0x3F800000u && guest_read32(vector + 4u) == 0u);
    CHECK(guest_read32(vector + 8u) == 0u);
    /* Product rounded before the add (no fused multiply-add, no double): out1 =
     * (m18*d2 + m14*d1) + m10*d0 with m10 = d0 = 1 + 2^-12, m18 = 2^-23, m14 = 0. The float product
     * (1 + 2^-12)^2 = 1 + 2^-11 + 2^-24 ties to 1 + 2^-11 (0x3F801000), the sum is 0x3F801001.
     * A fused or double evaluation gives 0x3F801002. */
    fp2_begin();
    for (unsigned index = 0u; index < 9u; index++) guest_write32(matrix + matrix_offsets[index], 0u);
    guest_write32(matrix + 0x10u, 0x3F800800u);
    guest_write32(matrix + 0x18u, 0x34000000u);
    guest_write32(vector, 0x3F800800u);
    guest_write32(vector + 4u, fp2_bits(1.0f));
    guest_write32(vector + 8u, fp2_bits(1.0f));
    guest_write32(g_esp + 4u, matrix);
    guest_write32(g_esp + 8u, vector);
    invoke(0x00151F20u, 4u);
    CHECK(guest_read32(vector) == 0u && guest_read32(vector + 4u) == 0x3F801001u);
    CHECK(guest_read32(vector + 8u) == 0u);

    /* 0x347FA0: clamp to [0, [0x475C78]], stored to [0x7670FC]. */
    guest_write32(0x00475C78u, fp2_bits(3.0f));
    const uint32_t clamp_in[7] = {fp2_bits(5.0f), fp2_bits(-2.0f), fp2_bits(1.5f), fp2_bits(3.0f),
                                  0x80000000u, FP2_NAN, 0u};
    const uint32_t clamp_out[7] = {fp2_bits(3.0f), 0u, fp2_bits(1.5f), fp2_bits(3.0f),
                                   0x80000000u, FP2_NAN, 0u};
    for (unsigned index = 0u; index < 7u; index++) {
        fp2_begin();
        guest_write32(g_esp + 4u, clamp_in[index]);
        guest_write32(0x007670FCu, 0xCCCCCCCCu);
        invoke(0x00347FA0u, 4u);
        vec3_check_gprs(0x11111111u, 0x22222222u, 0x33333333u);
        CHECK(guest_read32(0x007670FCu) == clamp_out[index]);
        fp2_scalar(0, clamp_in[index]);
        fp2_scalar(1, fp2_bits(3.0f));
        if (index == 0u) fp2_untouched(2);
        else fp2_zero(2);
    }
}
