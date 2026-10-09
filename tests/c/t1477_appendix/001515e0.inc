/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x001515E0 (fp-scalar, cdecl 3 args), derived from the original listing:
 *   movss xmm0,[esp+8]            ; T
 *   mov eax,[esp+4]               ; P
 *   movaps xmm1,xmm0 ; subss xmm1,[eax] ; movss [esp+8],xmm1     ; [esp+8] = T - *P
 *   mov ecx,[esp+8] ; movss xmm1,[esp+0xC] (S) ; and ecx,0x7fffffff ; mov [esp+8],ecx
 *   comiss xmm1,[esp+8] ; jbe L   ; S > |T-*P| ordered:  movss [eax],xmm0 ; eax=1 ; ret
 *   L: comiss xmm0,[eax] ; movss xmm0,[eax] ; jbe M
 *      addss xmm0,xmm1 ; movss [eax],xmm0 ; eax=0 ; ret
 *   M: subss xmm0,xmm1 ; movss [eax],xmm0 ; eax=0 ; ret
 * The reference below is that listing transcribed instruction by instruction: the float
 * instructions run on the host CPU through inline asm with fixed registers (xmm6 destination,
 * xmm7 source, operands as 32-bit patterns, so no compiler float op decides the NaN operand
 * order), every guest access goes through a write-overlay shadow so stack aliases of P see the
 * stores of the original in the original order. Hand derived and Unicorn-derived literal
 * cases (original bytes executed in Unicorn 2.1.4 on the named entry states, original only,
 * never the draft) are checked separately at the end. */
extern __thread uint32_t t1477a_1515e0_xmm0[4] __asm__("g_xmm0");
extern __thread uint32_t t1477a_1515e0_xmm1[4] __asm__("g_xmm1");
extern __thread uint32_t t1477a_1515e0_xmm2[4] __asm__("g_xmm2");
extern __thread uint32_t t1477a_1515e0_xmm3[4] __asm__("g_xmm3");
extern __thread uint32_t t1477a_1515e0_xmm4[4] __asm__("g_xmm4");
extern __thread uint32_t t1477a_1515e0_xmm5[4] __asm__("g_xmm5");
extern __thread uint32_t t1477a_1515e0_xmm6[4] __asm__("g_xmm6");
extern __thread uint32_t t1477a_1515e0_xmm7[4] __asm__("g_xmm7");

#define T1477A_1515E0_CELL 0x20000u
#define T1477A_1515E0_STACK_LO 0xE0u
#define T1477A_1515E0_STACK_HI 0x120u
#define T1477A_1515E0_WINDOW 0x20u

static uint32_t *t1477a_1515e0_xmm(unsigned number)
{
    uint32_t *const registers[8] = {t1477a_1515e0_xmm0, t1477a_1515e0_xmm1, t1477a_1515e0_xmm2,
                                    t1477a_1515e0_xmm3, t1477a_1515e0_xmm4, t1477a_1515e0_xmm5,
                                    t1477a_1515e0_xmm6, t1477a_1515e0_xmm7};
    return registers[number];
}

#if defined(__x86_64__) && defined(__SSE2__)
#define T1477A_1515E0_HOST_SSE 1
static uint32_t t1477a_1515e0_subss(uint32_t dest, uint32_t source)
{
    uint32_t out;
    __asm__ volatile("movd %1, %%xmm6\n\tmovd %2, %%xmm7\n\tsubss %%xmm7, %%xmm6\n\tmovd %%xmm6, %0"
                     : "=r"(out) : "r"(dest), "r"(source) : "xmm6", "xmm7");
    return out;
}

static uint32_t t1477a_1515e0_addss(uint32_t dest, uint32_t source)
{
    uint32_t out;
    __asm__ volatile("movd %1, %%xmm6\n\tmovd %2, %%xmm7\n\taddss %%xmm7, %%xmm6\n\tmovd %%xmm6, %0"
                     : "=r"(out) : "r"(dest), "r"(source) : "xmm6", "xmm7");
    return out;
}

/* comiss a, b ; ja  (taken only for ordered a > b) */
static int t1477a_1515e0_ja(uint32_t a, uint32_t b)
{
    unsigned char taken;
    __asm__ volatile("movd %1, %%xmm6\n\tmovd %2, %%xmm7\n\tcomiss %%xmm7, %%xmm6\n\tseta %0"
                     : "=q"(taken) : "r"(a), "r"(b) : "xmm6", "xmm7", "cc");
    return taken;
}
#else
#define T1477A_1515E0_HOST_SSE 0
static float t1477a_1515e0_f(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t t1477a_1515e0_b(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint32_t t1477a_1515e0_subss(uint32_t dest, uint32_t source)
{
    volatile float result = t1477a_1515e0_f(dest) - t1477a_1515e0_f(source);
    return t1477a_1515e0_b(result);
}

static uint32_t t1477a_1515e0_addss(uint32_t dest, uint32_t source)
{
    volatile float result = t1477a_1515e0_f(dest) + t1477a_1515e0_f(source);
    return t1477a_1515e0_b(result);
}

static int t1477a_1515e0_ja(uint32_t a, uint32_t b)
{
    return t1477a_1515e0_f(a) > t1477a_1515e0_f(b);
}
#endif

/* Write overlay: reads fall through to the (unmodified, pre-call) guest memory. */
typedef struct {
    uint32_t address[16];
    uint32_t value[16];
    unsigned count;
} T1477A1515E0Shadow;

static uint32_t t1477a_1515e0_rd(const T1477A1515E0Shadow *shadow, uint32_t address)
{
    for (unsigned i = shadow->count; i-- > 0;)
        if (shadow->address[i] == address) return shadow->value[i];
    return guest_read32(address);
}

static void t1477a_1515e0_wr(T1477A1515E0Shadow *shadow, uint32_t address, uint32_t value)
{
    shadow->address[shadow->count] = address;
    shadow->value[shadow->count] = value;
    shadow->count++;
}

static uint32_t t1477a_1515e0_canary(uint32_t address)
{
    return 0xC0DE0000u ^ (address * 2654435761u);
}

typedef struct {
    uint32_t eax, ecx, xmm0, xmm1;
} T1477A1515E0Result;

/* Reference execution of the listing on the current guest memory (esp = g_esp). */
static T1477A1515E0Result t1477a_1515e0_reference(T1477A1515E0Shadow *shadow)
{
    T1477A1515E0Result result;
    const uint32_t esp = g_esp;
    uint32_t xmm0 = t1477a_1515e0_rd(shadow, esp + 8u);
    const uint32_t eax = t1477a_1515e0_rd(shadow, esp + 4u);
    uint32_t xmm1 = xmm0;
    xmm1 = t1477a_1515e0_subss(xmm1, t1477a_1515e0_rd(shadow, eax));
    t1477a_1515e0_wr(shadow, esp + 8u, xmm1);
    uint32_t ecx = t1477a_1515e0_rd(shadow, esp + 8u);
    xmm1 = t1477a_1515e0_rd(shadow, esp + 0xCu);
    ecx &= 0x7FFFFFFFu;
    t1477a_1515e0_wr(shadow, esp + 8u, ecx);
    result.ecx = ecx;
    if (t1477a_1515e0_ja(xmm1, t1477a_1515e0_rd(shadow, esp + 8u))) {
        t1477a_1515e0_wr(shadow, eax, xmm0);
        result.eax = 1u;
    } else {
        const int above = t1477a_1515e0_ja(xmm0, t1477a_1515e0_rd(shadow, eax));
        xmm0 = t1477a_1515e0_rd(shadow, eax);
        xmm0 = above ? t1477a_1515e0_addss(xmm0, xmm1) : t1477a_1515e0_subss(xmm0, xmm1);
        t1477a_1515e0_wr(shadow, eax, xmm0);
        result.eax = 0u;
    }
    result.xmm0 = xmm0;
    result.xmm1 = xmm1;
    return result;
}

/* Sets the state, predicts with the reference, runs the replacement, compares everything.
 * pointer 0 = the private cell at 0x20000 holding `cell`; any other value is P itself and the
 * slot it points at keeps the content `prepare` or the arguments gave it (alias cases). */


/* Sets the state, predicts with the reference, runs the replacement, compares everything.
 * pointer 0 = the private cell at 0x20000 holding `cell`; any other value is P itself and the
 * slot it points at keeps the content the setup gave it (alias cases: the return slot holds
 * 0x00010000, the arguments are P, target and step). */
static T1477A1515E0Result t1477a_1515e0_run(uint32_t pointer, uint32_t target, uint32_t step,
                                            uint32_t cell)
{
    const uint32_t p = pointer != 0u ? pointer : T1477A_1515E0_CELL;
    uint32_t before_stack[(T1477A_1515E0_STACK_HI - T1477A_1515E0_STACK_LO) / 4u];
    uint32_t before_cell[2u * T1477A_1515E0_WINDOW / 4u];
    prepare();
    for (uint32_t a = T1477A_1515E0_STACK_LO; a < T1477A_1515E0_STACK_HI; a += 4u)
        guest_write32(a, t1477a_1515e0_canary(a));
    for (uint32_t a = T1477A_1515E0_CELL - T1477A_1515E0_WINDOW;
         a < T1477A_1515E0_CELL + T1477A_1515E0_WINDOW; a += 4u)
        guest_write32(a, t1477a_1515e0_canary(a));
    guest_write32(g_esp, 0x00010000u);
    guest_write32(g_esp + 4u, p);
    guest_write32(g_esp + 8u, target);
    guest_write32(g_esp + 0xCu, step);
    if (pointer == 0u) guest_write32(T1477A_1515E0_CELL, cell);
    g_ebx = 0xB1B1B1B1u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    for (unsigned number = 0; number < 8; ++number)
        for (unsigned lane = 0; lane < 4; ++lane)
            t1477a_1515e0_xmm(number)[lane] = 0xA5A5A500u + number * 0x10u + lane;
    for (unsigned i = 0; i < sizeof before_stack / sizeof before_stack[0]; ++i)
        before_stack[i] = guest_read32(T1477A_1515E0_STACK_LO + 4u * i);
    for (unsigned i = 0; i < sizeof before_cell / sizeof before_cell[0]; ++i)
        before_cell[i] = guest_read32(T1477A_1515E0_CELL - T1477A_1515E0_WINDOW + 4u * i);

    T1477A1515E0Shadow shadow = {{0}, {0}, 0u};
    const T1477A1515E0Result expected = t1477a_1515e0_reference(&shadow);

    invoke(0x001515E0u, 4u);
    CHECK(g_eax == expected.eax);
    CHECK(g_ecx == expected.ecx);
    CHECK(g_edx == 0x33333333u);
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u &&
          g_ebp == 0xBBBB0001u);
    CHECK(t1477a_1515e0_xmm(0)[0] == expected.xmm0 && t1477a_1515e0_xmm(0)[1] == 0u &&
          t1477a_1515e0_xmm(0)[2] == 0u && t1477a_1515e0_xmm(0)[3] == 0u);
    CHECK(t1477a_1515e0_xmm(1)[0] == expected.xmm1 && t1477a_1515e0_xmm(1)[1] == 0u &&
          t1477a_1515e0_xmm(1)[2] == 0u && t1477a_1515e0_xmm(1)[3] == 0u);
    for (unsigned number = 2; number < 8; ++number)
        for (unsigned lane = 0; lane < 4; ++lane)
            CHECK(t1477a_1515e0_xmm(number)[lane] == 0xA5A5A500u + number * 0x10u + lane);
    for (unsigned i = 0; i < sizeof before_stack / sizeof before_stack[0]; ++i) {
        const uint32_t address = T1477A_1515E0_STACK_LO + 4u * i;
        uint32_t want = before_stack[i];
        for (unsigned k = 0; k < shadow.count; ++k)
            if (shadow.address[k] == address) want = shadow.value[k];
        CHECK(guest_read32(address) == want);
    }
    for (unsigned i = 0; i < sizeof before_cell / sizeof before_cell[0]; ++i) {
        const uint32_t address = T1477A_1515E0_CELL - T1477A_1515E0_WINDOW + 4u * i;
        uint32_t want = before_cell[i];
        for (unsigned k = 0; k < shadow.count; ++k)
            if (shadow.address[k] == address) want = shadow.value[k];
        CHECK(guest_read32(address) == want);
    }
    return expected;
}

typedef struct {
    uint32_t target, step, cell;
    uint32_t eax, ecx, xmm0, xmm1, cell_after;
} T1477A1515E0CellRow;

typedef struct {
    uint32_t pointer, target, step;
    uint32_t eax, ecx, xmm0, xmm1;
    uint32_t w100, w104, w108, w10c, w110, wfc;
} T1477A1515E0AliasRow;

/* Literal rows below were produced by executing the ORIGINAL bytes (91 bytes at 0x1515E0) in
 * Unicorn 2.1.4 on the named entry state (stack: [esp]=0x10000, canaries, eax/ecx/edx
 * 0x11111111/0x22222222/0x33333333, XMM seeds 0xA5A5A5xx), stopping at the ret. No draft code
 * was run. Rows with two DISTINCT NaN operands are NOT taken from Unicorn (it picks the larger
 * payload, hardware picks the first operand): see the hand-derived rows after the tables. */
static const T1477A1515E0CellRow t1477a_1515e0_cell_rows[] = {
    {0x41200000u, 0x40400000u, 0x00000000u, 0x0u, 0x41200000u, 0x40400000u, 0x40400000u, 0x40400000u}, /* far */
    {0x00000000u, 0x40400000u, 0x41200000u, 0x0u, 0x41200000u, 0x40E00000u, 0x40400000u, 0x40E00000u}, /* far_below */
    {0x41200000u, 0x40400000u, 0x41100000u, 0x1u, 0x3F800000u, 0x41200000u, 0x40400000u, 0x41200000u}, /* arrived */
    {0x41200000u, 0x3F800000u, 0x41100000u, 0x0u, 0x3F800000u, 0x41200000u, 0x3F800000u, 0x41200000u}, /* tie */
    {0x40A00000u, 0x3F800000u, 0x40A00000u, 0x1u, 0x00000000u, 0x40A00000u, 0x3F800000u, 0x40A00000u}, /* same */
    {0x40A00000u, 0x00000000u, 0x40A00000u, 0x0u, 0x00000000u, 0x40A00000u, 0x00000000u, 0x40A00000u}, /* step0 */
    {0x40A00000u, 0xBF800000u, 0x40800000u, 0x0u, 0x3F800000u, 0x40400000u, 0xBF800000u, 0x40400000u}, /* negstep */
    {0x3F800000u, 0x3F800000u, 0x40800000u, 0x0u, 0x40400000u, 0x40400000u, 0x3F800000u, 0x40400000u}, /* diffneg */
    {0x4E800000u, 0x3F800000u, 0x4B800000u, 0x0u, 0x4E7C0000u, 0x4B800000u, 0x3F800000u, 0x4B800000u}, /* round_even */
    {0x7F800000u, 0x3F800000u, 0x7F800000u, 0x0u, 0x7FC00000u, 0x7F800000u, 0x3F800000u, 0x7F800000u}, /* inf_inf */
    {0x40A00000u, 0x7F800000u, 0x3F800000u, 0x1u, 0x40800000u, 0x40A00000u, 0x7F800000u, 0x40A00000u}, /* inf_step */
    {0x7F800001u, 0x3F800000u, 0x40A00000u, 0x0u, 0x7FC00001u, 0x40800000u, 0x3F800000u, 0x40800000u}, /* snan_target */
    {0x00000002u, 0x00000004u, 0x00000001u, 0x1u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00000002u}, /* denorm_arrive */
    {0x00000003u, 0x00000002u, 0x00000001u, 0x0u, 0x00000002u, 0x00000003u, 0x00000002u, 0x00000003u}, /* denorm_tie */
    {0x80000000u, 0x3F800000u, 0x00000000u, 0x1u, 0x00000000u, 0x80000000u, 0x3F800000u, 0x80000000u}, /* neg_zero */
    {0x7F7FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x0u, 0x7F800000u, 0x00000000u, 0x7F7FFFFFu, 0x00000000u}, /* max_cur */
    {0x40A00000u, 0x3F800000u, 0xFF800000u, 0x0u, 0x7F800000u, 0xFF800000u, 0x3F800000u, 0xFF800000u}, /* neg_inf_cur */
    {0x40A00000u, 0x3F800000u, 0x7FC00007u, 0x0u, 0x7FC00007u, 0x7FC00007u, 0x3F800000u, 0x7FC00007u}, /* nan_cur_single */
    {0x40A00000u, 0xFFC00009u, 0x3F800000u, 0x0u, 0x40800000u, 0xFFC00009u, 0xFFC00009u, 0xFFC00009u}, /* nan_step_single */
    {0x7FC0000Bu, 0x3F800000u, 0x40A00000u, 0x0u, 0x7FC0000Bu, 0x40800000u, 0x3F800000u, 0x40800000u}, /* nan_target_single */
};

static const T1477A1515E0AliasRow t1477a_1515e0_alias_rows[] = {
    {0x108u, 0x41200000u, 0x40400000u, 0x1u, 0x00000000u, 0x41200000u, 0x40400000u, 0x00010000u, 0x00000108u, 0x41200000u, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg1 */
    {0x108u, 0x7F800000u, 0x3F800000u, 0x0u, 0x7FC00000u, 0x7FC00000u, 0x3F800000u, 0x00010000u, 0x00000108u, 0x7FC00000u, 0x3F800000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg1_inf */
    {0x108u, 0x40A00000u, 0x00000000u, 0x0u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00010000u, 0x00000108u, 0x00000000u, 0x00000000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg1_step0 */
    {0x108u, 0x80000000u, 0xBF800000u, 0x0u, 0x00000000u, 0x3F800000u, 0xBF800000u, 0x00010000u, 0x00000108u, 0x3F800000u, 0xBF800000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg1_negzero */
    {0x10Cu, 0x41200000u, 0x40400000u, 0x0u, 0x40E00000u, 0x40C00000u, 0x40400000u, 0x00010000u, 0x0000010Cu, 0x40E00000u, 0x40C00000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* step_slot */
    {0x10Cu, 0x40000000u, 0x40400000u, 0x1u, 0x3F800000u, 0x40000000u, 0x40400000u, 0x00010000u, 0x0000010Cu, 0x3F800000u, 0x40000000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* step_slot_arrive */
    {0x10Cu, 0x00000000u, 0x40400000u, 0x0u, 0x40400000u, 0x00000000u, 0x40400000u, 0x00010000u, 0x0000010Cu, 0x40400000u, 0x00000000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* step_slot_zero */
    {0x10Cu, 0x41200000u, 0x7F800000u, 0x0u, 0x7F800000u, 0xFFC00000u, 0x7F800000u, 0x00010000u, 0x0000010Cu, 0x7F800000u, 0xFFC00000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* step_slot_inf */
    {0x104u, 0x41200000u, 0x40400000u, 0x0u, 0x41200000u, 0x40400000u, 0x40400000u, 0x00010000u, 0x40400000u, 0x41200000u, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg0 */
    {0x104u, 0x00000000u, 0x40400000u, 0x1u, 0x00000104u, 0x00000000u, 0x40400000u, 0x00010000u, 0x00000000u, 0x00000104u, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg0_arrive */
    {0x104u, 0x00000000u, 0x3F800000u, 0x1u, 0x00000104u, 0x00000000u, 0x3F800000u, 0x00010000u, 0x00000000u, 0x00000104u, 0x3F800000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* arg0_one */
    {0x100u, 0x41200000u, 0x40400000u, 0x0u, 0x41200000u, 0x40400000u, 0x40400000u, 0x40400000u, 0x00000100u, 0x41200000u, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* ret */
    {0x100u, 0x4D000000u, 0x40400000u, 0x0u, 0x4D000000u, 0x40400000u, 0x40400000u, 0x40400000u, 0x00000100u, 0x4D000000u, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* ret_far */
    {0x100u, 0x00000000u, 0x4E000000u, 0x1u, 0x00010000u, 0x00000000u, 0x4E000000u, 0x00000000u, 0x00000100u, 0x00010000u, 0x4E000000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* ret_arrive */
    {0x110u, 0x41200000u, 0x40400000u, 0x0u, 0x5A2F4C10u, 0xDA2F4C10u, 0x40400000u, 0x00010000u, 0x00000110u, 0x5A2F4C10u, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* above */
    {0xFCu, 0x41200000u, 0x40400000u, 0x0u, 0x7E45CA3Cu, 0x7E45CA3Cu, 0x40400000u, 0x00010000u, 0x000000FCu, 0x7E45CA3Cu, 0x40400000u, 0xDA2F4C10u, 0x7E45CA3Cu}, /* below */
};

static void t1477a_1515e0_check_cell_row(const T1477A1515E0CellRow *row)
{
    t1477a_1515e0_run(0u, row->target, row->step, row->cell);
    CHECK(g_eax == row->eax && g_ecx == row->ecx && g_edx == 0x33333333u);
    CHECK(t1477a_1515e0_xmm(0)[0] == row->xmm0 && t1477a_1515e0_xmm(1)[0] == row->xmm1);
    CHECK(guest_read32(T1477A_1515E0_CELL) == row->cell_after);
    CHECK(guest_read32(0x108u) == row->ecx);
    CHECK(guest_read32(0x100u) == 0x00010000u && guest_read32(0x104u) == T1477A_1515E0_CELL);
    CHECK(guest_read32(0x10Cu) == row->step);
}

static void t1477a_1515e0_check_alias_row(const T1477A1515E0AliasRow *row)
{
    t1477a_1515e0_run(row->pointer, row->target, row->step, 0u);
    CHECK(g_eax == row->eax && g_ecx == row->ecx && g_edx == 0x33333333u);
    CHECK(t1477a_1515e0_xmm(0)[0] == row->xmm0 && t1477a_1515e0_xmm(1)[0] == row->xmm1);
    CHECK(guest_read32(0x100u) == row->w100 && guest_read32(0x104u) == row->w104);
    CHECK(guest_read32(0x108u) == row->w108 && guest_read32(0x10Cu) == row->w10c);
    CHECK(guest_read32(0x110u) == row->w110 && guest_read32(0xFCu) == row->wfc);
}

#if T1477A_1515E0_HOST_SSE
/* Hand derived from the instruction semantics (Intel SDM vol. 1 table 4-7: an SSE scalar
 * arithmetic instruction with two NaN operands returns the FIRST (destination) source operand,
 * quieted). subss #1 is `subss xmm1(T), [P]` (T first), subss #2 is `subss xmm0(*P), xmm1(S)`. */
static void t1477a_1515e0_nan_pair_rows(void)
{
    const uint32_t one = 0x3F800000u;
    /* T = QNaN1, *P = QNaN2 (sign set): diff = T's NaN; ECX/slot = 0x7FC00001; compares are
     * unordered so the move path subtracts: *P - S = *P's NaN 0xFFC00002. */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){0x7FC00001u, one, 0xFFC00002u, 0u,
                                                        0x7FC00001u, 0xFFC00002u, one, 0xFFC00002u});
    /* swapped payloads: diff = T's NaN 0xFFC00002 (ECX 0x7FC00002), *P - S = *P NaN 0x7FC00001 */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){0xFFC00002u, one, 0x7FC00001u, 0u,
                                                        0x7FC00002u, 0x7FC00001u, one, 0x7FC00001u});
    /* *P = QNaN3, S = QNaN4 (T finite): subss #2 destination *P wins => 0x7FC00003, not S's */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){one, 0x7FC00004u, 0x7FC00003u, 0u,
                                                        0x7FC00003u, 0x7FC00003u, 0x7FC00004u,
                                                        0x7FC00003u});
    /* same with the larger payload in *P and the sign set on S: *P still wins */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){one, 0xFFC00003u, 0x7FC00004u, 0u,
                                                        0x7FC00004u, 0x7FC00004u, 0xFFC00003u,
                                                        0x7FC00004u});
    /* T = SNaN1, *P = QNaN2: first operand wins and is quieted: diff 0x7FC00001 */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){0x7F800001u, one, 0x7FC00002u, 0u,
                                                        0x7FC00001u, 0x7FC00002u, one, 0x7FC00002u});
    /* T = QNaN5, *P = SNaN3: first operand (the QNaN) wins for diff; *P - S quiets *P's SNaN */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){0x7FC00005u, one, 0x7F800003u, 0u,
                                                        0x7FC00005u, 0x7FC00003u, one, 0x7FC00003u});
    /* *P = SNaN4, S = QNaN9: destination *P wins, quieted */
    t1477a_1515e0_check_cell_row(&(T1477A1515E0CellRow){one, 0x7FC00009u, 0x7F800004u, 0u,
                                                        0x7FC00004u, 0x7FC00004u, 0x7FC00009u,
                                                        0x7FC00004u});
    /* aliased P = arg1 slot with a NaN target: *P = T (equal NaNs): diff T quieted */
    t1477a_1515e0_check_alias_row(&(T1477A1515E0AliasRow){0x108u, 0x7F800005u, one, 0u,
                                                          0x7FC00005u, 0x7FC00005u, one,
                                                          0x00010000u, 0x00000108u, 0x7FC00005u,
                                                          one, 0xDA2F4C10u, 0x7E45CA3Cu});
}
#endif

static void test_t1477_appendix_001515e0(void)
{
    /* ---- Original-derived literal rows ---- */
    for (unsigned i = 0; i < sizeof t1477a_1515e0_cell_rows / sizeof t1477a_1515e0_cell_rows[0]; ++i)
        t1477a_1515e0_check_cell_row(&t1477a_1515e0_cell_rows[i]);
    for (unsigned i = 0; i < sizeof t1477a_1515e0_alias_rows / sizeof t1477a_1515e0_alias_rows[0]; ++i)
        t1477a_1515e0_check_alias_row(&t1477a_1515e0_alias_rows[i]);
#if T1477A_1515E0_HOST_SSE
    t1477a_1515e0_nan_pair_rows();
#endif

    /* ---- Reference sweep over a value set: private cell ---- */
    const uint32_t values[] = {
        0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x40000000u, 0x3F000000u, 0x40400000u,
        0x41200000u, 0x4B800000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x80000001u, 0x7F800000u,
        0xFF800000u, 0x7F7FFFFFu, 0x7FC00000u, 0xFFC00000u, 0x7FC00001u, 0xFFC00002u, 0x7F800001u,
        0x7FA00000u, 0x3F800001u, 0x3F7FFFFFu};
    const unsigned count = sizeof values / sizeof values[0];
    unsigned arrived = 0u;
    unsigned moved = 0u;
    for (unsigned t = 0; t < count; ++t)
        for (unsigned s = 0; s < count; ++s)
            for (unsigned c = 0; c < count; ++c) {
                const T1477A1515E0Result e = t1477a_1515e0_run(0u, values[t], values[s], values[c]);
                if (e.eax == 1u) arrived++; else moved++;
            }
    CHECK(arrived > 100u && moved > 100u);

    /* ---- Reference sweep with P aliasing the call's own frame ---- */
    const uint32_t aliases[] = {0x100u, 0x104u, 0x108u, 0x10Cu, 0x110u, 0xFCu};
    for (unsigned a = 0; a < sizeof aliases / sizeof aliases[0]; ++a)
        for (unsigned t = 0; t < count; ++t)
            for (unsigned s = 0; s < count; ++s)
                t1477a_1515e0_run(aliases[a], values[t], values[s], 0u);
}
