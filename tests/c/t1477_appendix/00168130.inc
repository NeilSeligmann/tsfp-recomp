/* SPDX-License-Identifier: GPL-3.0-or-later
Independent control for 0x00168130 (derived from the original instructions only):
 ecx = a0; eax = [0x74C458]; edx = a1; ecx = ((ecx & 0xFFFF) << 9) + eax; eax = [ecx+0x1C0];
 edx = ~edx & eax; [ecx+0x1C0] = edx. Exit EAX = old, ECX = entry address, EDX = new value.
 No PUSH or POP exists in the original, so the POP-slot alias does not exist here. Alias
 cases: the stored dword landing on the base global, on the a0/a1 argument slots (no later
 reader, the store is last) and on the return slot. The census refusal reason ("store through
 a register that may alias the stack at 0x168152") is the last case: the original would
 return to the stored value, so the control stores the return address itself (an idempotent
 store, the only representable form) and checks ESP/memory. All expected values come from
 executing the ORIGINAL bytes in Unicorn on the named entry state (original only, never a
 draft) cross-checked against a Python model of the listing. Whole guest memory is diffed. */

#define C00168130_RET 0x00010000u

typedef struct {
    uint32_t address;
    uint32_t length;
    const void *bytes;
} c00168130_run;

static uint8_t c00168130_before[sizeof memory];
static uint8_t c00168130_model[sizeof memory];

/* Fresh zeroed guest memory, the standalone prepare() registers, the sentinels and DF. */
static void c00168130_begin(uint32_t esp, int df)
{
    memset(memory, 0, sizeof memory);
    prepare();
    g_esp = esp;
    guest_write32(g_esp, C00168130_RET);
    g_ebx = 0xB4B4B4B4u;
    g_esi = 0x54545454u;
    g_edi = 0xD4D4D4D4u;
    g_ebp = 0xBDBDBDBDu;
    g_df = df;
}

/* dword i of the run = first + i * step (modulo 2^32). */
static void __attribute__((unused)) c00168130_fill(uint32_t address, uint32_t count, uint32_t first, uint32_t step)
{
    for (uint32_t i = 0; i < count; i++) {
        guest_write32(address + 4u * i, first + i * step);
    }
}

/* Invoke, then compare EAX ECX EDX EBX ESI EDI EBP, DF, ESP (via invoke) and the whole guest
 * memory against the pre-call snapshot plus the listed byte runs (the original's writes). */
static void c00168130_check(const char *name, const uint32_t expect[7], const c00168130_run *runs, size_t run_count)
{
    const unsigned failures_before = failures;
    const int df = g_df;
    memcpy(c00168130_before, memory, sizeof memory);
    invoke(0x168130u, 4u);
    CHECK(g_eax == expect[0]);
    CHECK(g_ecx == expect[1]);
    CHECK(g_edx == expect[2]);
    CHECK(g_ebx == expect[3]);
    CHECK(g_esi == expect[4]);
    CHECK(g_edi == expect[5]);
    CHECK(g_ebp == expect[6]);
    CHECK(g_df == df);
    memcpy(c00168130_model, c00168130_before, sizeof memory);
    for (size_t i = 0; i < run_count; i++) {
        memcpy(&c00168130_model[runs[i].address], runs[i].bytes, runs[i].length);
    }
    int memory_equal = 1;
    for (size_t i = 0; i < sizeof memory; i++) {
        if (c00168130_model[i] != memory[i]) {
            memory_equal = 0;
            fprintf(stderr, "memory differs at 0x%zX: got 0x%02X expected 0x%02X\n", i, memory[i],
                    c00168130_model[i]);
            break;
        }
    }
    CHECK(memory_equal);
    if (failures != failures_before) {
        fprintf(stderr, "failing case: %s\n", name);
    }
}

static void test_t1477_appendix_00168130(void)
{
    /* id 0x0 mask 0x0 old 0xffffffff */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3001B8u, 0x770000F8u);
    guest_write32(0x3001BCu, 0x770000FCu);
    guest_write32(0x3001C4u, 0x77000004u);
    guest_write32(0x3001C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3001C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0x0u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300000u, 0xFFFFFFFFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("id 0x0 mask 0x0 old 0xffffffff", expect, NULL, 0);
    }
    /* id 0x0 mask 0xffffffff old 0xffffffff */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3001B8u, 0x770000F8u);
    guest_write32(0x3001BCu, 0x770000FCu);
    guest_write32(0x3001C4u, 0x77000004u);
    guest_write32(0x3001C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3001C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const c00168130_run runs[] = {{0x3001C0u, sizeof run0, run0}};
        c00168130_check("id 0x0 mask 0xffffffff old 0xffffffff", expect, runs, 1);
    }
    /* id 0x1 mask 0x1 old 0x80000001 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3003B8u, 0x770000F8u);
    guest_write32(0x3003BCu, 0x770000FCu);
    guest_write32(0x3003C4u, 0x77000004u);
    guest_write32(0x3003C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3003C0u, 0x80000001u);
    guest_write32(0x104u, 0x1u);
    guest_write32(0x108u, 0x1u);
    {
        static const uint32_t expect[7] = {0x80000001u, 0x300200u, 0x80000000u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x80000000u};
        static const c00168130_run runs[] = {{0x3003C0u, sizeof run0, run0}};
        c00168130_check("id 0x1 mask 0x1 old 0x80000001", expect, runs, 1);
    }
    /* id 0x2 mask 0x80000000 old 0xffffffff */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3005B8u, 0x770000F8u);
    guest_write32(0x3005BCu, 0x770000FCu);
    guest_write32(0x3005C4u, 0x77000004u);
    guest_write32(0x3005C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3005C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x2u);
    guest_write32(0x108u, 0x80000000u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300400u, 0x7FFFFFFFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x7FFFFFFFu};
        static const c00168130_run runs[] = {{0x3005C0u, sizeof run0, run0}};
        c00168130_check("id 0x2 mask 0x80000000 old 0xffffffff", expect, runs, 1);
    }
    /* id 0x7f mask 0x800100 old 0xa5a5a5a5 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x30FFB8u, 0x770000F8u);
    guest_write32(0x30FFBCu, 0x770000FCu);
    guest_write32(0x30FFC4u, 0x77000004u);
    guest_write32(0x30FFC8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x30FFC0u, 0xA5A5A5A5u);
    guest_write32(0x104u, 0x7Fu);
    guest_write32(0x108u, 0x800100u);
    {
        static const uint32_t expect[7] = {0xA5A5A5A5u, 0x30FE00u, 0xA525A4A5u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xA525A4A5u};
        static const c00168130_run runs[] = {{0x30FFC0u, sizeof run0, run0}};
        c00168130_check("id 0x7f mask 0x800100 old 0xa5a5a5a5", expect, runs, 1);
    }
    /* id 0x3 mask 0x7fffffff old 0xffffffff */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3007B8u, 0x770000F8u);
    guest_write32(0x3007BCu, 0x770000FCu);
    guest_write32(0x3007C4u, 0x77000004u);
    guest_write32(0x3007C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3007C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x3u);
    guest_write32(0x108u, 0x7FFFFFFFu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300600u, 0x80000000u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x80000000u};
        static const c00168130_run runs[] = {{0x3007C0u, sizeof run0, run0}};
        c00168130_check("id 0x3 mask 0x7fffffff old 0xffffffff", expect, runs, 1);
    }
    /* id 0x3 mask 0xffff0000 old 0xffffffff */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3007B8u, 0x770000F8u);
    guest_write32(0x3007BCu, 0x770000FCu);
    guest_write32(0x3007C4u, 0x77000004u);
    guest_write32(0x3007C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3007C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x3u);
    guest_write32(0x108u, 0xFFFF0000u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300600u, 0xFFFFu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFu};
        static const c00168130_run runs[] = {{0x3007C0u, sizeof run0, run0}};
        c00168130_check("id 0x3 mask 0xffff0000 old 0xffffffff", expect, runs, 1);
    }
    /* id 0x5 mask 0x100 old 0x100 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x300BB8u, 0x770000F8u);
    guest_write32(0x300BBCu, 0x770000FCu);
    guest_write32(0x300BC4u, 0x77000004u);
    guest_write32(0x300BC8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x300BC0u, 0x100u);
    guest_write32(0x104u, 0x5u);
    guest_write32(0x108u, 0x100u);
    {
        static const uint32_t expect[7] = {0x100u, 0x300A00u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const c00168130_run runs[] = {{0x300BC0u, sizeof run0, run0}};
        c00168130_check("id 0x5 mask 0x100 old 0x100", expect, runs, 1);
    }
    /* id 0x5 mask 0x100 old 0x0 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x300BB8u, 0x770000F8u);
    guest_write32(0x300BBCu, 0x770000FCu);
    guest_write32(0x300BC4u, 0x77000004u);
    guest_write32(0x300BC8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x300BC0u, 0x0u);
    guest_write32(0x104u, 0x5u);
    guest_write32(0x108u, 0x100u);
    {
        static const uint32_t expect[7] = {0x0u, 0x300A00u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("id 0x5 mask 0x100 old 0x0", expect, NULL, 0);
    }
    /* id 0x1f mask 0xffffffff old 0x0 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x303FB8u, 0x770000F8u);
    guest_write32(0x303FBCu, 0x770000FCu);
    guest_write32(0x303FC4u, 0x77000004u);
    guest_write32(0x303FC8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x303FC0u, 0x0u);
    guest_write32(0x104u, 0x1Fu);
    guest_write32(0x108u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x0u, 0x303E00u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("id 0x1f mask 0xffffffff old 0x0", expect, NULL, 0);
    }
    /* id 0x20 mask 0x0 old 0x0 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3041B8u, 0x770000F8u);
    guest_write32(0x3041BCu, 0x770000FCu);
    guest_write32(0x3041C4u, 0x77000004u);
    guest_write32(0x3041C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3041C0u, 0x0u);
    guest_write32(0x104u, 0x20u);
    guest_write32(0x108u, 0x0u);
    {
        static const uint32_t expect[7] = {0x0u, 0x304000u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("id 0x20 mask 0x0 old 0x0", expect, NULL, 0);
    }
    /* id 0x10003 uses only the low 16 bits */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3007B8u, 0x770000F8u);
    guest_write32(0x3007BCu, 0x770000FCu);
    guest_write32(0x3007C4u, 0x77000004u);
    guest_write32(0x3007C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3007C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x10003u);
    guest_write32(0x108u, 0xF0F0F0Fu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300600u, 0xF0F0F0F0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xF0F0F0F0u};
        static const c00168130_run runs[] = {{0x3007C0u, sizeof run0, run0}};
        c00168130_check("id 0x10003 uses only the low 16 bits", expect, runs, 1);
    }
    /* id 0xffff0003 uses only the low 16 bits */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3007B8u, 0x770000F8u);
    guest_write32(0x3007BCu, 0x770000FCu);
    guest_write32(0x3007C4u, 0x77000004u);
    guest_write32(0x3007C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3007C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0xFFFF0003u);
    guest_write32(0x108u, 0xF0F0F0Fu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300600u, 0xF0F0F0F0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xF0F0F0F0u};
        static const c00168130_run runs[] = {{0x3007C0u, sizeof run0, run0}};
        c00168130_check("id 0xffff0003 uses only the low 16 bits", expect, runs, 1);
    }
    /* id 0x12340003 uses only the low 16 bits */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3007B8u, 0x770000F8u);
    guest_write32(0x3007BCu, 0x770000FCu);
    guest_write32(0x3007C4u, 0x77000004u);
    guest_write32(0x3007C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3007C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x12340003u);
    guest_write32(0x108u, 0xF0F0F0Fu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300600u, 0xF0F0F0F0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xF0F0F0F0u};
        static const c00168130_run runs[] = {{0x3007C0u, sizeof run0, run0}};
        c00168130_check("id 0x12340003 uses only the low 16 bits", expect, runs, 1);
    }
    /* id 0x80000003 uses only the low 16 bits */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3007B8u, 0x770000F8u);
    guest_write32(0x3007BCu, 0x770000FCu);
    guest_write32(0x3007C4u, 0x77000004u);
    guest_write32(0x3007C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3007C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x80000003u);
    guest_write32(0x108u, 0xF0F0F0Fu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x300600u, 0xF0F0F0F0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xF0F0F0F0u};
        static const c00168130_run runs[] = {{0x3007C0u, sizeof run0, run0}};
        c00168130_check("id 0x80000003 uses only the low 16 bits", expect, runs, 1);
    }
    /* id 0x3FFF base 0 (highest id inside the 8 MB guest window) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x7FFFB8u, 0x770000F8u);
    guest_write32(0x7FFFBCu, 0x770000FCu);
    guest_write32(0x7FFFC4u, 0x77000004u);
    guest_write32(0x7FFFC8u, 0x77000008u);
    guest_write32(0x74C458u, 0x0u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7FFFC0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x3FFFu);
    guest_write32(0x108u, 0xF0u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x7FFE00u, 0xFFFFFF0Fu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFFF0Fu};
        static const c00168130_run runs[] = {{0x7FFFC0u, sizeof run0, run0}};
        c00168130_check("id 0x3FFF base 0 (highest id inside the 8 MB guest window)", expect, runs, 1);
    }
    /* id 0 base 0 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x1B8u, 0x770000F8u);
    guest_write32(0x1BCu, 0x770000FCu);
    guest_write32(0x1C4u, 0x77000004u);
    guest_write32(0x1C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x0u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x1C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0xF0u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x0u, 0xFFFFFF0Fu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFFF0Fu};
        static const c00168130_run runs[] = {{0x1C0u, sizeof run0, run0}};
        c00168130_check("id 0 base 0", expect, runs, 1);
    }
    /* 32 bit wrap base 0xFFF00000 id 0x1000 */
    c00168130_begin(0x100u, 0);
    guest_write32(0x1001B8u, 0x770000F8u);
    guest_write32(0x1001BCu, 0x770000FCu);
    guest_write32(0x1001C4u, 0x77000004u);
    guest_write32(0x1001C8u, 0x77000008u);
    guest_write32(0x74C458u, 0xFFF00000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x1001C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x1000u);
    guest_write32(0x108u, 0x3u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x100000u, 0xFFFFFFFCu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFFFFCu};
        static const c00168130_run runs[] = {{0x1001C0u, sizeof run0, run0}};
        c00168130_check("32 bit wrap base 0xFFF00000 id 0x1000", expect, runs, 1);
    }
    /* 32 bit wrap of entry+0x1C0 to address 0 (base 0xFFFFFE40) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x4u, 0x77000004u);
    guest_write32(0x8u, 0x77000008u);
    guest_write32(0x74C458u, 0xFFFFFE40u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0xFF0u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0xFFFFFE40u, 0xFFFFF00Fu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFF00Fu};
        static const c00168130_run runs[] = {{0x0u, sizeof run0, run0}};
        c00168130_check("32 bit wrap of entry+0x1C0 to address 0 (base 0xFFFFFE40)", expect, runs, 1);
    }
    /* 32 bit wrap of entry+0x1C0 to address 4 (base 0xFFFFFE44 id 0) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x0u, 0x770000FCu);
    guest_write32(0x8u, 0x77000004u);
    guest_write32(0xCu, 0x77000008u);
    guest_write32(0x74C458u, 0xFFFFFE44u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x4u, 0xDEADBEEFu);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0x1u);
    {
        static const uint32_t expect[7] = {0xDEADBEEFu, 0xFFFFFE44u, 0xDEADBEEEu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xDEADBEEEu};
        static const c00168130_run runs[] = {{0x4u, sizeof run0, run0}};
        c00168130_check("32 bit wrap of entry+0x1C0 to address 4 (base 0xFFFFFE44 id 0)", expect, runs, 1);
    }
    /* id 0x8003: bit 15 counts (base 0xFF000000 wraps the entry to 0x600) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x7B8u, 0x770000F8u);
    guest_write32(0x7BCu, 0x770000FCu);
    guest_write32(0x7C4u, 0x77000004u);
    guest_write32(0x7C8u, 0x77000008u);
    guest_write32(0x74C458u, 0xFF000000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x7C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0x8003u);
    guest_write32(0x108u, 0xFu);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x600u, 0xFFFFFFF0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFFFFF0u};
        static const c00168130_run runs[] = {{0x7C0u, sizeof run0, run0}};
        c00168130_check("id 0x8003: bit 15 counts (base 0xFF000000 wraps the entry to 0x600)", expect, runs, 1);
    }
    /* id 0xFFFF: all 16 bits count (base 0xFE000300 wraps the entry to 0x100) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x2B8u, 0x770000F8u);
    guest_write32(0x2BCu, 0x770000FCu);
    guest_write32(0x2C4u, 0x77000004u);
    guest_write32(0x2C8u, 0x77000008u);
    guest_write32(0x74C458u, 0xFE000300u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x2C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0xFFFFu);
    guest_write32(0x108u, 0xF0F0u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x100u, 0xFFFF0F0Fu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFF0F0Fu};
        static const c00168130_run runs[] = {{0x2C0u, sizeof run0, run0}};
        c00168130_check("id 0xFFFF: all 16 bits count (base 0xFE000300 wraps the entry to 0x100)", expect, runs, 1);
    }
    /* id 0xFFFF0000 masks to entry 0 (base 0x100) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x2B8u, 0x770000F8u);
    guest_write32(0x2BCu, 0x770000FCu);
    guest_write32(0x2C4u, 0x77000004u);
    guest_write32(0x2C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x100u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x2C0u, 0xFFFFFFFFu);
    guest_write32(0x104u, 0xFFFF0000u);
    guest_write32(0x108u, 0xF0F0u);
    {
        static const uint32_t expect[7] = {0xFFFFFFFFu, 0x100u, 0xFFFF0F0Fu, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xFFFF0F0Fu};
        static const c00168130_run runs[] = {{0x2C0u, sizeof run0, run0}};
        c00168130_check("id 0xFFFF0000 masks to entry 0 (base 0x100)", expect, runs, 1);
    }
    /* DF set (unused) */
    c00168130_begin(0x100u, 1);
    guest_write32(0x3009B8u, 0x770000F8u);
    guest_write32(0x3009BCu, 0x770000FCu);
    guest_write32(0x3009C4u, 0x77000004u);
    guest_write32(0x3009C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3009C0u, 0xFFu);
    guest_write32(0x104u, 0x4u);
    guest_write32(0x108u, 0x6u);
    {
        static const uint32_t expect[7] = {0xFFu, 0x300800u, 0xF9u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0xF9u};
        static const c00168130_run runs[] = {{0x3009C0u, sizeof run0, run0}};
        c00168130_check("DF set (unused)", expect, runs, 1);
    }
    /* EBX ESI EDI EBP custom sentinels */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3009B8u, 0x770000F8u);
    guest_write32(0x3009BCu, 0x770000FCu);
    guest_write32(0x3009C4u, 0x77000004u);
    guest_write32(0x3009C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300000u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3009C0u, 0xFFu);
    guest_write32(0x104u, 0x4u);
    guest_write32(0x108u, 0x6u);
    g_eax = 0x5u;
    g_ecx = 0x4u;
    g_edx = 0x3u;
    g_ebx = 0x9u;
    g_esi = 0x8u;
    g_edi = 0x7u;
    g_ebp = 0x6u;
    {
        static const uint32_t expect[7] = {0xFFu, 0x300800u, 0xF9u, 0x9u, 0x8u, 0x7u, 0x6u};
        static const uint32_t run0[] = {0xF9u};
        static const c00168130_run runs[] = {{0x3009C0u, sizeof run0, run0}};
        c00168130_check("EBX ESI EDI EBP custom sentinels", expect, runs, 1);
    }
    /* old value planted zero, mask all ones, base unaligned stride neighbour */
    c00168130_begin(0x100u, 0);
    guest_write32(0x3003C8u, 0x770000F8u);
    guest_write32(0x3003CCu, 0x770000FCu);
    guest_write32(0x3003D4u, 0x77000004u);
    guest_write32(0x3003D8u, 0x77000008u);
    guest_write32(0x74C458u, 0x300010u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x3003D0u, 0x0u);
    guest_write32(0x104u, 0x1u);
    guest_write32(0x108u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x0u, 0x300210u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("old value planted zero, mask all ones, base unaligned stride neighbour", expect, NULL, 0);
    }
    /* alias: entry+0x1C0 is the base global itself (old = base, new = base & ~mask) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x74C450u, 0x770000F8u);
    guest_write32(0x74C454u, 0x770000FCu);
    guest_write32(0x74C45Cu, 0x77000004u);
    guest_write32(0x74C460u, 0x77000008u);
    guest_write32(0x74C458u, 0x74C298u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0xF00u);
    {
        static const uint32_t expect[7] = {0x74C298u, 0x74C298u, 0x74C098u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x74C098u};
        static const c00168130_run runs[] = {{0x74C458u, sizeof run0, run0}};
        c00168130_check("alias: entry+0x1C0 is the base global itself (old = base, new = base & ~mask)", expect, runs, 1);
    }
    /* alias: same with mask 0xFFFFFFFF (global becomes 0) */
    c00168130_begin(0x100u, 0);
    guest_write32(0x74C450u, 0x770000F8u);
    guest_write32(0x74C454u, 0x770000FCu);
    guest_write32(0x74C45Cu, 0x77000004u);
    guest_write32(0x74C460u, 0x77000008u);
    guest_write32(0x74C458u, 0x74C298u);
    guest_write32(0x100u, 0x10000u);
    guest_write32(0x104u, 0x0u);
    guest_write32(0x108u, 0xFFFFFFFFu);
    {
        static const uint32_t expect[7] = {0x74C298u, 0x74C298u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const c00168130_run runs[] = {{0x74C458u, sizeof run0, run0}};
        c00168130_check("alias: same with mask 0xFFFFFFFF (global becomes 0)", expect, runs, 1);
    }
    /* alias: entry+0x1C0 is the a1 slot (old = mask, stores 0) */
    c00168130_begin(0x13B8u, 0);
    guest_write32(0x13B8u, 0x770000F8u);
    guest_write32(0x13BCu, 0x770000FCu);
    guest_write32(0x13C4u, 0x77000004u);
    guest_write32(0x13C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x1000u);
    guest_write32(0x13B8u, 0x10000u);
    guest_write32(0x13BCu, 0x1u);
    guest_write32(0x13C0u, 0xFF00u);
    {
        static const uint32_t expect[7] = {0xFF00u, 0x1200u, 0x0u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        static const uint32_t run0[] = {0x0u};
        static const c00168130_run runs[] = {{0x13C0u, sizeof run0, run0}};
        c00168130_check("alias: entry+0x1C0 is the a1 slot (old = mask, stores 0)", expect, runs, 1);
    }
    /* alias: entry+0x1C0 is the a0 slot (old = id, stores id & ~mask) */
    c00168130_begin(0x13BCu, 0);
    guest_write32(0x13B8u, 0x770000F8u);
    guest_write32(0x13BCu, 0x770000FCu);
    guest_write32(0x13C4u, 0x77000004u);
    guest_write32(0x13C8u, 0x77000008u);
    guest_write32(0x74C458u, 0x1000u);
    guest_write32(0x13BCu, 0x10000u);
    guest_write32(0x13C0u, 0x1u);
    guest_write32(0x13C4u, 0xFFFFFFF0u);
    {
        static const uint32_t expect[7] = {0x1u, 0x1200u, 0x1u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("alias: entry+0x1C0 is the a0 slot (old = id, stores id & ~mask)", expect, NULL, 0);
    }
    /* alias: entry+0x1C0 is the return slot, idempotent store of the return address */
    c00168130_begin(0x13C0u, 0);
    guest_write32(0x74C458u, 0x1000u);
    guest_write32(0x13C0u, 0x10000u);
    guest_write32(0x13C4u, 0x1u);
    guest_write32(0x13C8u, 0x1u);
    {
        static const uint32_t expect[7] = {0x10000u, 0x1200u, 0x10000u, 0xB4B4B4B4u, 0x54545454u, 0xD4D4D4D4u, 0xBDBDBDBDu};
        c00168130_check("alias: entry+0x1C0 is the return slot, idempotent store of the return address", expect, NULL, 0);
    }
}
