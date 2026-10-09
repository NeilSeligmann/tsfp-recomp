/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1631 native tests: operand order and NaN payloads of the 10 admitted fp-scalar replacements
 * (src/game/game_fp_list001.c: 0x46280 0x5A150 0x5A1E0 0x73530 0xA4CA0 0xAEE20 0xC9BC0,
 * src/game/game_fp_list002.c: 0x120220 0x151F20 0x347FA0). Included by test_game_replacements.c
 * after test_game_fp_list002.inc (uses fp2_begin, fp_call_1, fp_call_2, FP_* and the externs).
 *
 * WHY. x86 mulss/addss/subss with two NaN operands return the FIRST (destination) operand's
 * payload (an sNaN is quieted), so `a * b` and `b * a` differ in the stored bits. The
 * fp-scalar-v1 differential harness cannot see this: its NaN-pair tripwire excludes two
 * different NaN operands and the Unicorn oracle is symmetric there (docs/t-vector-scalar-design.md),
 * so this file is the only coverage of the NaN-pair domain.
 *
 * REFERENCE. Each expected value is produced by the real instruction, in the original operand
 * order of the retail disassembly, through inline asm with fixed registers: operands travel as
 * 32-bit patterns through integer registers into %xmm6 (destination) and %xmm7 (source), so no
 * compiler float operation, no commuting and no ABI float handling is involved. Control flow
 * (which branch a comiss takes) is also read from the real flags (seta/setb, and the original
 * `ucomiss; lahf; test ah,0x44; jnp` sequence for 0x5A150). The reference chain per body is the
 * disassembly transcribed instruction by instruction.
 *
 * STICKY FLAGS. MXCSR is cleared to 0x1F80 before the replacement and again before the reference
 * chain, and the sticky bits (mask 0x3F) are compared afterwards. That is enforced only when the
 * replacements use the inline-asm helpers (GAME_FP_FIXED_DEST, same condition as in the sources).
 * The portable model (-DGAME_FP_FORCE_PORTABLE, or a non x86-64 host) must still match the
 * reference on every NaN payload, but cannot match comiss/ucomiss sticky flags and does not
 * claim to. */

#if defined(__x86_64__) && defined(__SSE2__)

#if !defined(GAME_FP_FORCE_PORTABLE)
#define NAN1631_STICKY 1
#else
#define NAN1631_STICKY 0
#endif

#define NAN1631_STICKY_MASK 0x3Fu

/* ---------------------------------------------------------------- per-instruction reference */

#define NAN1631_ARITH(name, mnemonic)                                                        \
    static uint32_t name(uint32_t dest, uint32_t source)                                     \
    {                                                                                        \
        uint32_t result;                                                                     \
        __asm__ volatile("movd %1, %%xmm6\n\tmovd %2, %%xmm7\n\t" mnemonic                   \
                         " %%xmm7, %%xmm6\n\tmovd %%xmm6, %0"                                \
                         : "=r"(result)                                                      \
                         : "r"(dest), "r"(source)                                            \
                         : "xmm6", "xmm7");                                                  \
        return result;                                                                       \
    }

NAN1631_ARITH(ref_mulss, "mulss")
NAN1631_ARITH(ref_addss, "addss")
NAN1631_ARITH(ref_subss, "subss")

#define NAN1631_COMPARE(name, mnemonic, setcc)                                               \
    static unsigned name(uint32_t first, uint32_t second)                                    \
    {                                                                                        \
        unsigned char taken;                                                                 \
        __asm__ volatile("movd %1, %%xmm6\n\tmovd %2, %%xmm7\n\t" mnemonic                   \
                         " %%xmm7, %%xmm6\n\t" setcc " %0"                                   \
                         : "=q"(taken)                                                       \
                         : "r"(first), "r"(second)                                           \
                         : "xmm6", "xmm7", "cc");                                            \
        return taken;                                                                        \
    }

NAN1631_COMPARE(ref_comiss_ja, "comiss", "seta")
NAN1631_COMPARE(ref_comiss_jb, "comiss", "setb")

/* ucomiss xmm0, [m] ; lahf ; test ah, 0x44 ; jnp : returns 1 when the jnp is taken. */
static unsigned ref_ucomiss_jnp(uint32_t first, uint32_t second)
{
    unsigned char taken;
    __asm__ volatile("movd %1, %%xmm6\n\tmovd %2, %%xmm7\n\tucomiss %%xmm7, %%xmm6\n\tlahf\n\t"
                     "test $0x44, %%ah\n\tsetnp %0"
                     : "=q"(taken)
                     : "r"(first), "r"(second)
                     : "xmm6", "xmm7", "eax", "cc");
    return taken;
}

static void nan1631_mxcsr_clear(void)
{
    const uint32_t clear = 0x1F80u;
    __asm__ volatile("ldmxcsr %0" : : "m"(clear));
}

static uint32_t nan1631_mxcsr_read(void)
{
    uint32_t value;
    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value & NAN1631_STICKY_MASK;
}

/* ---------------------------------------------------------------- operand corpus */

/* Finite, signed-zero, denormal and infinite values, then NaNs with distinct payloads, both
 * signs and both quiet/signaling (bit 22 clear = signaling). */
static const uint32_t nan1631_corpus[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xC0000000u, 0x7F800000u, 0xFF800000u, 0x00000001u,
    0x7F7FFFFFu, 0x7FC00000u, 0x7FC00001u, 0x7FC00002u, 0x7FC12345u, 0xFFC00001u, 0xFFC00000u,
    0x7F800001u, 0x7F800002u, 0xFF800003u, 0x7FA00000u,
};
#define NAN1631_CORPUS_COUNT ((unsigned)(sizeof nan1631_corpus / sizeof nan1631_corpus[0]))

static unsigned nan1631_failures_printed;
static const char *nan1631_failed_body[16];
static unsigned nan1631_failed_count[16];

/* One case result: counts as a CHECK, prints the first few mismatches with their inputs and
 * keeps a per-body failure tally that test_t1631_fp_nan_order prints at the end. */
static void nan1631_result(int ok, const char *body, uint32_t first, uint32_t second,
                           uint32_t third)
{
    CHECK(ok);
    if (ok) return;
    if (nan1631_failures_printed < 16u) {
        nan1631_failures_printed++;
        fprintf(stderr, "NaN order mismatch in %s, inputs %08X %08X %08X\n", body, first, second,
                third);
    }
    for (unsigned slot = 0u; slot < 16u; slot++) {
        if (nan1631_failed_body[slot] == NULL) nan1631_failed_body[slot] = body;
        if (nan1631_failed_body[slot] == body) {
            nan1631_failed_count[slot]++;
            return;
        }
    }
}

static void nan1631_summary(void)
{
    for (unsigned slot = 0u; slot < 16u && nan1631_failed_body[slot] != NULL; slot++)
        fprintf(stderr, "NaN order failures %s: %u\n", nan1631_failed_body[slot],
                nan1631_failed_count[slot]);
}

static int nan1631_sticky_ok(uint32_t got, uint32_t expected)
{
#if NAN1631_STICKY
    return got == expected;
#else
    (void)got;
    (void)expected;
    return 1;
#endif
}

static int nan1631_lanes_zero(const VecTestXmm *reg)
{
    return reg->u[1] == 0u && reg->u[2] == 0u && reg->u[3] == 0u;
}

/* ---------------------------------------------------------------- per-body tests */

/* 46280: xmm0 = [4D1848] ; mulss xmm0, [4D1840] ; [4D183C] = xmm0. */
static void nan1631_46280(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++) {
            const uint32_t a = nan1631_corpus[i];
            const uint32_t b = nan1631_corpus[j];
            fp2_begin();
            guest_write32(0x004D1848u, a);
            guest_write32(0x004D1840u, b);
            guest_write32(0x004D183Cu, 0xCCCCCCCCu);
            nan1631_mxcsr_clear();
            fp_call_1(0x00046280u, 0xCAFE0001u);
            const uint32_t got_sticky = nan1631_mxcsr_read();
            nan1631_mxcsr_clear();
            const uint32_t expected = ref_mulss(a, b);
            const uint32_t expected_sticky = nan1631_mxcsr_read();
            nan1631_result(guest_read32(0x004D183Cu) == expected && g_xmm0.u[0] == expected &&
                               nan1631_lanes_zero(&g_xmm0) && g_eax == 0xCAFE0001u &&
                               nan1631_sticky_ok(got_sticky, expected_sticky),
                           "0x46280", a, b, 0u);
        }
}

/* 73530: xmm0 = [obj+0x88] ; [obj+0x84] = xmm0 ; mulss xmm0, [7BA188] ; [obj+0xC0] = xmm0. */
static void nan1631_73530(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++) {
            const uint32_t a = nan1631_corpus[i];
            const uint32_t b = nan1631_corpus[j];
            fp2_begin();
            guest_write32(0x00022088u, a);
            guest_write32(0x007BA188u, b);
            guest_write32(0x00022084u, 0xCCCCCCCCu);
            guest_write32(0x000220C0u, 0xCCCCCCCCu);
            nan1631_mxcsr_clear();
            fp_call_1(0x00073530u, 0x00022000u);
            const uint32_t got_sticky = nan1631_mxcsr_read();
            nan1631_mxcsr_clear();
            const uint32_t expected = ref_mulss(a, b);
            const uint32_t expected_sticky = nan1631_mxcsr_read();
            nan1631_result(guest_read32(0x00022084u) == a && guest_read32(0x000220C0u) == expected &&
                               g_xmm0.u[0] == expected && nan1631_lanes_zero(&g_xmm0) &&
                               g_eax == 0x00022000u &&
                               nan1631_sticky_ok(got_sticky, expected_sticky),
                           "0x73530", a, b, 0u);
        }
}

/* 5A150: xmm0 = [obj+0x198] ; ucomiss xmm0, [475CAC] ; lahf ; test ah,0x44 ; jnp -> EAX 0, else 1. */
static void nan1631_5a150(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++) {
            const uint32_t a = nan1631_corpus[i];
            const uint32_t b = nan1631_corpus[j];
            fp2_begin();
            guest_write32(0x00020198u, a);
            guest_write32(0x00475CACu, b);
            nan1631_mxcsr_clear();
            fp_call_1(0x0005A150u, 0x00020000u);
            const uint32_t got_sticky = nan1631_mxcsr_read();
            nan1631_mxcsr_clear();
            const uint32_t expected = ref_ucomiss_jnp(a, b) ? 0u : 1u;
            const uint32_t expected_sticky = nan1631_mxcsr_read();
            nan1631_result(g_eax == expected && g_xmm0.u[0] == a && nan1631_lanes_zero(&g_xmm0) &&
                               nan1631_sticky_ok(got_sticky, expected_sticky),
                           "0x5A150", a, b, 0u);
        }
}

/* 5A1E0: xmm1 = [o+0x198] ; xorps xmm0 ; comiss xmm1, xmm0 ; jbe ; cmp [o+0x12C], key ; je found ;
 * comiss xmm0, [o+0x198] ; jb missing ; cmp [o+0xD4], key ; jne missing ; found: EAX 1. */
static void nan1631_5a1e0(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned variant = 0u; variant < 4u; variant++) {
            const uint32_t v = nan1631_corpus[i];
            const uint32_t key = 0x55u;
            fp2_begin();
            guest_write32(0x00020004u, 0x00021000u);
            guest_write32(0x00021198u, v);
            guest_write32(0x0002112Cu, (variant & 1u) != 0u ? key : 0x66u);
            guest_write32(0x000210D4u, (variant & 2u) != 0u ? key : 0x77u);
            nan1631_mxcsr_clear();
            fp_call_2(0x0005A1E0u, 0x00020000u, key);
            const uint32_t got_sticky = nan1631_mxcsr_read();
            nan1631_mxcsr_clear();
            uint32_t expected = 0u;
            if (ref_comiss_ja(v, 0u) && (variant & 1u) != 0u) {
                expected = 1u;
            } else if (!ref_comiss_jb(0u, v) && (variant & 2u) != 0u) {
                expected = 1u;
            }
            const uint32_t expected_sticky = nan1631_mxcsr_read();
            nan1631_result(g_eax == expected && g_xmm1.u[0] == v && nan1631_lanes_zero(&g_xmm1) &&
                               g_xmm0.u[0] == 0u && nan1631_lanes_zero(&g_xmm0) &&
                               nan1631_sticky_ok(got_sticky, expected_sticky),
                           "0x5A1E0", v, variant, 0u);
        }
}

/* A4CA0: xmm0 = [obj+0xAC] ; comiss xmm0, [obj+0x218] ; jb no ; xmm0 = [obj+0x218] ;
 * addss xmm0, [475CE8] ; comiss xmm0, [obj+0xAC] ; jbe no ; EAX 1. */
static void nan1631_a4ca0(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++)
            for (unsigned k = 0u; k < NAN1631_CORPUS_COUNT; k++) {
                const uint32_t x = nan1631_corpus[i];
                const uint32_t y = nan1631_corpus[j];
                const uint32_t d = nan1631_corpus[k];
                fp2_begin();
                guest_write32(0x000230ACu, x);
                guest_write32(0x00023218u, y);
                guest_write32(0x00475CE8u, d);
                nan1631_mxcsr_clear();
                fp_call_1(0x000A4CA0u, 0x00023000u);
                const uint32_t got_sticky = nan1631_mxcsr_read();
                nan1631_mxcsr_clear();
                uint32_t expected = 0u;
                uint32_t expected_xmm0 = x;
                if (!ref_comiss_jb(x, y)) {
                    expected_xmm0 = ref_addss(y, d);
                    expected = ref_comiss_ja(expected_xmm0, x) ? 1u : 0u;
                }
                const uint32_t expected_sticky = nan1631_mxcsr_read();
                nan1631_result(g_eax == expected && g_xmm0.u[0] == expected_xmm0 &&
                                   nan1631_lanes_zero(&g_xmm0) &&
                                   nan1631_sticky_ok(got_sticky, expected_sticky),
                               "0xA4CA0", x, y, d);
            }
}

/* AEE20: xmm0 = [o+0xAC] ; comiss xmm0, [47D5A8] ; jbe clear ; subss xmm0, [4E794C] ;
 * [o+0xAC] = xmm0 ; EAX = [actor+4] ; [EAX+0x30] |= 1. */
static void nan1631_aee20(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++)
            for (unsigned k = 0u; k < NAN1631_CORPUS_COUNT; k++) {
                const uint32_t x = nan1631_corpus[i];
                const uint32_t threshold = nan1631_corpus[j];
                const uint32_t amount = nan1631_corpus[k];
                fp2_begin();
                guest_write32(0x0002407Cu, 0x00025000u);
                guest_write32(0x00024004u, 0x00026000u);
                guest_write32(0x000250ACu, x);
                guest_write32(0x0047D5A8u, threshold);
                guest_write32(0x004E794Cu, amount);
                guest_write32(0x00026030u, 0x10u);
                guest_write32(0x00025004u, 0xFFu);
                nan1631_mxcsr_clear();
                fp_call_1(0x000AEE20u, 0x00024000u);
                const uint32_t got_sticky = nan1631_mxcsr_read();
                nan1631_mxcsr_clear();
                int ok;
                if (ref_comiss_ja(x, threshold)) {
                    const uint32_t result = ref_subss(x, amount);
                    ok = guest_read32(0x000250ACu) == result && g_xmm0.u[0] == result &&
                         guest_read32(0x00026030u) == 0x11u && guest_read32(0x00025004u) == 0xFFu &&
                         g_eax == 0x00026000u;
                } else {
                    ok = guest_read32(0x000250ACu) == x && g_xmm0.u[0] == x &&
                         guest_read32(0x00026030u) == 0x10u && guest_read32(0x00025004u) == 0xDFu &&
                         g_eax == 0x00025000u;
                }
                const uint32_t expected_sticky = nan1631_mxcsr_read();
                nan1631_result(ok && nan1631_lanes_zero(&g_xmm0) &&
                                   nan1631_sticky_ok(got_sticky, expected_sticky),
                               "0xAEE20", x, threshold, amount);
            }
}

/* C9BC0: EAX = [733108] ; test ; je zero ; xorps xmm0 ; comiss xmm0, [7330BC] ; jb zero ; EAX 1. */
static void nan1631_c9bc0(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned gate = 0u; gate < 2u; gate++) {
            const uint32_t w = nan1631_corpus[i];
            fp2_begin();
            guest_write32(0x00733108u, gate != 0u ? 0x1234u : 0u);
            guest_write32(0x007330BCu, w);
            nan1631_mxcsr_clear();
            invoke(0x000C9BC0u, 4u);
            const uint32_t got_sticky = nan1631_mxcsr_read();
            nan1631_mxcsr_clear();
            uint32_t expected = 0u;
            if (gate != 0u) expected = ref_comiss_jb(0u, w) ? 0u : 1u;
            const uint32_t expected_sticky = nan1631_mxcsr_read();
            nan1631_result(g_eax == expected && nan1631_sticky_ok(got_sticky, expected_sticky),
                           "0xC9BC0", w, gate, 0u);
        }
}

/* 120220: movss xmm0, [esp+8] ; test flag 0x200 ; xmm1 = [obj+0xA0] ; comiss xmm1, xmm0 ; ja ret. */
static void nan1631_120220(void)
{
    const uint32_t second_argument = 0x7FC00007u;
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++)
            for (unsigned flagged = 0u; flagged < 2u; flagged++) {
                const uint32_t arg = nan1631_corpus[i];
                const uint32_t stored = nan1631_corpus[j];
                const uint32_t object = 0x00020100u;
                fp2_begin();
                guest_write32(g_esp + 4u, object);
                guest_write32(g_esp + 8u, arg);
                guest_write32(g_esp + 12u, second_argument);
                guest_write32(object + 4u, flagged != 0u ? 0x200u : 0u);
                guest_write32(object + 0x9Cu, 0xCCCCCCCCu);
                guest_write32(object + 0xA0u, stored);
                guest_write32(object + 0xA4u, 0xDDDDDDDDu);
                nan1631_mxcsr_clear();
                invoke(0x00120220u, 4u);
                const uint32_t got_sticky = nan1631_mxcsr_read();
                nan1631_mxcsr_clear();
                const int returns_early = flagged != 0u && ref_comiss_ja(stored, arg);
                const uint32_t expected_sticky = nan1631_mxcsr_read();
                int ok;
                if (returns_early) {
                    ok = guest_read32(object + 0xA0u) == stored &&
                         guest_read32(object + 0xA4u) == 0xDDDDDDDDu && g_xmm0.u[0] == arg &&
                         g_xmm1.u[0] == stored;
                } else {
                    ok = guest_read32(object + 0xA0u) == arg &&
                         guest_read32(object + 0xA4u) == second_argument &&
                         guest_read32(object + 0x9Cu) == 0u &&
                         guest_read32(object + 4u) == 0x200u;
                }
                nan1631_result(ok && nan1631_sticky_ok(got_sticky, expected_sticky), "0x120220", arg,
                               stored, flagged);
            }
}

/* 347FA0: xmm0 = x ; xmm1 = [475C78] ; comiss xmm0, xmm1 ; ja store_limit ; xorps xmm2 ;
 * comiss xmm2, xmm0 ; jbe next ; store 0 ; ret ; next: comiss xmm0, xmm1 ; jbe store_x ;
 * store_limit: [7670FC] = xmm1 ; ret ; store_x: [7670FC] = xmm0. */
static void nan1631_347fa0(void)
{
    for (unsigned i = 0u; i < NAN1631_CORPUS_COUNT; i++)
        for (unsigned j = 0u; j < NAN1631_CORPUS_COUNT; j++) {
            const uint32_t x = nan1631_corpus[i];
            const uint32_t limit = nan1631_corpus[j];
            fp2_begin();
            guest_write32(0x00475C78u, limit);
            guest_write32(0x007670FCu, 0xCCCCCCCCu);
            nan1631_mxcsr_clear();
            fp_call_1(0x00347FA0u, x);
            const uint32_t got_sticky = nan1631_mxcsr_read();
            nan1631_mxcsr_clear();
            uint32_t stored;
            if (ref_comiss_ja(x, limit)) {
                stored = limit;
            } else if (ref_comiss_ja(0u, x)) {
                stored = 0u;
            } else if (!ref_comiss_ja(x, limit)) {
                stored = x;
            } else {
                stored = limit;
            }
            const uint32_t expected_sticky = nan1631_mxcsr_read();
            nan1631_result(guest_read32(0x007670FCu) == stored &&
                               nan1631_sticky_ok(got_sticky, expected_sticky),
                           "0x347FA0", x, limit, 0u);
        }
}

/* 151F20: the 3x3 transform. The chain below is the disassembly, instruction by instruction. */
typedef struct Nan1631Matrix {
    uint32_t m[9];         /* m0 m4 m8 m10 m14 m18 m20 m24 m28 (index 0..8) */
    uint32_t translate[3]; /* +0x30 +0x34 +0x38 */
    uint32_t vector[3];
} Nan1631Matrix;

static const uint32_t nan1631_matrix_offsets[9] = {0x00u, 0x04u, 0x08u, 0x10u, 0x14u,
                                                   0x18u, 0x20u, 0x24u, 0x28u};

static void nan1631_151f20_case(const Nan1631Matrix *in, uint32_t case_id)
{
    const uint32_t matrix = 0x00020500u;
    const uint32_t vector = 0x00020600u;
    fp2_begin();
    for (unsigned index = 0u; index < 9u; index++)
        guest_write32(matrix + nan1631_matrix_offsets[index], in->m[index]);
    for (unsigned index = 0u; index < 3u; index++) {
        guest_write32(matrix + 0x30u + 4u * index, in->translate[index]);
        guest_write32(vector + 4u * index, in->vector[index]);
    }
    guest_write32(g_esp + 4u, matrix);
    guest_write32(g_esp + 8u, vector);
    nan1631_mxcsr_clear();
    invoke(0x00151F20u, 4u);
    const uint32_t got_sticky = nan1631_mxcsr_read();
    const uint32_t *M = in->m;
    nan1631_mxcsr_clear();
    uint32_t x0, x1, x2, x3, x4;
    uint32_t out[3];
    x3 = M[2];
    x4 = M[1];
    x1 = ref_subss(in->vector[1], in->translate[1]);
    x2 = ref_subss(in->vector[2], in->translate[2]);
    x0 = ref_subss(in->vector[0], in->translate[0]);
    x3 = ref_mulss(x3, x2);
    x4 = ref_mulss(x4, x1);
    x3 = ref_addss(x3, x4);
    x4 = ref_mulss(M[0], x0);
    x3 = ref_addss(x3, x4);
    out[0] = x3;
    x3 = M[5];
    x4 = M[4];
    x3 = ref_mulss(x3, x2);
    x4 = ref_mulss(x4, x1);
    x3 = ref_addss(x3, x4);
    x4 = ref_mulss(M[3], x0);
    x3 = ref_addss(x3, x4);
    out[1] = x3;
    x3 = ref_mulss(M[8], x2);
    x2 = ref_mulss(M[7], x1);
    x1 = M[6];
    x3 = ref_addss(x3, x2);
    x1 = ref_mulss(x1, x0);
    x3 = ref_addss(x3, x1);
    out[2] = x3;
    const uint32_t expected_sticky = nan1631_mxcsr_read();
    nan1631_result(guest_read32(vector) == out[0] && guest_read32(vector + 4u) == out[1] &&
                       guest_read32(vector + 8u) == out[2] && g_xmm0.u[0] == x0 &&
                       g_xmm1.u[0] == x1 && g_xmm2.u[0] == x2 && g_xmm3.u[0] == x3 &&
                       g_xmm4.u[0] == x4 && nan1631_lanes_zero(&g_xmm0) &&
                       nan1631_lanes_zero(&g_xmm1) && nan1631_lanes_zero(&g_xmm2) &&
                       nan1631_lanes_zero(&g_xmm3) && nan1631_lanes_zero(&g_xmm4) &&
                       nan1631_sticky_ok(got_sticky, expected_sticky),
                   "0x151F20", case_id, in->m[0], in->vector[0]);
}

static uint32_t nan1631_random_state = 0x1631A5A5u;

static uint32_t nan1631_next(void)
{
    nan1631_random_state ^= nan1631_random_state << 13;
    nan1631_random_state ^= nan1631_random_state >> 17;
    nan1631_random_state ^= nan1631_random_state << 5;
    return nan1631_random_state;
}

static void nan1631_151f20(void)
{
    /* directed: two different NaN payloads in every ordered pair of the 15 inputs, others finite */
    const uint32_t payload_a[4] = {0x7FC00001u, 0x7F800001u, 0x7FC00003u, 0xFFC00004u};
    const uint32_t payload_b[4] = {0x7FC00002u, 0x7FC00002u, 0x7F800002u, 0x7FC00005u};
    for (unsigned first = 0u; first < 15u; first++)
        for (unsigned second = 0u; second < 15u; second++) {
            if (first == second) continue;
            for (unsigned payload = 0u; payload < 4u; payload++) {
                Nan1631Matrix in;
                uint32_t *slots[15];
                for (unsigned index = 0u; index < 9u; index++) slots[index] = &in.m[index];
                for (unsigned index = 0u; index < 3u; index++) {
                    slots[9u + index] = &in.translate[index];
                    slots[12u + index] = &in.vector[index];
                }
                for (unsigned index = 0u; index < 15u; index++)
                    *slots[index] = 0x3F800000u + 0x00100000u * index; /* 1.0, 1.125, ... */
                *slots[first] = payload_a[payload];
                *slots[second] = payload_b[payload];
                nan1631_151f20_case(&in, first * 16u + second);
            }
        }
    /* seeded random mix: a third of the inputs are NaN, the rest from the finite/inf/zero set */
    for (unsigned count = 0u; count < 20000u; count++) {
        Nan1631Matrix in;
        uint32_t *slots[15];
        for (unsigned index = 0u; index < 9u; index++) slots[index] = &in.m[index];
        for (unsigned index = 0u; index < 3u; index++) {
            slots[9u + index] = &in.translate[index];
            slots[12u + index] = &in.vector[index];
        }
        for (unsigned index = 0u; index < 15u; index++) {
            const uint32_t pick = nan1631_next();
            *slots[index] = (pick & 3u) == 0u
                                ? nan1631_corpus[8u + (pick >> 8) % (NAN1631_CORPUS_COUNT - 8u)]
                                : nan1631_corpus[(pick >> 8) % 8u];
        }
        nan1631_151f20_case(&in, 0xFFFF0000u + count);
    }
}

static void test_t1631_fp_nan_order(void)
{
    nan1631_46280();
    nan1631_73530();
    nan1631_5a150();
    nan1631_5a1e0();
    nan1631_a4ca0();
    nan1631_aee20();
    nan1631_c9bc0();
    nan1631_120220();
    nan1631_151f20();
    nan1631_347fa0();
    nan1631_mxcsr_clear();
    nan1631_summary();
}

#else

static void test_t1631_fp_nan_order(void)
{
    /* The inline-asm reference is x86-64 SSE2 only. Nothing to compare against elsewhere. */
}

#endif
