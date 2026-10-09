/* SPDX-License-Identifier: GPL-3.0-or-later
Independent control for 0x00172C80 (derived from the original instructions only):
 eax = a0; ecx = [eax+0x7C] (read BEFORE the pushes); push esi; push edi; edi = a1; eax = [edi]
 (v, signed); clamped = v < 0 ? 0 : v; edx = [ecx+0x140] - 1; if edx < clamped (signed) idx = edx
 else idx = clamped; edx = idx*0x34; [edi] = idx; esi = edx + ecx + 0x70; eax = idx + 1;
 cdq; idiv dword [ecx+0x140] (re-read after the store); eax = esi; [edi] = remainder; pop edi;
 pop esi. Exit EAX = slot pointer, ECX = [a0+0x7C], EDX = remainder, ESI/EDI popped from the
 guest slots. The `jmp` the census calls skip:jump is the unconditional short jump at 0x172CA8.
 TRAPS: a zero divisor raises #DE in the original and the draft executes __builtin_trap (the
 host analog). The trapping inputs (count 0, and the aliased index pointer on the count word with
 count 1 or 0x80000000 / v 0x80000000) are not run in-process: a forked child proves the draft
 does not return normally. The non-trapping part is checked against the original executed in
 Unicorn on the named entry states (original only, never a draft), cross-checked against a Python
 model of the listing. Quotient overflow (INT_MIN / -1) is unreachable here: -1 as a divisor
 means count -1 which gives idx -2 so the dividend is -1. Whole guest memory diffed.
 Alias controls: the index pointer on the count word, on the pushed ESI/EDI slots (POP reads
 the remainder store) and on the argument slots, count word on the pushed slots, and the
 [object+0x7C] read before the pushes (sees the pre-push memory). */

#define C00172C80_RET 0x00010000u

typedef struct {
    uint32_t address;
    uint32_t length;
    const void *bytes;
} c00172c80_run;

static uint8_t c00172c80_before[sizeof memory];
static uint8_t c00172c80_model[sizeof memory];

/* Fresh zeroed guest memory, the standalone prepare() registers, the sentinels and DF. */
static void c00172c80_begin(uint32_t esp, int df)
{
    memset(memory, 0, sizeof memory);
    prepare();
    g_esp = esp;
    guest_write32(g_esp, C00172C80_RET);
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = df;
}

/* dword i of the run = first + i * step (modulo 2^32). */
static void __attribute__((unused)) c00172c80_fill(uint32_t address, uint32_t count, uint32_t first, uint32_t step)
{
    for (uint32_t i = 0; i < count; i++) {
        guest_write32(address + 4u * i, first + i * step);
    }
}

/* Invoke, then compare EAX ECX EDX EBX ESI EDI EBP, DF, ESP (via invoke) and the whole guest
 * memory against the pre-call snapshot plus the listed byte runs (the original's writes). */
static void c00172c80_check(const char *name, const uint32_t expect[7], const c00172c80_run *runs, size_t run_count)
{
    const unsigned failures_before = failures;
    const int df = g_df;
    memcpy(c00172c80_before, memory, sizeof memory);
    invoke(0x172C80u, 4u);
    CHECK(g_eax == expect[0]);
    CHECK(g_ecx == expect[1]);
    CHECK(g_edx == expect[2]);
    CHECK(g_ebx == expect[3]);
    CHECK(g_esi == expect[4]);
    CHECK(g_edi == expect[5]);
    CHECK(g_ebp == expect[6]);
    CHECK(g_df == df);
    memcpy(c00172c80_model, c00172c80_before, sizeof memory);
    for (size_t i = 0; i < run_count; i++) {
        memcpy(&c00172c80_model[runs[i].address], runs[i].bytes, runs[i].length);
    }
    int memory_equal = 1;
    for (size_t i = 0; i < sizeof memory; i++) {
        if (c00172c80_model[i] != memory[i]) {
            memory_equal = 0;
            fprintf(stderr, "memory differs at 0x%zX: got 0x%02X expected 0x%02X\n", i, memory[i],
                    c00172c80_model[i]);
            break;
        }
    }
    CHECK(memory_equal);
    if (failures != failures_before) {
        fprintf(stderr, "failing case: %s\n", name);
    }
}

#include <signal.h>
/* POSIX calls declared by hand: the native runner compiles with -std=c11. */
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);
static void c00172c80_trap_ill(int sig) { (void)sig; _exit(41); }
static void c00172c80_trap_trap(int sig) { (void)sig; _exit(42); }
static void c00172c80_trap_fpe(int sig) { (void)sig; _exit(43); }

static void test_t1477_appendix_00172c80(void)
{
    /* count 1 index 0x0 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("count 1 index 0x0", expect, runs, 1);
    }
    /* count 1 index 0x1 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x1u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x1", expect, runs, 2);
    }
    /* count 1 index 0x2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x2u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x2", expect, runs, 2);
    }
    /* count 1 index 0x3 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x3u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x3", expect, runs, 2);
    }
    /* count 1 index 0x4 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x4u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x4", expect, runs, 2);
    }
    /* count 1 index 0x5 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x5", expect, runs, 2);
    }
    /* count 1 index 0x6 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x6u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x6", expect, runs, 2);
    }
    /* count 1 index 0x9 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x9u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x9", expect, runs, 2);
    }
    /* count 1 index 0xa */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0xAu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0xa", expect, runs, 2);
    }
    /* count 1 index 0x7fffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x7fffffff", expect, runs, 2);
    }
    /* count 1 index 0x80000000 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0x80000000", expect, runs, 2);
    }
    /* count 1 index 0xffffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0xffffffff", expect, runs, 2);
    }
    /* count 1 index 0xfffffffe */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x1u);
    guest_write32(0x2400u, 0xFFFFFFFEu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 1 index 0xfffffffe", expect, runs, 2);
    }
    /* count 2 index 0x0 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x0", expect, runs, 2);
    }
    /* count 2 index 0x1 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x1u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x1", expect, runs, 2);
    }
    /* count 2 index 0x2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x2u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x2", expect, runs, 2);
    }
    /* count 2 index 0x3 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x3u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x3", expect, runs, 2);
    }
    /* count 2 index 0x4 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x4u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x4", expect, runs, 2);
    }
    /* count 2 index 0x5 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x5u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x5", expect, runs, 2);
    }
    /* count 2 index 0x6 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x6u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x6", expect, runs, 2);
    }
    /* count 2 index 0x9 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x9u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x9", expect, runs, 2);
    }
    /* count 2 index 0xa */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0xAu);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0xa", expect, runs, 2);
    }
    /* count 2 index 0x7fffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x7fffffff", expect, runs, 2);
    }
    /* count 2 index 0x80000000 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0x80000000", expect, runs, 2);
    }
    /* count 2 index 0xffffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0xffffffff", expect, runs, 2);
    }
    /* count 2 index 0xfffffffe */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    guest_write32(0x2400u, 0xFFFFFFFEu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 2 index 0xfffffffe", expect, runs, 2);
    }
    /* count 3 index 0x0 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x0", expect, runs, 2);
    }
    /* count 3 index 0x1 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x1u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x2u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x2u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x1", expect, runs, 2);
    }
    /* count 3 index 0x2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x2u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x2", expect, runs, 2);
    }
    /* count 3 index 0x3 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x3u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x3", expect, runs, 2);
    }
    /* count 3 index 0x4 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x4u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x4", expect, runs, 2);
    }
    /* count 3 index 0x5 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x5u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x5", expect, runs, 2);
    }
    /* count 3 index 0x6 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x6u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x6", expect, runs, 2);
    }
    /* count 3 index 0x9 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x9u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x9", expect, runs, 2);
    }
    /* count 3 index 0xa */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0xAu);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0xa", expect, runs, 2);
    }
    /* count 3 index 0x7fffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x7fffffff", expect, runs, 2);
    }
    /* count 3 index 0x80000000 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0x80000000", expect, runs, 2);
    }
    /* count 3 index 0xffffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0xffffffff", expect, runs, 2);
    }
    /* count 3 index 0xfffffffe */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    guest_write32(0x2400u, 0xFFFFFFFEu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 3 index 0xfffffffe", expect, runs, 2);
    }
    /* count 5 index 0x0 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x0", expect, runs, 2);
    }
    /* count 5 index 0x1 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x1u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x2u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x2u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x1", expect, runs, 2);
    }
    /* count 5 index 0x2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x2u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x3u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x3u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x2", expect, runs, 2);
    }
    /* count 5 index 0x3 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x3u);
    {
        static const uint32_t expect[7] = {0x18010Cu, 0x180000u, 0x4u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x4u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x3", expect, runs, 2);
    }
    /* count 5 index 0x4 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x4u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x4", expect, runs, 2);
    }
    /* count 5 index 0x5 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x5", expect, runs, 2);
    }
    /* count 5 index 0x6 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x6u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x6", expect, runs, 2);
    }
    /* count 5 index 0x9 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x9u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x9", expect, runs, 2);
    }
    /* count 5 index 0xa */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0xAu);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0xa", expect, runs, 2);
    }
    /* count 5 index 0x7fffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x7fffffff", expect, runs, 2);
    }
    /* count 5 index 0x80000000 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0x80000000", expect, runs, 2);
    }
    /* count 5 index 0xffffffff */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0xffffffff", expect, runs, 2);
    }
    /* count 5 index 0xfffffffe */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0xFFFFFFFEu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 5 index 0xfffffffe", expect, runs, 2);
    }
    /* count 0xa index 0x0 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xAu);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xa index 0x0 (signed bounds)", expect, runs, 2);
    }
    /* count 0xa index 0x8 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xAu);
    guest_write32(0x2400u, 0x8u);
    {
        static const uint32_t expect[7] = {0x180210u, 0x180000u, 0x9u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x9u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xa index 0x8 (signed bounds)", expect, runs, 2);
    }
    /* count 0xa index 0x9 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xAu);
    guest_write32(0x2400u, 0x9u);
    {
        static const uint32_t expect[7] = {0x180244u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xa index 0x9 (signed bounds)", expect, runs, 2);
    }
    /* count 0xa index 0xa (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xAu);
    guest_write32(0x2400u, 0xAu);
    {
        static const uint32_t expect[7] = {0x180244u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xa index 0xa (signed bounds)", expect, runs, 2);
    }
    /* count 0xa index 0xb (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xAu);
    guest_write32(0x2400u, 0xBu);
    {
        static const uint32_t expect[7] = {0x180244u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xa index 0xb (signed bounds)", expect, runs, 2);
    }
    /* count 0x7fffffff index 0x0 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x7FFFFFFFu);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x7fffffff index 0x0 (signed bounds)", expect, runs, 2);
    }
    /* count 0x7fffffff index 0x5 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x7FFFFFFFu);
    guest_write32(0x2400u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180174u, 0x180000u, 0x6u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x6u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x7fffffff index 0x5 (signed bounds)", expect, runs, 2);
    }
    /* count 0x7fffffff index 0x7fffffff (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x7FFFFFFFu);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180008u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x7fffffff index 0x7fffffff (signed bounds)", expect, runs, 2);
    }
    /* count 0x7fffffff index 0xffffffff (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x7FFFFFFFu);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x7fffffff index 0xffffffff (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000000 index 0x0 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000000u);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000000 index 0x0 (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000000 index 0x1 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000000u);
    guest_write32(0x2400u, 0x1u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x2u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x2u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000000 index 0x1 (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000000 index 0x7fffffff (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000000u);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x18003Cu, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000000 index 0x7fffffff (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000000 index 0x80000000 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000000u);
    guest_write32(0x2400u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000000 index 0x80000000 (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000000 index 0xffffffff (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000000u);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000000 index 0xffffffff (signed bounds)", expect, runs, 2);
    }
    /* count 0xffffffff index 0x0 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFFu);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180008u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("count 0xffffffff index 0x0 (signed bounds)", expect, runs, 1);
    }
    /* count 0xffffffff index 0x3 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFFu);
    guest_write32(0x2400u, 0x3u);
    {
        static const uint32_t expect[7] = {0x180008u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xffffffff index 0x3 (signed bounds)", expect, runs, 2);
    }
    /* count 0xffffffff index 0xffffffff (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFFu);
    guest_write32(0x2400u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180008u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xffffffff index 0xffffffff (signed bounds)", expect, runs, 2);
    }
    /* count 0xfffffffe index 0x0 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFEu);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x17FFD4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("count 0xfffffffe index 0x0 (signed bounds)", expect, runs, 1);
    }
    /* count 0xfffffffe index 0x7 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFEu);
    guest_write32(0x2400u, 0x7u);
    {
        static const uint32_t expect[7] = {0x17FFD4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xfffffffe index 0x7 (signed bounds)", expect, runs, 2);
    }
    /* count 0xfffffffe index 0x80000000 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFEu);
    guest_write32(0x2400u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0x17FFD4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0xfffffffe index 0x80000000 (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000001 index 0x0 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000001u);
    guest_write32(0x2400u, 0x0u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("count 0x80000001 index 0x0 (signed bounds)", expect, runs, 1);
    }
    /* count 0x80000001 index 0x1 (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000001u);
    guest_write32(0x2400u, 0x1u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000001 index 0x1 (signed bounds)", expect, runs, 2);
    }
    /* count 0x80000001 index 0x7fffffff (signed bounds) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x80000001u);
    guest_write32(0x2400u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("count 0x80000001 index 0x7fffffff (signed bounds)", expect, runs, 2);
    }
    /* DF set (unused) */
    c00172c80_begin(0x4000u, 1);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x4u);
    guest_write32(0x2400u, 0x2u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x3u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x3u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("DF set (unused)", expect, runs, 2);
    }
    /* custom sentinels EBX EBP ESI EDI */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x4u);
    guest_write32(0x2400u, 0x3u);
    g_eax = 0x5u;
    g_ecx = 0x6u;
    g_edx = 0x7u;
    g_ebx = 0x1u;
    g_esi = 0x3u;
    g_edi = 0x4u;
    g_ebp = 0x2u;
    {
        static const uint32_t expect[7] = {0x18010Cu, 0x180000u, 0x0u, 0x1u, 0x3u, 0x4u, 0x2u};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0x4u, 0x3u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("custom sentinels EBX EBP ESI EDI", expect, runs, 2);
    }
    /* alias: index pointer is the count word (count 2), divisor re-read after the store */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x2u);
    {
        static const uint32_t expect[7] = {0x1800A4u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x0u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x180140u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the count word (count 2), divisor re-read after the store", expect, runs, 2);
    }
    /* alias: index pointer is the count word (count 3), divisor re-read after the store */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x3u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x1u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x180140u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the count word (count 3), divisor re-read after the store", expect, runs, 2);
    }
    /* alias: index pointer is the count word (count 4), divisor re-read after the store */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x4u);
    {
        static const uint32_t expect[7] = {0x18010Cu, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x1u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x180140u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the count word (count 4), divisor re-read after the store", expect, runs, 2);
    }
    /* alias: index pointer is the count word (count 5), divisor re-read after the store */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x1u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x180140u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the count word (count 5), divisor re-read after the store", expect, runs, 2);
    }
    /* alias: index pointer is the count word (count 10), divisor re-read after the store */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xAu);
    {
        static const uint32_t expect[7] = {0x180244u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x1u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x180140u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the count word (count 10), divisor re-read after the store", expect, runs, 2);
    }
    /* alias: index pointer is the count word, count 0x7FFFFFFF */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180008u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x1u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x180140u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the count word, count 0x7FFFFFFF", expect, runs, 2);
    }
    /* alias: index pointer is the count word, count -1 (v negative) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x180140u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x180008u, 0x180000u, 0xFFFFFFFFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("alias: index pointer is the count word, count -1 (v negative)", expect, runs, 1);
    }
    /* pop alias: index pointer is the pushed EDI slot (EDI sentinel negative -> clamp 0) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x3FF8u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180070u, 0x180000u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0x1u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x1u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("pop alias: index pointer is the pushed EDI slot (EDI sentinel negative -> clamp 0)", expect, runs, 1);
    }
    /* pop alias: index pointer is the pushed EDI slot, EDI = 3 (v = 3) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x3FF8u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    g_edi = 0x3u;
    {
        static const uint32_t expect[7] = {0x18010Cu, 0x180000u, 0x4u, 0xB4B4B4B4u, 0x54545454u, 0x4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("pop alias: index pointer is the pushed EDI slot, EDI = 3 (v = 3)", expect, runs, 1);
    }
    /* pop alias: index pointer is the pushed EDI slot, EDI = 9 (above count) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x3FF8u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    g_edi = 0x9u;
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0x0u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x54545454u};
        static const c00172c80_run runs[] = {{0x3FFCu, sizeof run0, run0}};
        c00172c80_check("pop alias: index pointer is the pushed EDI slot, EDI = 9 (above count)", expect, runs, 1);
    }
    /* pop alias: index pointer is the pushed ESI slot, ESI = 2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x3FFCu);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    g_esi = 0x2u;
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x3u, 0xB4B4B4B4u, 0x3u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x3u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("pop alias: index pointer is the pushed ESI slot, ESI = 2", expect, runs, 1);
    }
    /* pop alias: index pointer is the pushed ESI slot, ESI sentinel (positive big) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x3FFCu);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x0u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}};
        c00172c80_check("pop alias: index pointer is the pushed ESI slot, ESI sentinel (positive big)", expect, runs, 1);
    }
    /* alias: index pointer is the a1 slot (v = its own address) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x4008u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x0u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x4008u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the a1 slot (v = its own address)", expect, runs, 2);
    }
    /* alias: index pointer is the a0 slot (v = the object pointer) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x4004u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x5u);
    {
        static const uint32_t expect[7] = {0x180140u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x0u};
        static const c00172c80_run runs[] = {{0x3FF8u, sizeof run0, run0}, {0x4004u, sizeof run1, run1}};
        c00172c80_check("alias: index pointer is the a0 slot (v = the object pointer)", expect, runs, 2);
    }
    /* alias: small stack, index pointer is the a1 slot (v = 0x108 > count) */
    c00172c80_begin(0x100u, 0);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x104u, 0x2000u);
    guest_write32(0x108u, 0x108u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x9u);
    {
        static const uint32_t expect[7] = {0x180210u, 0x180000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x0u};
        static const c00172c80_run runs[] = {{0xF8u, sizeof run0, run0}, {0x108u, sizeof run1, run1}};
        c00172c80_check("alias: small stack, index pointer is the a1 slot (v = 0x108 > count)", expect, runs, 2);
    }
    /* alias: small stack, v is the a1 slot, count 0x200 so v < count */
    c00172c80_begin(0x100u, 0);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x104u, 0x2000u);
    guest_write32(0x108u, 0x108u);
    guest_write32(0x207Cu, 0x180000u);
    guest_write32(0x180140u, 0x200u);
    {
        static const uint32_t expect[7] = {0x183610u, 0x180000u, 0x109u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xD4D4D4D4u, 0x54545454u};
        static const uint32_t run1[] = {0x109u};
        static const c00172c80_run runs[] = {{0xF8u, sizeof run0, run0}, {0x108u, sizeof run1, run1}};
        c00172c80_check("alias: small stack, v is the a1 slot, count 0x200 so v < count", expect, runs, 2);
    }
    /* order: [object+0x7C] is the ESI slot, read before the push (sees R) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x3F80u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x2u);
    guest_write32(0x3FFCu, 0x180000u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x3u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x3u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("order: [object+0x7C] is the ESI slot, read before the push (sees R)", expect, runs, 2);
    }
    /* order: [object+0x7C] is the EDI slot, read before both pushes (sees R) */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x3F7Cu);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x180140u, 0x5u);
    guest_write32(0x2400u, 0x2u);
    guest_write32(0x3FF8u, 0x180000u);
    {
        static const uint32_t expect[7] = {0x1800D8u, 0x180000u, 0x3u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x3u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("order: [object+0x7C] is the EDI slot, read before both pushes (sees R)", expect, runs, 2);
    }
    /* push alias: count word is the pushed ESI slot (ESI = 6), v 9 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x3EBCu);
    guest_write32(0x2400u, 0x9u);
    g_esi = 0x6u;
    {
        static const uint32_t expect[7] = {0x4030u, 0x3EBCu, 0x0u, 0xB4B4B4B4u, 0x6u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x6u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("push alias: count word is the pushed ESI slot (ESI = 6), v 9", expect, runs, 2);
    }
    /* push alias: count word is the pushed EDI slot (EDI = 5), v 2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x3EB8u);
    guest_write32(0x2400u, 0x2u);
    g_edi = 0x5u;
    {
        static const uint32_t expect[7] = {0x3F90u, 0x3EB8u, 0x3u, 0xB4B4B4B4u, 0x54545454u, 0x5u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x3u};
        static const uint32_t run1[] = {0x5u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("push alias: count word is the pushed EDI slot (EDI = 5), v 2", expect, runs, 2);
    }
    /* push alias: count word is the pushed EDI slot (EDI sentinel negative), v 2 */
    c00172c80_begin(0x4000u, 0);
    guest_write32(0x4000u, 0x10000u);
    guest_write32(0x4004u, 0x2000u);
    guest_write32(0x4008u, 0x2400u);
    guest_write32(0x207Cu, 0x3EB8u);
    guest_write32(0x2400u, 0x2u);
    {
        static const uint32_t expect[7] = {0x3B3B7A04u, 0x3EB8u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const uint32_t run1[] = {0xD4D4D4D4u, 0x54545454u};
        static const c00172c80_run runs[] = {{0x2400u, sizeof run0, run0}, {0x3FF8u, sizeof run1, run1}};
        c00172c80_check("push alias: count word is the pushed EDI slot (EDI sentinel negative), v 2", expect, runs, 2);
    }

    /* TRAPPING inputs: the original raises #DE (zero divisor). The draft must trap too and must
     * not return normally. A forked child runs each state with SIGILL/SIGTRAP/SIGFPE handlers
     * that exit with a distinct code (41/42/43), the parent requires one of those codes (a
     * normal return exits 0, a wild access would die by signal 11 and fail the check). */
    {
        static const struct {
            uint32_t count;
            uint32_t index;
            int alias_count_word;
        } trap_cases[] = {
            {0u, 0u, 0}, {0u, 7u, 0}, {0u, 0xFFFFFFFFu, 0},
            {1u, 0u, 1}, {0x80000000u, 0u, 1},
        };
        for (size_t i = 0; i < sizeof trap_cases / sizeof trap_cases[0]; i++) {
            fflush(stderr);
            const int child = fork();
            if (child == 0) {
                signal(SIGILL, c00172c80_trap_ill);
                signal(SIGTRAP, c00172c80_trap_trap);
                signal(SIGFPE, c00172c80_trap_fpe);
                c00172c80_begin(0x4000u, 0);
                guest_write32(0x4004u, 0x2000u);
                guest_write32(0x2000u + 0x7cu, 0x180000u);
                guest_write32(0x180000u + 0x140u, trap_cases[i].count);
                if (trap_cases[i].alias_count_word) {
                    guest_write32(0x4008u, 0x180000u + 0x140u);
                } else {
                    guest_write32(0x4008u, 0x2400u);
                    guest_write32(0x2400u, trap_cases[i].index);
                }
                invoke(0x172C80u, 4u);
                _exit(0); /* returned normally: wrong */
            }
            int status = 0;
            CHECK(child > 0 && waitpid(child, &status, 0) == child);
            CHECK((status & 0x7f) == 0 && ((status >> 8) & 0xff) >= 41 && ((status >> 8) & 0xff) <= 43);
        }
    }
}
