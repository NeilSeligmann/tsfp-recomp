/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 native tests for the guarded-jump roots of src/game/game_fp_guarded.c (included by
 * test_game_replacements.c after test_game_fp_step2b.inc, which declares s2_begin, s2_scalar,
 * s2_untouched_from, fp2_bits, VecTestXmm and g_xmm0..g_xmm7).
 *
 * 0x002777E0 expectations are hand-derived from the retail disassembly, not by running the
 * replacement against itself:
 *   eax = [79094C] - 1; xmm0 = K1 = [475C78]; xmm1 = xmm0; cmp eax,0x50; ja default;
 *   eax = byte [eax+0x277840]; jmp [eax*4+0x277838] with slot 0 = 0x277804 (xmm1 = [475D18], falls
 *   into 0x27780C) and slot 1 = default = 0x27780C; then xmm2 = (float)[4E8CEC]; ecx = obj;
 *   xmm0 /= xmm2; xmm2 = [obj+0x1D8]; xmm2 /= xmm1; xmm0 *= xmm2; xmm0 *= [4E794C].
 * Edges covered: slot 0 and slot 1, the last in-bound index 0x50, the first out-of-bound index
 * 0x51 (whose byte-table neighbour is 0 so an off-by-one bound reads slot 0 and fails), a huge
 * unsigned index and the wrap of [79094C] = 0 to 0xFFFFFFFF. */

#define G1_BYTES 0x00277840u

static void g1_2777e0(uint32_t global, uint32_t byte_value, uint32_t expect_eax, int slot0,
                      int negative_divisor)
{
    const uint32_t obj = 0x00010100u;
    const uint32_t k1 = fp2_bits(8.0f), k2 = fp2_bits(2.0f), health = fp2_bits(6.0f);
    s2_begin();
    memset(&memory[G1_BYTES], 1, 0x51u);
    memset(&memory[G1_BYTES + 0x51u], 0, 8u);
    if (global >= 1u && global - 1u <= 0x50u) memory[G1_BYTES + (global - 1u)] = (uint8_t)byte_value;
    guest_write32(0x0079094Cu, global);
    guest_write32(0x00475C78u, k1);
    guest_write32(0x00475D18u, k2);
    guest_write32(0x004E8CECu, negative_divisor ? 0xFFFFFFFCu : 4u); /* signed int: -4 or 4 */
    guest_write32(0x004E794Cu, fp2_bits(0.5f));
    guest_write32(obj + 0x1D8u, health);
    guest_write32(g_esp + 4u, obj);
    invoke(0x002777E0u, 4u);
    vec3_check_gprs(expect_eax, obj, 0x33333333u);
    s2_scalar(1, slot0 ? k2 : k1);
    s2_scalar(2, fp2_bits(slot0 ? 3.0f : 0.75f));
    s2_scalar(0, fp2_bits((slot0 ? 3.0f : 0.75f) * (negative_divisor ? -1.0f : 1.0f)));
    s2_untouched_from(3u);
}

static void test_t1576_fp_guarded(void)
{
    g1_2777e0(1u, 0u, 0u, 1, 0);           /* index 0, slot 0 */
    g1_2777e0(6u, 1u, 1u, 0, 0);           /* index 5, slot 1 */
    g1_2777e0(0x51u, 0u, 0u, 1, 0);        /* index 0x50: last in-bound entry, slot 0 */
    g1_2777e0(0x52u, 0u, 0x51u, 0, 0);     /* index 0x51: ja taken, eax keeps the index */
    g1_2777e0(0u, 0u, 0xFFFFFFFFu, 0, 0);  /* index 0xFFFFFFFF */
    g1_2777e0(0x80000000u, 0u, 0x7FFFFFFFu, 0, 0);
    g1_2777e0(0x7Fu, 0u, 0x7Eu, 0, 0);     /* far out of bound */
    g1_2777e0(1u, 0u, 0u, 1, 1);           /* [4E8CEC] = -4: cvtsi2ss is signed */
    g1_2777e0(6u, 1u, 1u, 0, 1);
}
