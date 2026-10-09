/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x0010E640 (game_record_set_field_0x88_flag), written from the
 * original listing, not from the draft:
 *   0010E640 mov eax,[esp+4]      ; obj
 *   0010E644 test eax,eax
 *   0010E646 je 0010E659          ; null: EAX = 0, ECX/EDX untouched, nothing written
 *   0010E648 mov ecx,[esp+8]      ; value
 *   0010E64C mov [eax+0x88],ecx
 *   0010E652 or dword ptr [eax+4],0x100000   ; read-modify-write after the 0x88 store
 *   0010E659 ret                  ; cdecl, plain ret, pops 4 through invoke
 * Exit: obj != 0 -> EAX = obj, ECX = value, EDX unchanged; obj == 0 -> EAX = 0, ECX = 0x22222222,
 * EDX = 0x33333333. EBX/ESI/EDI/EBP/DF unchanged.
 * No guest PUSH or POP in the original. The alias controls place +4 on the argument slots and the
 * return slot (the OR modifies a stack dword after the arguments were read) and +0x88 on the
 * return slot and the argument slots (the value store overwrites them), and wrap the 32 bit
 * address arithmetic (obj = 0xFFFFFFFC puts +4 at 0 and +0x88 at 0x84). */

static uint8_t c0010e640_before[sizeof memory];

static void c0010e640_clear(void)
{
    memset(memory, 0, 0x1000u);
    memset(&memory[0x1F000u], 0, 0x3000u);
}

static void c0010e640_sentinels(void)
{
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    g_df = 0;
}

static void c0010e640_check_kept(void)
{
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_df == 0);
}

/* Expected guest write for the whole-memory diff: patch the pre-call snapshot. */
static void c0010e640_expect32(uint32_t address, uint32_t value)
{
    memcpy(&c0010e640_before[address], &value, 4u);
}

static int c0010e640_memory_matches(void)
{
    return memcmp(c0010e640_before, memory, sizeof memory) == 0;
}

/* Prepare the frame (return slot 0x12345678 at ESP = 0x100, arguments at 0x104, 0x108, 0x10C),
 * the sentinels, and snapshot memory. Pre-state the test wrote before this call survives. */
static void c0010e640_enter(uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    prepare();
    guest_write32(g_esp + 4u, arg0);
    guest_write32(g_esp + 8u, arg1);
    guest_write32(g_esp + 12u, arg2);
    c0010e640_sentinels();
    memcpy(c0010e640_before, memory, sizeof memory);
}

typedef struct C0010E640Alias {
    uint32_t arg[3];
    unsigned pre_count;
    struct { uint32_t address, value; } pre[4];
    uint32_t eax, ecx, edx;
    unsigned watch_count;
    struct { uint32_t address, value; } watch[10];
} C0010E640Alias;

/* Alias states. Expected values were produced by EXECUTING THE ORIGINAL BYTES of 0x0010E640 in
 * Unicorn 2.1.4 (tmp/c96/c96_oracle.py, tmp/c96/c96_gen.py: original only, never the draft) on
 * these named entry states. Memory is zero except the listed pre writes (applied after the
 * arguments, so they may overwrite stack slots on purpose), ESP = 0x100, [0x100] = 0x12345678,
 * EAX/ECX/EDX = 0x11111111/0x22222222/0x33333333, EBX/ESI/EDI/EBP sentinels. watch lists every
 * dword whose final value the original produced (stack slots 0x100..0x10C plus the pre
 * addresses and aliased targets), the rest of memory must be unchanged. */
static const C0010E640Alias c0010e640_alias_table[] = {
    {{0x100U, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0x100U, 0xCAFEF00DU, 0x33333333U, 5, {{0x100U, 0x12245678U}, {0x104U, 0x100100U}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}, {0x188U, 0xCAFEF00DU}}},
    {{0x104U, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0x104U, 0xCAFEF00DU, 0x33333333U, 5, {{0x100U, 0x12245678U}, {0x104U, 0x104U}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}, {0x18CU, 0xCAFEF00DU}}},
    {{0xFCU, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0xFCU, 0xCAFEF00DU, 0x33333333U, 5, {{0x100U, 0x12345678U}, {0x104U, 0xFCU}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}, {0x184U, 0xCAFEF00DU}}},
    {{0x78U, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0x78U, 0xCAFEF00DU, 0x33333333U, 5, {{0x7CU, 0x100000U}, {0x100U, 0xCAFEF00DU}, {0x104U, 0x78U}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}}},
    {{0x7CU, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0x7CU, 0xCAFEF00DU, 0x33333333U, 5, {{0x80U, 0x100000U}, {0x100U, 0x12245678U}, {0x104U, 0xCAFEF00DU}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}}},
    {{0x80U, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0x80U, 0xCAFEF00DU, 0x33333333U, 5, {{0x84U, 0x100000U}, {0x100U, 0x12245678U}, {0x104U, 0x80U}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}}},
    {{0xFFFFFFFCU, 0xCAFEF00DU, 0x0U}, 1, {{0x100U, 0x12245678U}}, 0xFFFFFFFCU, 0xCAFEF00DU, 0x33333333U, 6, {{0x0U, 0x100000U}, {0x84U, 0xCAFEF00DU}, {0x100U, 0x12245678U}, {0x104U, 0xFFFFFFFCU}, {0x108U, 0xCAFEF00DU}, {0x10CU, 0x0U}}},
};

static void c0010e640_run_alias(const C0010E640Alias *state)
{
    unsigned index;
    c0010e640_clear();
    prepare();
    guest_write32(g_esp + 4u, state->arg[0]);
    guest_write32(g_esp + 8u, state->arg[1]);
    guest_write32(g_esp + 12u, state->arg[2]);
    for (index = 0; index < state->pre_count; ++index) {
        guest_write32(state->pre[index].address, state->pre[index].value);
    }
    c0010e640_sentinels();
    memcpy(c0010e640_before, memory, sizeof memory);
    invoke(0x0010E640U, 4u);
    CHECK(g_eax == state->eax);
    CHECK(g_ecx == state->ecx);
    CHECK(g_edx == state->edx);
    c0010e640_check_kept();
    for (index = 0; index < state->watch_count; ++index) {
        c0010e640_expect32(state->watch[index].address, state->watch[index].value);
    }
    CHECK(c0010e640_memory_matches());
}


typedef struct C0010E640Flags {
    uint32_t before, after;
} C0010E640Flags;

static void test_t1477_appendix_0010e640(void)
{
    static const uint32_t values[] = {0u, 1u, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x3F800000u};
    static const C0010E640Flags flags[] = {
        {0x00000000u, 0x00100000u},
        {0x00100000u, 0x00100000u},
        {0xFFEFFFFFu, 0xFFFFFFFFu},
        {0xFFFFFFFFu, 0xFFFFFFFFu},
        {0x000FFFFFu, 0x001FFFFFu},
        {0x80000000u, 0x80100000u},
        {0x00200000u, 0x00300000u},
    };
    const uint32_t obj = 0x20000u;
    unsigned value_index;
    unsigned flag_index;
    unsigned index;

    for (value_index = 0; value_index < sizeof values / sizeof values[0]; ++value_index) {
        for (flag_index = 0; flag_index < sizeof flags / sizeof flags[0]; ++flag_index) {
            c0010e640_clear();
            guest_write32(obj, 0x01010101u);
            guest_write32(obj + 4u, flags[flag_index].before);
            guest_write32(obj + 8u, 0x02020202u);
            guest_write32(obj + 0x84u, 0x03030303u);
            guest_write32(obj + 0x88u, 0x04040404u);
            guest_write32(obj + 0x8Cu, 0x05050505u);
            c0010e640_enter(obj, values[value_index], 0u);
            invoke(0x0010E640u, 4u);
            CHECK(g_eax == obj && g_ecx == values[value_index] && g_edx == 0x33333333u);
            c0010e640_check_kept();
            c0010e640_expect32(obj + 4u, flags[flag_index].after);
            c0010e640_expect32(obj + 0x88u, values[value_index]);
            CHECK(c0010e640_memory_matches());
        }
    }

    /* null object: nothing at 4 or 0x88 is touched, ECX/EDX are the entry values */
    c0010e640_clear();
    guest_write32(4u, 0x0A0A0A0Au);
    guest_write32(0x88u, 0x0B0B0B0Bu);
    c0010e640_enter(0u, 0xFFFFFFFFu, 0u);
    invoke(0x0010E640u, 4u);
    CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    c0010e640_check_kept();
    CHECK(guest_read32(4u) == 0x0A0A0A0Au && guest_read32(0x88u) == 0x0B0B0B0Bu);
    CHECK(c0010e640_memory_matches());

    /* unaligned object: both dwords straddle aligned dwords */
    c0010e640_clear();
    memset(&memory[0x20000u], 0, 0x100u);
    c0010e640_enter(0x20001u, 0xA1B2C3D4u, 0u);
    invoke(0x0010E640u, 4u);
    CHECK(g_eax == 0x20001u && g_ecx == 0xA1B2C3D4u);
    c0010e640_expect32(0x20005u, 0x00100000u);
    c0010e640_expect32(0x20089u, 0xA1B2C3D4u);
    CHECK(c0010e640_memory_matches());

    for (index = 0; index < sizeof c0010e640_alias_table / sizeof c0010e640_alias_table[0]; ++index) {
        c0010e640_run_alias(&c0010e640_alias_table[index]);
    }
}
