/* SPDX-License-Identifier: GPL-3.0-or-later
Independent control for 0x0012C6E0 (derived from the original instructions only):
 ecx = [0x7ABF50]; ecx >= 8 (signed) -> return (EAX, EDX untouched, ECX = count). Else
 edx = a1; eax = ecx*24 + 0x7450D8; [0x7ABF50] = ecx + 1; ecx = a0; [eax] = a0; ecx = a2;
 [eax+4] = a1; [eax+8] = [a2]; [eax+0xC] = [a2+4]; ecx = [a2+8]; edx = a3; [eax+0x10] = ecx;
 [eax+0x14] = a3. Every argument is read after the earlier stores. No PUSH or POP exists in
 the original, so the POP-slot alias does not exist here and the alias cases are the
 argument-slot, return-slot and global overlaps. Every case is a named entry state: the
 expected EAX/ECX/EDX/EBX/ESI/EDI/EBP/DF and every written byte were obtained by executing the
 ORIGINAL bytes in Unicorn (original only, never a draft) and cross-checked against a Python
 model of the listing. The whole 8 MB guest memory is diffed on every case (no stray writes).
 Signed boundaries: counts 7 and 8, 0x7FFFFFFF, 0x80000000, -1. */

#define C0012C6E0_RET 0x00010000u

typedef struct {
    uint32_t address;
    uint32_t length;
    const void *bytes;
} c0012c6e0_run;

static uint8_t c0012c6e0_before[sizeof memory];
static uint8_t c0012c6e0_model[sizeof memory];

/* Fresh zeroed guest memory, the standalone prepare() registers, the sentinels and DF. */
static void c0012c6e0_begin(uint32_t esp, int df)
{
    memset(memory, 0, sizeof memory);
    prepare();
    g_esp = esp;
    guest_write32(g_esp, C0012C6E0_RET);
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = df;
}

/* dword i of the run = first + i * step (modulo 2^32). */
static void __attribute__((unused)) c0012c6e0_fill(uint32_t address, uint32_t count, uint32_t first, uint32_t step)
{
    for (uint32_t i = 0; i < count; i++) {
        guest_write32(address + 4u * i, first + i * step);
    }
}

/* Invoke, then compare EAX ECX EDX EBX ESI EDI EBP, DF, ESP (via invoke) and the whole guest
 * memory against the pre-call snapshot plus the listed byte runs (the original's writes). */
static void c0012c6e0_check(const char *name, const uint32_t expect[7], const c0012c6e0_run *runs, size_t run_count)
{
    const unsigned failures_before = failures;
    const int df = g_df;
    memcpy(c0012c6e0_before, memory, sizeof memory);
    invoke(0x12C6E0u, 4u);
    CHECK(g_eax == expect[0]);
    CHECK(g_ecx == expect[1]);
    CHECK(g_edx == expect[2]);
    CHECK(g_ebx == expect[3]);
    CHECK(g_esi == expect[4]);
    CHECK(g_edi == expect[5]);
    CHECK(g_ebp == expect[6]);
    CHECK(g_df == df);
    memcpy(c0012c6e0_model, c0012c6e0_before, sizeof memory);
    for (size_t i = 0; i < run_count; i++) {
        memcpy(&c0012c6e0_model[runs[i].address], runs[i].bytes, runs[i].length);
    }
    int memory_equal = 1;
    for (size_t i = 0; i < sizeof memory; i++) {
        if (c0012c6e0_model[i] != memory[i]) {
            memory_equal = 0;
            fprintf(stderr, "memory differs at 0x%zX: got 0x%02X expected 0x%02X\n", i, memory[i],
                    c0012c6e0_model[i]);
            break;
        }
    }
    CHECK(memory_equal);
    if (failures != failures_before) {
        fprintf(stderr, "failing case: %s\n", name);
    }
}

static void test_t1477_appendix_0012c6e0(void)
{
    /* count 0 appends record 0 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x0u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x1u};
        static const c0012c6e0_run runs[] = {{0x7450D8u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 0 appends record 0", expect, runs, 2);
    }
    /* count 1 appends record 1 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x1u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450F0u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x2u};
        static const c0012c6e0_run runs[] = {{0x7450F0u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 1 appends record 1", expect, runs, 2);
    }
    /* count 2 appends record 2 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x2u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745108u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x3u};
        static const c0012c6e0_run runs[] = {{0x745108u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 2 appends record 2", expect, runs, 2);
    }
    /* count 3 appends record 3 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x3u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745120u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x4u};
        static const c0012c6e0_run runs[] = {{0x745120u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 3 appends record 3", expect, runs, 2);
    }
    /* count 4 appends record 4 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x4u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745138u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x5u};
        static const c0012c6e0_run runs[] = {{0x745138u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 4 appends record 4", expect, runs, 2);
    }
    /* count 5 appends record 5 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x5u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745150u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x6u};
        static const c0012c6e0_run runs[] = {{0x745150u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 5 appends record 5", expect, runs, 2);
    }
    /* count 6 appends record 6 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x6u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745168u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x7u};
        static const c0012c6e0_run runs[] = {{0x745168u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 6 appends record 6", expect, runs, 2);
    }
    /* count 7 appends record 7 */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x7u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745180u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x8u};
        static const c0012c6e0_run runs[] = {{0x745180u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 7 appends record 7", expect, runs, 2);
    }
    /* count 0x8 skips (signed >= 8), EAX/EDX leftovers */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x8u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    g_eax = 0xCAFEF00Du;
    g_edx = 0xBADBEEFu;
    {
        static const uint32_t expect[7] = {0xCAFEF00Du, 0x8u, 0xBADBEEFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c0012c6e0_check("count 0x8 skips (signed >= 8), EAX/EDX leftovers", expect, NULL, 0);
    }
    /* count 0x9 skips (signed >= 8), EAX/EDX leftovers */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x9u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    g_eax = 0xCAFEF00Du;
    g_edx = 0xBADBEEFu;
    {
        static const uint32_t expect[7] = {0xCAFEF00Du, 0x9u, 0xBADBEEFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c0012c6e0_check("count 0x9 skips (signed >= 8), EAX/EDX leftovers", expect, NULL, 0);
    }
    /* count 0x7fffffff skips (signed >= 8), EAX/EDX leftovers */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x7FFFFFFFu);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    g_eax = 0xCAFEF00Du;
    g_edx = 0xBADBEEFu;
    {
        static const uint32_t expect[7] = {0xCAFEF00Du, 0x7FFFFFFFu, 0xBADBEEFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c0012c6e0_check("count 0x7fffffff skips (signed >= 8), EAX/EDX leftovers", expect, NULL, 0);
    }
    /* count 8 skip default regs */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x8u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x11111111u, 0x8u, 0x33333333u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c0012c6e0_check("count 8 skip default regs", expect, NULL, 0);
    }
    /* negative count 0xffffffff appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0xFFFFFFFFu);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450C0u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x0u};
        static const c0012c6e0_run runs[] = {{0x7450C0u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0xffffffff appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* negative count 0xfffffff8 appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0xFFFFFFF8u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745018u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0xFFFFFFF9u};
        static const c0012c6e0_run runs[] = {{0x745018u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0xfffffff8 appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* negative count 0xfffffff0 appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0xFFFFFFF0u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x744F58u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0xFFFFFFF1u};
        static const c0012c6e0_run runs[] = {{0x744F58u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0xfffffff0 appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* negative count 0x80000000 appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x80000000u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x80000001u};
        static const c0012c6e0_run runs[] = {{0x7450D8u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0x80000000 appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* negative count 0x80000001 appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x80000001u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450F0u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x80000002u};
        static const c0012c6e0_run runs[] = {{0x7450F0u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0x80000001 appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* negative count 0xc0000000 appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0xC0000000u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0xC0000001u};
        static const c0012c6e0_run runs[] = {{0x7450D8u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0xc0000000 appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* negative count 0xfffffffe appends (signed < 8, 32 bit wrap) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0xFFFFFFFEu);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450A8u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0xFFFFFFFFu};
        static const c0012c6e0_run runs[] = {{0x7450A8u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("negative count 0xfffffffe appends (signed < 8, 32 bit wrap)", expect, runs, 2);
    }
    /* count 3 with DF set (DF unused) */
    c0012c6e0_begin(0x100u, 1);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x3u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745120u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x4u};
        static const c0012c6e0_run runs[] = {{0x745120u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 3 with DF set (DF unused)", expect, runs, 2);
    }
    /* count 7 extreme vec and args */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x7u);
    guest_write32(0x2000u, 0xFFFFFFFFu);
    guest_write32(0x2004u, 0x80000000u);
    guest_write32(0x2008u, 0x1u);
    guest_write32(0x104u, 0xFFFFFFFFu);
    guest_write32(0x108u, 0x80000000u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x745180u, 0x1u, 0x7FFFFFFFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0x1u, 0x7FFFFFFFu};
        static const uint32_t run1[] = {0x8u};
        static const c0012c6e0_run runs[] = {{0x745180u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 7 extreme vec and args", expect, runs, 2);
    }
    /* count 0 registers EBX ESI EDI EBP custom */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x0u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x2000u);
    guest_write32(0x110u, 0xA3A3A304u);
    g_eax = 0x5u;
    g_ecx = 0x6u;
    g_edx = 0x7u;
    g_ebx = 0x1u;
    g_esi = 0x2u;
    g_edi = 0x3u;
    g_ebp = 0x4u;
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0xA3A3A304u, 0x1u, 0x2u, 0x3u, 0x4u};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x1u};
        static const c0012c6e0_run runs[] = {{0x7450D8u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("count 0 registers EBX ESI EDI EBP custom", expect, runs, 2);
    }
    /* alias: args at record+0..+0xC (count 0), a3 read after vec.y stored there */
    c0012c6e0_begin(0x7450D4u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x7450D4u, 0x10000u);
    guest_write32(0x7ABF50u, 0x0u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x7450D8u, 0xA0A0A001u);
    guest_write32(0x7450DCu, 0xA1A1A102u);
    guest_write32(0x7450E0u, 0x2000u);
    guest_write32(0x7450E4u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0x40000000u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x3F800000u, 0x40000000u, 0xC0400000u, 0x40000000u};
        static const uint32_t run1[] = {0x1u};
        static const c0012c6e0_run runs[] = {{0x7450E0u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: args at record+0..+0xC (count 0), a3 read after vec.y stored there", expect, runs, 2);
    }
    /* alias: a2 slot is record+0, vec pointer read back as a0 (stored first) */
    c0012c6e0_begin(0x7450CCu, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x7450CCu, 0x10000u);
    guest_write32(0x7ABF50u, 0x0u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x7450D0u, 0x2000u);
    guest_write32(0x7450D4u, 0xA1A1A102u);
    guest_write32(0x7450D8u, 0xDEAD0000u);
    guest_write32(0x7450DCu, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0xA1A1A102u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x2000u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA1A1A102u};
        static const uint32_t run1[] = {0x1u};
        static const c0012c6e0_run runs[] = {{0x7450D8u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: a2 slot is record+0, vec pointer read back as a0 (stored first)", expect, runs, 2);
    }
    /* alias: a3 slot is record+8 (count 1), a3 read as vec.x */
    c0012c6e0_begin(0x7450E8u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x7450E8u, 0x10000u);
    guest_write32(0x7ABF50u, 0x1u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x7450ECu, 0xA0A0A001u);
    guest_write32(0x7450F0u, 0xA1A1A102u);
    guest_write32(0x7450F4u, 0x2000u);
    guest_write32(0x7450F8u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450F0u, 0xC0400000u, 0x3F800000u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0x3F800000u};
        static const uint32_t run1[] = {0x2u};
        static const c0012c6e0_run runs[] = {{0x7450F0u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: a3 slot is record+8 (count 1), a3 read as vec.x", expect, runs, 2);
    }
    /* alias: vec pointer is the record (count 2) */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x2u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x745108u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745108u, 0xA0A0A001u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0xA0A0A001u, 0xA1A1A102u, 0xA0A0A001u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x3u};
        static const c0012c6e0_run runs[] = {{0x745108u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: vec pointer is the record (count 2)", expect, runs, 2);
    }
    /* alias: vec pointer is the count global */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x4u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x7ABF50u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745138u, 0x0u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0x5u, 0x0u, 0x0u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x5u};
        static const c0012c6e0_run runs[] = {{0x745138u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: vec pointer is the count global", expect, runs, 2);
    }
    /* alias: a0 slot is the count global (read after the count store) */
    c0012c6e0_begin(0x7ABF4Cu, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x7ABF4Cu, 0x10000u);
    guest_write32(0x7ABF50u, 0x5u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x7ABF50u, 0xA0A0A001u);
    guest_write32(0x7ABF54u, 0xA1A1A102u);
    guest_write32(0x7ABF58u, 0x2000u);
    guest_write32(0x7ABF5Cu, 0xA3A3A304u);
    guest_write32(0x7ABF50u, 0x5u);
    {
        static const uint32_t expect[7] = {0x745150u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x6u, 0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x6u};
        static const c0012c6e0_run runs[] = {{0x745150u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: a0 slot is the count global (read after the count store)", expect, runs, 2);
    }
    /* alias: a1 slot is the count global (read before the count store) */
    c0012c6e0_begin(0x7ABF48u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x7ABF48u, 0x10000u);
    guest_write32(0x7ABF50u, 0x6u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x7ABF4Cu, 0xA0A0A001u);
    guest_write32(0x7ABF50u, 0xA1A1A102u);
    guest_write32(0x7ABF54u, 0x2000u);
    guest_write32(0x7ABF58u, 0xA3A3A304u);
    guest_write32(0x7ABF50u, 0x6u);
    {
        static const uint32_t expect[7] = {0x745168u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0x6u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x7u};
        static const c0012c6e0_run runs[] = {{0x745168u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: a1 slot is the count global (read before the count store)", expect, runs, 2);
    }
    /* alias: return slot is record+0, a0 equals the return address */
    c0012c6e0_begin(0x7450D8u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x7450D8u, 0x10000u);
    guest_write32(0x7ABF50u, 0x0u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x7450DCu, 0x10000u);
    guest_write32(0x7450E0u, 0xA1A1A102u);
    guest_write32(0x7450E4u, 0x2000u);
    guest_write32(0x7450E8u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x7450D8u, 0xC0400000u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA1A1A102u, 0x3F800000u, 0x40000000u, 0xC0400000u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x1u};
        static const c0012c6e0_run runs[] = {{0x7450DCu, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: return slot is record+0, a0 equals the return address", expect, runs, 2);
    }
    /* alias: vec pointer at the argument slots */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x2u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x104u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745108u, 0x104u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0xA0A0A001u, 0xA1A1A102u, 0x104u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x3u};
        static const c0012c6e0_run runs[] = {{0x745108u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: vec pointer at the argument slots", expect, runs, 2);
    }
    /* alias: vec+8 is the a3 slot */
    c0012c6e0_begin(0x100u, 0);
    c0012c6e0_fill(0x744F40u, 152u, 0xEE000000u, 0x4u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7ABF50u, 0x3u);
    guest_write32(0x2000u, 0x3F800000u);
    guest_write32(0x2004u, 0x40000000u);
    guest_write32(0x2008u, 0xC0400000u);
    guest_write32(0x104u, 0xA0A0A001u);
    guest_write32(0x108u, 0xA1A1A102u);
    guest_write32(0x10Cu, 0x108u);
    guest_write32(0x110u, 0xA3A3A304u);
    {
        static const uint32_t expect[7] = {0x745120u, 0xA3A3A304u, 0xA3A3A304u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA0A0A001u, 0xA1A1A102u, 0xA1A1A102u, 0x108u, 0xA3A3A304u, 0xA3A3A304u};
        static const uint32_t run1[] = {0x4u};
        static const c0012c6e0_run runs[] = {{0x745120u, sizeof run0, run0}, {0x7ABF50u, sizeof run1, run1}};
        c0012c6e0_check("alias: vec+8 is the a3 slot", expect, runs, 2);
    }
}
