/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent control for 0x0010E6E0 (game_record_set_byte_0x12_one), written from the original listing, not from
 * the draft (13 byte leaf):
 *   0010E6E0 mov eax,[esp+4]      ; obj
 *   0010E6E4 test eax,eax
 *   0010E6E6 je skip            ; null: EAX = 0, nothing written
 *   0010E6E8 mov byte ptr [eax+0x12],1
 *   0010E6EC ret                  ; cdecl, plain ret, pops 4 through invoke
 * Exit: EAX = obj (0 for null), ECX/EDX = entry values 0x22222222/0x33333333, EBX/ESI/EDI/EBP/DF
 * unchanged. The store is ONE BYTE at obj + 0x12 (32 bit wrap). No guest PUSH or POP in the
 * original, so there is no saved-register slot. The store address is caller controlled: the
 * alias table sweeps obj = 0xEA..0xF7 so the byte lands in each byte of the return slot
 * (0x100..0x103), the object argument slot (0x104..0x107) and the next slot (0x108..0x109),
 * with obj read before the store (so EAX keeps the entry argument) and only the one byte
 * changed; it also wraps (obj = 0xFFFFFFF0 -> byte 2, obj = 0xFFFFFFEE -> byte 0). */

static uint8_t c0010e6e0_before[sizeof memory];

static void c0010e6e0_clear(void)
{
    memset(memory, 0, 0x1000u);
    memset(&memory[0x1F000u], 0, 0x3000u);
}

static void c0010e6e0_sentinels(void)
{
    g_ebx = 0xB1B1B1B1u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xBBBB0001u;
    g_df = 0;
}

static void c0010e6e0_check_kept(void)
{
    CHECK(g_ebx == 0xB1B1B1B1u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(g_df == 0);
}

/* Expected guest write for the whole-memory diff: patch the pre-call snapshot. */
static void c0010e6e0_expect32(uint32_t address, uint32_t value)
{
    memcpy(&c0010e6e0_before[address], &value, 4u);
}

static int c0010e6e0_memory_matches(void)
{
    return memcmp(c0010e6e0_before, memory, sizeof memory) == 0;
}

/* Prepare the frame (return slot 0x12345678 at ESP = 0x100, arguments at 0x104, 0x108, 0x10C),
 * the sentinels, and snapshot memory. Pre-state the test wrote before this call survives. */
static void c0010e6e0_enter(uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    prepare();
    guest_write32(g_esp + 4u, arg0);
    guest_write32(g_esp + 8u, arg1);
    guest_write32(g_esp + 12u, arg2);
    c0010e6e0_sentinels();
    memcpy(c0010e6e0_before, memory, sizeof memory);
}

typedef struct C0010E6E0Alias {
    uint32_t arg[3];
    unsigned pre_count;
    struct { uint32_t address, value; } pre[4];
    uint32_t eax, ecx, edx;
    unsigned watch_count;
    struct { uint32_t address, value; } watch[10];
} C0010E6E0Alias;

/* Alias states. Expected values were produced by EXECUTING THE ORIGINAL BYTES of 0x0010E6E0 in
 * Unicorn 2.1.4 (tmp/c96/c96_oracle.py, tmp/c96/c96_gen.py: original only, never the draft) on
 * these named entry states. Memory is zero except the listed pre writes (applied after the
 * arguments, so they may overwrite stack slots on purpose), ESP = 0x100, [0x100] = 0x12345678,
 * EAX/ECX/EDX = 0x11111111/0x22222222/0x33333333, EBX/ESI/EDI/EBP sentinels. watch lists every
 * dword whose final value the original produced (stack slots 0x100..0x10C plus the pre
 * addresses and aliased targets), the rest of memory must be unchanged. */
static const C0010E6E0Alias c0010e6e0_alias_table[] = {
    {{0xEAU, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xEAU, 0x22222222U, 0x33333333U, 5, {{0xFCU, 0x1U}, {0x100U, 0xA1B2C3D4U}, {0x104U, 0xEAU}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xEBU, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xEBU, 0x22222222U, 0x33333333U, 5, {{0xFCU, 0x100U}, {0x100U, 0xA1B2C3D4U}, {0x104U, 0xEBU}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xECU, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xECU, 0x22222222U, 0x33333333U, 5, {{0xFCU, 0x10000U}, {0x100U, 0xA1B2C3D4U}, {0x104U, 0xECU}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xEDU, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xEDU, 0x22222222U, 0x33333333U, 5, {{0xFCU, 0x1000000U}, {0x100U, 0xA1B2C3D4U}, {0x104U, 0xEDU}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xEEU, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xEEU, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C301U}, {0x104U, 0xEEU}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xEFU, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xEFU, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B201D4U}, {0x104U, 0xEFU}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF0U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF0U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA101C3D4U}, {0x104U, 0xF0U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF1U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF1U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0x1B2C3D4U}, {0x104U, 0xF1U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF2U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF2U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C3D4U}, {0x104U, 0x1U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF3U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF3U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C3D4U}, {0x104U, 0x1F3U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF4U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF4U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C3D4U}, {0x104U, 0x100F4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF5U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF5U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C3D4U}, {0x104U, 0x10000F5U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF6U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF6U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C3D4U}, {0x104U, 0xF6U}, {0x108U, 0xE5F60701U}, {0x10CU, 0x293A4B5CU}}},
    {{0xF7U, 0x0U, 0x0U}, 3, {{0x100U, 0xA1B2C3D4U}, {0x108U, 0xE5F60718U}, {0x10CU, 0x293A4B5CU}}, 0xF7U, 0x22222222U, 0x33333333U, 4, {{0x100U, 0xA1B2C3D4U}, {0x104U, 0xF7U}, {0x108U, 0xE5F60118U}, {0x10CU, 0x293A4B5CU}}},
    {{0xFFFFFFF0U, 0x0U, 0x0U}, 2, {{0x0U, 0x99887766U}, {0x4U, 0x55443322U}}, 0xFFFFFFF0U, 0x22222222U, 0x33333333U, 6, {{0x0U, 0x99017766U}, {0x4U, 0x55443322U}, {0x100U, 0x12345678U}, {0x104U, 0xFFFFFFF0U}, {0x108U, 0x0U}, {0x10CU, 0x0U}}},
    {{0xFFFFFFEEU, 0x0U, 0x0U}, 2, {{0xFCU, 0x99887766U}, {0x100U, 0x55443322U}}, 0xFFFFFFEEU, 0x22222222U, 0x33333333U, 6, {{0x0U, 0x1U}, {0xFCU, 0x99887766U}, {0x100U, 0x55443322U}, {0x104U, 0xFFFFFFEEU}, {0x108U, 0x0U}, {0x10CU, 0x0U}}},
};

static void c0010e6e0_run_alias(const C0010E6E0Alias *state)
{
    unsigned index;
    c0010e6e0_clear();
    prepare();
    guest_write32(g_esp + 4u, state->arg[0]);
    guest_write32(g_esp + 8u, state->arg[1]);
    guest_write32(g_esp + 12u, state->arg[2]);
    for (index = 0; index < state->pre_count; ++index) {
        guest_write32(state->pre[index].address, state->pre[index].value);
    }
    c0010e6e0_sentinels();
    memcpy(c0010e6e0_before, memory, sizeof memory);
    invoke(0x0010E6E0U, 4u);
    CHECK(g_eax == state->eax);
    CHECK(g_ecx == state->ecx);
    CHECK(g_edx == state->edx);
    c0010e6e0_check_kept();
    for (index = 0; index < state->watch_count; ++index) {
        c0010e6e0_expect32(state->watch[index].address, state->watch[index].value);
    }
    CHECK(c0010e6e0_memory_matches());
}


static void c0010e6e0_expect8(uint32_t address, uint8_t value)
{
    c0010e6e0_before[address] = value;
}

static void test_t1477_appendix_0010e6e0(void)
{
    static const uint32_t objects[] = {0x20000u, 0x20001u, 0x2FFEEu, 0x80FFEDu};
    static const uint8_t previous[] = {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu, 0xEEu};
    unsigned object_index;
    unsigned previous_index;
    unsigned index;

    for (object_index = 0; object_index < sizeof objects / sizeof objects[0]; ++object_index) {
        const uint32_t obj = objects[object_index];
        for (previous_index = 0; previous_index < sizeof previous; ++previous_index) {
            c0010e6e0_clear();
            memset(&memory[obj + 0xEu], 0xEE, 5u);                      /* bytes +0xE..+0x12 */
            if (obj + 0x16u < sizeof memory) {
                memset(&memory[obj + 0x13u], 0xEE, 4u);                 /* bytes +0x13..+0x16 */
            }
            memory[obj + 0x12u] = previous[previous_index];
            c0010e6e0_enter(obj, 0u, 0u);
            invoke(0x0010E6E0U, 4u);
            CHECK(g_eax == obj && g_ecx == 0x22222222u && g_edx == 0x33333333u);
            c0010e6e0_check_kept();
            c0010e6e0_expect8(obj + 0x12u, 1u);
            CHECK(c0010e6e0_memory_matches());
            CHECK(memory[obj + 0x11u] == 0xEEu);
            CHECK(memory[obj + 0x12u] == 1u);
            if (obj + 0x13u < sizeof memory) {
                CHECK(memory[obj + 0x13u] == 0xEEu);
            }
        }
    }

    /* null object: EAX = 0, ECX/EDX are the entry values, byte 0x12 and 0 are untouched */
    c0010e6e0_clear();
    memory[0x12u] = 0xEEu;
    memory[0x13u] = 0xEEu;
    c0010e6e0_enter(0u, 0u, 0u);
    invoke(0x0010E6E0U, 4u);
    CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    c0010e6e0_check_kept();
    CHECK(memory[0x12u] == 0xEEu && memory[0x13u] == 0xEEu);
    CHECK(c0010e6e0_memory_matches());

    for (index = 0; index < sizeof c0010e6e0_alias_table / sizeof c0010e6e0_alias_table[0]; ++index) {
        c0010e6e0_run_alias(&c0010e6e0_alias_table[index]);
    }
}
