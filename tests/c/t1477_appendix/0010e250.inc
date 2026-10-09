/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x0010E250 (game_record_modify_flags_0x8), written from the original
 * listing, not from the draft:
 *   0010E250 mov eax,[esp+4]      ; obj
 *   0010E254 test eax,eax
 *   0010E256 je 0010E26A          ; null: return with EAX = 0, ECX/EDX untouched
 *   0010E258 mov ecx,[esp+0xC]    ; mask
 *   0010E25C mov edx,[eax+8]      ; old
 *   0010E25F not ecx
 *   0010E261 and ecx,edx          ; old & ~mask
 *   0010E263 or ecx,[esp+8]       ; | set  (read after [obj+8], before the store)
 *   0010E267 mov [eax+8],ecx
 *   0010E26A ret                  ; cdecl, plain ret, pops 4 through invoke
 * Exit: obj != 0 -> EAX = obj, ECX = new flags, EDX = old; obj == 0 -> EAX = 0, ECX = 0x22222222,
 * EDX = 0x33333333 (entry values). EBX/ESI/EDI/EBP/DF unchanged.
 * No guest PUSH or POP in the original. The alias controls place [obj+8] on the return slot,
 * the object slot, the set slot, the mask slot and a slot above the frame (the only store is the
 * last instruction, the set argument is read between the [obj+8] read and the store), and wrap
 * the address at 32 bits (obj = 0xFFFFFFF8 and 0xFFFFFFFC). The function has no compare, the
 * bit patterns below straddle the sign bit and test set versus mask roles. */

static uint8_t c0010e250_before[sizeof memory];

static void c0010e250_clear(void)
{
    memset(memory, 0, 0x1000u);
    memset(&memory[0x1F000u], 0, 0x3000u);
}

static void c0010e250_sentinels(void)
{
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    g_df = 0;
}

static void c0010e250_check_kept(void)
{
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_df == 0);
}

/* Expected guest write for the whole-memory diff: patch the pre-call snapshot. */
static void c0010e250_expect32(uint32_t address, uint32_t value)
{
    memcpy(&c0010e250_before[address], &value, 4u);
}

static int c0010e250_memory_matches(void)
{
    return memcmp(c0010e250_before, memory, sizeof memory) == 0;
}

/* Prepare the frame (return slot 0x12345678 at ESP = 0x100, arguments at 0x104, 0x108, 0x10C),
 * the sentinels, and snapshot memory. Pre-state the test wrote before this call survives. */
static void c0010e250_enter(uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    prepare();
    guest_write32(g_esp + 4u, arg0);
    guest_write32(g_esp + 8u, arg1);
    guest_write32(g_esp + 12u, arg2);
    c0010e250_sentinels();
    memcpy(c0010e250_before, memory, sizeof memory);
}

typedef struct C0010E250Alias {
    uint32_t arg[3];
    unsigned pre_count;
    struct { uint32_t address, value; } pre[4];
    uint32_t eax, ecx, edx;
    unsigned watch_count;
    struct { uint32_t address, value; } watch[10];
} C0010E250Alias;

/* Alias states. Expected values were produced by EXECUTING THE ORIGINAL BYTES of 0x0010E250 in
 * Unicorn 2.1.4 (tmp/c96/c96_oracle.py, tmp/c96/c96_gen.py: original only, never the draft) on
 * these named entry states. Memory is zero except the listed pre writes (applied after the
 * arguments, so they may overwrite stack slots on purpose), ESP = 0x100, [0x100] = 0x12345678,
 * EAX/ECX/EDX = 0x11111111/0x22222222/0x33333333, EBX/ESI/EDI/EBP sentinels. watch lists every
 * dword whose final value the original produced (stack slots 0x100..0x10C plus the pre
 * addresses and aliased targets), the rest of memory must be unchanged. */
static const C0010E250Alias c0010e250_alias_table[] = {
    {{0x100U, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0x100U, 0xF0F0000U, 0xF0F0000U, 4, {{0x100U, 0x12345678U}, {0x104U, 0x100U}, {0x108U, 0xF0F0000U}, {0x10CU, 0xFF00FFU}}},
    {{0x104U, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0x104U, 0xF0F0000U, 0xFF00FFU, 4, {{0x100U, 0x12345678U}, {0x104U, 0x104U}, {0x108U, 0xF0F0000U}, {0x10CU, 0xF0F0000U}}},
    {{0x108U, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0x108U, 0xF0F0000U, 0x0U, 5, {{0x100U, 0x12345678U}, {0x104U, 0x108U}, {0x108U, 0xF0F0000U}, {0x10CU, 0xFF00FFU}, {0x110U, 0xF0F0000U}}},
    {{0xFCU, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0xFCU, 0xF0F0000U, 0xFCU, 4, {{0x100U, 0x12345678U}, {0x104U, 0xF0F0000U}, {0x108U, 0xF0F0000U}, {0x10CU, 0xFF00FFU}}},
    {{0xF8U, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0xF8U, 0x1F0F5600U, 0x12345678U, 4, {{0x100U, 0x1F0F5600U}, {0x104U, 0xF8U}, {0x108U, 0xF0F0000U}, {0x10CU, 0xFF00FFU}}},
    {{0xF4U, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0xF4U, 0xF0F0000U, 0x0U, 5, {{0xFCU, 0xF0F0000U}, {0x100U, 0x12345678U}, {0x104U, 0xF4U}, {0x108U, 0xF0F0000U}, {0x10CU, 0xFF00FFU}}},
    {{0xFFFFFFFCU, 0xF0F0000U, 0xFF00FFU}, 0, {}, 0xFFFFFFFCU, 0xF0F0000U, 0x0U, 5, {{0x4U, 0xF0F0000U}, {0x100U, 0x12345678U}, {0x104U, 0xFFFFFFFCU}, {0x108U, 0xF0F0000U}, {0x10CU, 0xFF00FFU}}},
    {{0xF8U, 0x1U, 0x80000000U}, 1, {{0x100U, 0xFFFFFFFFU}}, 0xF8U, 0x7FFFFFFFU, 0xFFFFFFFFU, 4, {{0x100U, 0x7FFFFFFFU}, {0x104U, 0xF8U}, {0x108U, 0x1U}, {0x10CU, 0x80000000U}}},
    {{0x100U, 0xFU, 0xF0U}, 1, {{0x108U, 0xFFU}}, 0x100U, 0xFFU, 0xFFU, 4, {{0x100U, 0x12345678U}, {0x104U, 0x100U}, {0x108U, 0xFFU}, {0x10CU, 0xF0U}}},
    {{0xFCU, 0xF0000000U, 0xFFFFFFFFU}, 0, {}, 0xFCU, 0xF0000000U, 0xFCU, 4, {{0x100U, 0x12345678U}, {0x104U, 0xF0000000U}, {0x108U, 0xF0000000U}, {0x10CU, 0xFFFFFFFFU}}},
};

static void c0010e250_run_alias(const C0010E250Alias *state)
{
    unsigned index;
    c0010e250_clear();
    prepare();
    guest_write32(g_esp + 4u, state->arg[0]);
    guest_write32(g_esp + 8u, state->arg[1]);
    guest_write32(g_esp + 12u, state->arg[2]);
    for (index = 0; index < state->pre_count; ++index) {
        guest_write32(state->pre[index].address, state->pre[index].value);
    }
    c0010e250_sentinels();
    memcpy(c0010e250_before, memory, sizeof memory);
    invoke(0x0010E250U, 4u);
    CHECK(g_eax == state->eax);
    CHECK(g_ecx == state->ecx);
    CHECK(g_edx == state->edx);
    c0010e250_check_kept();
    for (index = 0; index < state->watch_count; ++index) {
        c0010e250_expect32(state->watch[index].address, state->watch[index].value);
    }
    CHECK(c0010e250_memory_matches());
}


typedef struct C0010E250Flags {
    uint32_t old_flags, set, mask, expected;
} C0010E250Flags;

static void test_t1477_appendix_0010e250(void)
{
    static const C0010E250Flags flags[] = {
        {0xFFFFFFFFu, 0x00000000u, 0x80000000u, 0x7FFFFFFFu},
        {0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u},
        {0x12345678u, 0x00000000u, 0x00000000u, 0x12345678u},
        {0x000000FFu, 0x0000000Fu, 0x000000F0u, 0x0000000Fu},
        {0x000000FFu, 0x000000F0u, 0x0000000Fu, 0x000000F0u},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
        {0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u},
        {0xAAAAAAAAu, 0x55555555u, 0x0F0F0F0Fu, 0xF5F5F5F5u},
        {0x7FFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000000u},
        {0x80000000u, 0x7FFFFFFFu, 0x80000000u, 0x7FFFFFFFu},
        {0x0000FFFFu, 0x00010000u, 0x0000FFFFu, 0x00010000u},
        {0x80000000u, 0x80000000u, 0x00000001u, 0x80000000u},
        {0x00000001u, 0x00000002u, 0x00000001u, 0x00000002u},
    };
    const uint32_t obj = 0x20000u;
    unsigned index;

    for (index = 0; index < sizeof flags / sizeof flags[0]; ++index) {
        c0010e250_clear();
        guest_write32(obj, 0x01010101u);
        guest_write32(obj + 4u, 0x02020202u);
        guest_write32(obj + 8u, flags[index].old_flags);
        guest_write32(obj + 0xCu, 0x03030303u);
        guest_write32(obj + 0x10u, 0x04040404u);
        c0010e250_enter(obj, flags[index].set, flags[index].mask);
        invoke(0x0010E250u, 4u);
        CHECK(g_eax == obj);
        CHECK(g_ecx == flags[index].expected);
        CHECK(g_edx == flags[index].old_flags);
        c0010e250_check_kept();
        c0010e250_expect32(obj + 8u, flags[index].expected);
        CHECK(c0010e250_memory_matches());
    }

    /* null object: EAX = 0, ECX/EDX are the entry values, no memory read or written */
    c0010e250_clear();
    guest_write32(8u, 0xDEADBEEFu);
    c0010e250_enter(0u, 0xFFFFFFFFu, 0xFFFFFFFFu);
    invoke(0x0010E250u, 4u);
    CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    c0010e250_check_kept();
    CHECK(guest_read32(8u) == 0xDEADBEEFu);
    CHECK(c0010e250_memory_matches());

    /* unaligned object: the dword at obj + 8 straddles two aligned dwords */
    c0010e250_clear();
    memset(&memory[0x20008u], 0xEE, 12u);
    c0010e250_enter(0x20001u, 0x00000011u, 0x00000FF0u);
    invoke(0x0010E250u, 4u);
    CHECK(g_eax == 0x20001u && g_edx == 0xEEEEEEEEu);
    CHECK(g_ecx == ((0xEEEEEEEEu & ~0x00000FF0u) | 0x00000011u));
    c0010e250_expect32(0x20009u, ((0xEEEEEEEEu & ~0x00000FF0u) | 0x00000011u));
    CHECK(c0010e250_memory_matches());

    /* address wrap: obj + 8 wraps to 0 */
    c0010e250_clear();
    guest_write32(0u, 0x12345678u);
    c0010e250_enter(0xFFFFFFF8u, 0x0000000Fu, 0x00FF0000u);
    invoke(0x0010E250u, 4u);
    CHECK(g_eax == 0xFFFFFFF8u && g_edx == 0x12345678u && g_ecx == 0x1200567Fu);
    c0010e250_expect32(0u, 0x1200567Fu);
    CHECK(c0010e250_memory_matches());

    for (index = 0; index < sizeof c0010e250_alias_table / sizeof c0010e250_alias_table[0]; ++index) {
        c0010e250_run_alias(&c0010e250_alias_table[index]);
    }
}
