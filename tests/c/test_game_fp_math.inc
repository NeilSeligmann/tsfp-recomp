/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1672 native tests for the 3 admitted engine/math roots (00152120 00150580 00151190) of
 * src/game/game_fp_math.c (included by
 * test_game_replacements.c after test_game_fp_step2b.inc, which declares fp2_bits, VecTestXmm,
 * vec_begin and vec3_check_gprs). Each root is checked against an independent plain-C reference
 * that keeps the association order read off the retail disassembly (tmp/dis, XBE sha256 prefix
 * 3cfd001a84fc3e08): every expected value is a float computed on the host with the original
 * product and sum order, compared bit for bit. Inputs are chosen so rounding is visible (0.1f,
 * 1.3f ...), so a re-associated body would differ. Branch edges: both sides of every comiss,
 * equality, NaN operands. NaN payload order is covered by the harness proof, not here. */

/* clang -O3 folds a*b+c into a fused multiply-add when everything is constant (-ffp-contract=on),
 * which is not what the original two-instruction mulss/addss sequence computes. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

static float m1_f(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void m1_put(uint32_t address, const float *values, unsigned count)
{
    for (unsigned index = 0u; index < count; index++) guest_write32(address + 4u * index, fp2_bits(values[index]));
}

static void m1_expect(uint32_t address, const float *values, unsigned count)
{
    for (unsigned index = 0u; index < count; index++)
        CHECK(guest_read32(address + 4u * index) == fp2_bits(values[index]));
}

static const float m1_matrix[16] = {1.3f, 0.7f, -2.1f, 0.3f, 0.1f, -1.7f, 2.9f, 0.45f,
                                    -0.6f, 1.1f, 3.3f, -0.35f, 5.7f, -4.1f, 0.9f, 1.25f};
static const float m1_vector[4] = {0.37f, -1.9f, 2.6f, 0.8f};

#define M1_MATRIX 0x00010200u
#define M1_VEC 0x00010300u
#define M1_OUT 0x00010400u

/* v transformed by columns (m[c+8], m[c+4], m[c]) in the original order. */
static float m1_row(const float *m, unsigned col, const float *v)
{
    float sum = m[col + 8u] * v[2];
    sum = sum + m[col + 4u] * v[1];
    return sum + m[col] * v[0];
}

static void m1_check_direction(const float *m, const float *v)
{
    float expect[3];
    for (unsigned axis = 0u; axis < 3u; axis++) expect[axis] = m1_row(m, axis, v);
    /* 0x00152120 copies four dwords then transforms the copy, the source stays. */
    vec_begin();
    m1_put(M1_MATRIX, m, 16u);
    m1_put(M1_VEC, v, 4u);
    guest_write32(g_esp + 4u, M1_MATRIX);
    guest_write32(g_esp + 8u, M1_VEC);
    guest_write32(g_esp + 12u, M1_OUT);
    invoke(0x00152120u, 4u);
    m1_expect(M1_OUT, expect, 3u);
    CHECK(guest_read32(M1_OUT + 12u) == fp2_bits(v[3]));
    m1_expect(M1_VEC, v, 4u);
    vec3_check_gprs(M1_MATRIX, M1_OUT, M1_OUT);
}

static float m1_edge(float a, float b, float c, float d, float e, float f)
{
    float left = (a - b) * (c - d);
    float right = (e - b) * (f - d);
    return left - right;
}

static void m1_check_triangle(const float *a)
{
    /* i0..i9 as the original stack arguments. */
    uint32_t bits = 0u;
    if (m1_edge(a[2], a[8], a[1], a[9], a[0], a[3]) > 0.0f) bits |= 1u;
    {
        float value = (a[1] - a[7]) * (a[8] - a[6]);
        value = value - (a[0] - a[6]) * (a[9] - a[7]);
        if (value > 0.0f) bits |= 2u;
    }
    {
        float value = (a[1] - a[5]) * (a[6] - a[4]);
        value = value - (a[0] - a[4]) * (a[7] - a[5]);
        if (value > 0.0f) bits |= 4u;
    }
    {
        float value = (a[1] - a[3]) * (a[4] - a[2]);
        value = value - (a[0] - a[2]) * (a[5] - a[3]);
        if (value > 0.0f) bits |= 8u;
    }
    vec_begin();
    for (unsigned index = 0u; index < 10u; index++) guest_write32(g_esp + 4u + 4u * index, fp2_bits(a[index]));
    invoke(0x00151190u, 4u);
    CHECK(g_eax == bits);
}

static void test_t1672_fp_math(void)
{
    m1_check_direction(m1_matrix, m1_vector);
    {
        const float other[4] = {3.1f, 2.2f, -1.15f, 6.0f};
        m1_check_direction(m1_matrix, other);
    }

    /* 0x00150580: quaternion product into out. */
    {
        const float a[4] = {0.11f, -0.52f, 0.33f, 0.78f};
        const float b[4] = {-0.41f, 0.27f, 0.69f, 0.55f};
        const float t0 = b[0] * a[3];
        const float t1 = a[2] * b[1];
        const float t2 = a[3] * b[1];
        const float t3 = b[0] * a[1];
        const float t4 = a[2] * b[2];
        float x0 = t0 - t1;
        x0 = x0 + b[2] * a[1];
        float x2 = t2 - a[0] * b[2];
        x2 = x2 + a[2] * b[0];
        float x1 = b[2] * a[3];
        x1 = x1 - t3;
        x1 = x1 + a[0] * b[1];
        float x3 = a[0] * b[0];
        x3 = x3 + t4;
        x3 = x3 + a[1] * b[1];
        float out[3];
        out[0] = a[0] * x3;
        out[0] = out[0] + x1 * a[1];
        out[0] = out[0] + x0 * a[3];
        out[0] = out[0] - a[2] * x2;
        out[1] = a[2] * x0;
        out[1] = out[1] + x3 * a[1];
        out[1] = out[1] + x2 * a[3];
        out[1] = out[1] - a[0] * x1;
        out[2] = a[2] * x3;
        out[2] = out[2] + a[0] * x2;
        out[2] = out[2] + x1 * a[3];
        out[2] = out[2] - x0 * a[1];
        vec_begin();
        m1_put(M1_MATRIX, a, 4u);
        m1_put(M1_VEC, b, 4u);
        guest_write32(g_esp + 4u, M1_MATRIX);
        guest_write32(g_esp + 8u, M1_VEC);
        guest_write32(g_esp + 12u, M1_OUT);
        invoke(0x00150580u, 4u);
        m1_expect(M1_OUT, out, 3u);
    }

    /* 0x00151190: point against four edge functions. */
    {
        const float inside[10] = {0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const float outside[10] = {5.0f, -3.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const float edge[10] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const float mixed[10] = {0.31f, 0.77f, 0.1f, 0.2f, 0.9f, 0.15f, 0.8f, 0.95f, 0.05f, 0.85f};
        float with_nan[10];
        memcpy(with_nan, inside, sizeof with_nan);
        with_nan[1] = m1_f(0x7FC00000u);
        m1_check_triangle(inside);
        m1_check_triangle(outside);
        m1_check_triangle(edge);
        m1_check_triangle(mixed);
        m1_check_triangle(with_nan);
    }
}

#ifdef __clang__
#pragma STDC FP_CONTRACT ON
#endif
