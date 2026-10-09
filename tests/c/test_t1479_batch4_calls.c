/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 fourth batch caller controls. Callees are native transcriptions that check the exact
 * return PC, log every call with its stack arguments and keep the original register effects;
 * the differential proofs use the authentic lifted callees instead. Expected values are
 * computed from the original disassembly, not from the adapters.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
static struct { uint32_t pc, arg0, arg1, arg2; } call_log[64];
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define ROOT_STACK 0x00010000u

static void enter(void)
{
    if (calls < 64u) {
        call_log[calls].pc = guest_read32(g_esp);
        call_log[calls].arg0 = guest_read32(g_esp + 4u);
        call_log[calls].arg1 = guest_read32(g_esp + 8u);
        call_log[calls].arg2 = guest_read32(g_esp + 12u);
    }
    calls++;
}
static void leaf_35e960(void)
{
    enter();
    g_ecx = guest_read32(0x00774024u);
    g_eax = g_ecx == 1u ? 1u : 0u;
    g_esp += 4u;
}
static void leaf_42620(void)
{
    enter();
    const uint32_t state = guest_read32(0x006B98A4u);
    g_eax = state == 5u || state == 2u ? 0u : 1u;
    g_esp += 4u;
}
static void leaf_cb0f0(void) { enter(); g_eax = guest_read32(0x007330D8u); g_esp += 4u; }
static void leaf_69930(void)
{
    enter();
    const uint32_t first = call_log[calls - 1u].arg0, second = call_log[calls - 1u].arg1;
    uint32_t base = 0x006F11C0u;
    if ((guest_read32(0x007DE458u) & 0x800000u) == 0u && guest_read32(first * 0x1F54u + 0x007BEDC0u) != 0u) {
        base = first * 0x1F54u + 0x007C0050u;
    }
    g_eax = base + second * 24u + 0x490u;
    g_ecx = 0x1111u;
    g_esp += 4u;
}
static void leaf_42710(void)
{
    enter();
    g_eax = guest_read32(call_log[calls - 1u].arg0 * 0x3Cu + 0x006B93D4u);
    g_esp += 4u;
}
static void leaf_42c30(void)
{
    enter();
    g_eax = guest_read32(0x006B9888u) != 0u ? 0u : guest_read32(call_log[calls - 1u].arg0 * 0x3Cu + 0x006B93E0u);
    g_esp += 4u;
}
static void leaf_3c8960(void)
{
    enter();
    const uint32_t dest = call_log[calls - 1u].arg0, source = call_log[calls - 1u].arg1;
    uint32_t count = call_log[calls - 1u].arg2;
    uint32_t index = 0u;
    int terminated = 0;
    for (; index < count; index++) {
        const uint8_t value = terminated ? 0u : guest_read8(source + index);
        guest_write8(dest + index, value);
        if (value == 0u) terminated = 1;
    }
    g_eax = dest;
    g_esp += 4u;
}
static void leaf_80f20(void)
{
    enter();
    g_eax = guest_read32(call_log[calls - 1u].arg0 * 24u + 0x004D2D64u);
    g_esp += 4u;
}
static void leaf_2be590(void)
{
    enter();
    const uint32_t key = call_log[calls - 1u].arg0, tag = call_log[calls - 1u].arg1;
    g_ecx = 0x2222u;
    g_eax = 0u;
    for (uint32_t row = 0u; row < 147u; row++) {
        const uint32_t address = 0x00521A18u + 0x30u * row;
        if (guest_read32(address + 4u) == key &&
            ((int32_t)tag < 0 || (uint32_t)(int32_t)(int8_t)guest_read8(address + 1u) == tag)) {
            g_eax = address;
            break;
        }
    }
    g_esp += 4u;
}
static void leaf_59db0(void)
{
    enter();
    const uint32_t object = call_log[calls - 1u].arg0, tag = call_log[calls - 1u].arg1;
    g_eax = guest_read32(object + 4u);
    g_ecx = guest_read32(g_eax + 0xD4u);
    g_eax = g_ecx == tag ? guest_read32(g_eax + 0x124u) : 0u;
    g_esp += 4u;
}
static void leaf_42870(void) { enter(); guest_write32(0x00600000u, call_log[calls - 1u].arg0); g_eax = 0x4287u; g_ecx = 0x4288u; g_esp += 4u; }
static void leaf_55d80(void) { enter(); guest_write32(0x007E71C8u, call_log[calls - 1u].arg0); g_eax = 0x55D8u; g_esp += 4u; }
static void leaf_83970(void)
{
    enter();
    g_eax = guest_read32(call_log[calls - 1u].arg0 * 4u + 0x00703460u);
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x0035E960u: return leaf_35e960;
    case 0x00042620u: return leaf_42620;
    case 0x000CB0F0u: return leaf_cb0f0;
    case 0x00069930u: return leaf_69930;
    case 0x00042710u: return leaf_42710;
    case 0x00042C30u: return leaf_42c30;
    case 0x003C8960u: return leaf_3c8960;
    case 0x00080F20u: return leaf_80f20;
    case 0x002BE590u: return leaf_2be590;
    case 0x00059DB0u: return leaf_59db0;
    case 0x00042870u: return leaf_42870;
    case 0x00055D80u: return leaf_55d80;
    case 0x00083970u: return leaf_83970;
    default: return NULL;
    }
}
extern void sub_002E4C10(void), sub_002E4470(void), sub_002CB1E0(void), sub_002D74B0(void);
extern void sub_00361950(void), sub_0036EBE0(void), sub_002BE5E0(void), sub_002875F0(void);
extern void sub_002E4CB0(void), sub_003381D0(void);

static void prepare(unsigned arguments, const uint32_t *values)
{
    g_esp = ROOT_STACK;
    g_eax = 0x11223344u; g_ecx = 0x22334455u; g_edx = 0x33445566u;
    g_ebx = 0x44556677u; g_ebp = 0x55667788u;
    g_esi = 0x66778899u; g_edi = 0x778899AAu;
    guest_write32(ROOT_STACK, 0x12345678u);
    for (unsigned i = 0u; i < arguments; i++) guest_write32(ROOT_STACK + 4u + 4u * i, values[i]);
    calls = 0u;
}
static void check_preserved_state(void)
{
    CHECK(g_esp == ROOT_STACK + 4u && guest_read32(ROOT_STACK) == 0x12345678u);
    CHECK(g_ebx == 0x44556677u && g_ebp == 0x55667788u);
    CHECK(g_esi == 0x66778899u && g_edi == 0x778899AAu);
}

static void test_slot_flag_gates(void)
{
    static const uint32_t indices[8] = {0u, 3u, 0xFFFFFFFFu, 4u, 0x80000000u, 1u, 2u, 0x7FFFFFFFu};
    static const uint32_t flags[4] = {0u, 1u, 0x200u, 0x201u};
    static const uint32_t g458[4] = {0u, 0x20000000u, 0x40u, 0x20000040u};
    for (unsigned i = 0u; i < 8u; i++) {
        for (unsigned f = 0u; f < 4u; f++) {
            for (unsigned g = 0u; g < 4u; g++) {
                for (unsigned mode = 0u; mode < 2u; mode++) {
                    for (unsigned live = 0u; live < 2u; live++) {
                        for (unsigned s = 0u; s < 3u; s++) {
                            prepare(1u, &indices[i]);
                            for (unsigned slot = 0u; slot < 4u; slot++) {
                                guest_write32(0x00761C48u + 4u * slot, 0x00300000u + 0x40u * slot);
                                guest_write32(0x00300000u + 0x40u * slot, 0x55u);
                            }
                            const int in_range = (int32_t)indices[i] >= 0 && (int32_t)indices[i] < 4;
                            if (in_range) guest_write32(0x00300000u + 0x40u * indices[i], flags[f]);
                            guest_write32(0x0079094Cu, mode == 0u ? 0x64u : 0x65u);
                            guest_write32(0x007DE458u, g458[g]);
                            guest_write32(0x00774024u, live == 0u ? 1u : 0u);
                            guest_write32(0x006B98A4u, s == 0u ? 5u : (s == 1u ? 2u : 3u));
                            sub_002E4C10();
                            uint32_t expected = 0u, expected_calls = 0u;
                            if (in_range && (flags[f] & 0x200u) == 0u) {
                                if (mode == 1u || (g458[g] & 0x20000000u) != 0u) {
                                    expected = 1u;
                                } else {
                                    expected_calls = 1u;
                                    uint32_t result = ~flags[f] & 1u;
                                    if (live == 0u && (g458[g] & 0x40u) != 0u) {
                                        expected_calls = 2u;
                                        if (s != 2u) result = 0u;
                                    }
                                    expected = result;
                                }
                            }
                            CHECK(g_eax == expected && calls == expected_calls);
                            CHECK(calls < 1u || call_log[0].pc == 0x002E4C46u);
                            CHECK(calls < 2u || call_log[1].pc == 0x002E4C58u);
                            check_preserved_state();
                        }
                    }
                }
            }
        }
    }
}

static void test_mode_a_pair(void)
{
    static const uint32_t limits[5] = {2u, 1u, 0u, 0xFFFFFFFFu, 5u};
    static const uint32_t probes[3] = {0u, 1u, 0xFFFFFFFFu};
    static const uint32_t g458[4] = {0u, 0x200u, 0x100u, 0xFFFFFDFFu};
    for (unsigned b = 0u; b < 3u; b++) {
        for (unsigned l = 0u; l < 5u; l++) {
            for (unsigned p = 0u; p < 3u; p++) {
                for (unsigned g = 0u; g < 4u; g++) {
                    prepare(0u, NULL);
                    guest_write8(0x007DE455u, b == 0u ? 0xAu : (b == 1u ? 9u : 0u));
                    guest_write32(0x00790950u, limits[l]);
                    guest_write32(0x007330D8u, probes[p]);
                    guest_write32(0x007DE458u, g458[g]);
                    sub_002E4470();
                    const int active = b == 0u && (int32_t)limits[l] > 1;
                    const uint32_t expected = !active ? 0u : (probes[p] != 0u || (g458[g] & 0x200u) != 0u ? 1u : 0u);
                    CHECK(g_eax == expected && calls == (active ? 1u : 0u));
                    CHECK(g_ecx == 0x22334455u && g_edx == 0x33445566u);
                    check_preserved_state();
                }
            }
        }
    }
}

static void test_selected_index(void)
{
    static const uint32_t indices[5] = {0u, 1u, 2u, 5u, 0xFFFFFFFFu};
    static const uint32_t limits[4] = {0xFFFFFFFBu, 7u, 0x80000000u, 0u};
    static const uint32_t fields[4] = {0xFFFFFFFBu, 7u, 8u, 0x7FFFFFFFu};
    for (unsigned i = 0u; i < 5u; i++) {
        for (unsigned bit = 0u; bit < 2u; bit++) {
            for (unsigned player = 0u; player < 2u; player++) {
                for (unsigned l = 0u; l < 4u; l++) {
                    for (unsigned f = 0u; f < 4u; f++) {
                        prepare(0u, NULL);
                        guest_write32(0x0078AD40u, indices[i]);
                        guest_write32(0x007DE458u, bit != 0u ? 0x800000u : 0u);
                        guest_write32(0x007BEDC0u, player);
                        guest_write32(0x00761260u, limits[l]);
                        const uint32_t base = (bit != 0u || player == 0u) ? 0x006F11C0u : 0x007C0050u;
                        const uint32_t field = base + indices[i] * 24u + 0x490u + 0xCu;
                        guest_write32(field, fields[f]);
                        sub_002CB1E0();
                        const uint32_t expected = (int32_t)fields[f] > (int32_t)limits[l] ? 1u : 0u;
                        CHECK(g_eax == expected && g_ecx == expected && g_edx == fields[f]);
                        CHECK(calls == 1u && call_log[0].pc == 0x002CB1EDu);
                        CHECK(call_log[0].arg0 == 0u && call_log[0].arg1 == indices[i]);
                        check_preserved_state();
                    }
                }
            }
        }
    }
}

static void test_pad_masks(void)
{
    static const uint32_t masks[8] = {0u, 1u, 2u, 4u, 8u, 0x10u, 0xFFFFFFFFu, 3u};
    static const uint32_t patterns[5] = {0u, 1u, 6u, 15u, 8u};
    for (unsigned c = 0u; c < 5u; c++) {
        for (unsigned g = 0u; g < 2u; g++) {
            for (unsigned b = 0u; b < 3u; b++) {
                for (unsigned m = 0u; m < 8u; m++) {
                    static const uint32_t sets[3][4] = {{0u, 0u, 0u, 0u}, {1u, 2u, 4u, 8u}, {0x10u, 0x20u, 0xFFFF0000u, 0x80000000u}};
                    prepare(1u, &masks[m]);
                    guest_write32(0x006B9888u, g);
                    for (unsigned pad = 0u; pad < 4u; pad++) {
                        guest_write32(0x006B93D4u + pad * 0x3Cu, ((patterns[c] >> pad) & 1u) != 0u ? 1u : 0u);
                        guest_write32(0x006B93E0u + pad * 0x3Cu, sets[b][pad]);
                    }
                    sub_002D74B0();
                    unsigned expected_calls = 0u;
                    uint32_t expected = 0u, expected_pc[8];
                    for (unsigned pad = 0u; pad < 4u; pad++) {
                        expected_pc[expected_calls++] = 0x002D74BEu;
                        if (((patterns[c] >> pad) & 1u) == 0u) continue;
                        expected_pc[expected_calls++] = 0x002D74CBu;
                        if ((masks[m] & (g != 0u ? 0u : sets[b][pad])) != 0u) { expected = 1u; break; }
                    }
                    CHECK(g_eax == expected && calls == expected_calls);
                    for (unsigned k = 0u; k < expected_calls && k < calls; k++) CHECK(call_log[k].pc == expected_pc[k]);
                    check_preserved_state();
                }
            }
        }
    }
}

static void test_store_target_name(void)
{
    static const uint32_t lengths[6] = {0u, 1u, 7u, 0x27u, 0x28u, 0x30u};
    for (unsigned l = 0u; l < 6u; l++) {
        const uint32_t arguments[3] = {0x00300000u, 0x00301000u, 0xFFFFFFFFu};
        prepare(3u, arguments);
        memset(&memory[0x773F00u], 0xEE, 0x2Cu);
        for (uint32_t i = 0u; i < lengths[l]; i++) memory[0x300000u + i] = (uint8_t)(0x41u + i % 26u);
        memory[0x300000u + lengths[l]] = 0u;
        for (uint32_t i = 0u; i < 6u; i++) guest_write32(0x00301000u + 4u * i, 0x1000u * (i + 1u));
        sub_00361950();
        CHECK(calls == 1u && call_log[0].pc == 0x00361961u);
        CHECK(call_log[0].arg0 == 0x773F00u && call_log[0].arg1 == 0x300000u && call_log[0].arg2 == 0x28u);
        for (uint32_t i = 0u; i < 0x28u; i++) {
            const uint8_t expected = i < lengths[l] ? (uint8_t)(0x41u + i % 26u) : 0u;
            CHECK(memory[0x773F00u + i] == expected);
        }
        CHECK(memory[0x773F28u] == 0xFFu && guest_read32(0x773F28u) == 0xFFFFFFFFu);
        for (uint32_t i = 0u; i < 6u; i++) CHECK(guest_read32(0x773EE8u + 4u * i) == 0x1000u * (i + 1u));
        CHECK(g_eax == 0x6000u && g_ecx == 0xFFFFFFFFu && g_edx == 0x5000u);
        check_preserved_state();
    }
    /* Struct inside the copy destination: read after the copy (zeros). */
    const uint32_t arguments[3] = {0x00300000u, 0x00773F10u, 7u};
    prepare(3u, arguments);
    memset(&memory[0x773F00u], 0xEE, 0x2Cu);
    memcpy(&memory[0x300000u], "ABC", 4);
    sub_00361950();
    CHECK(guest_read32(0x773EE8u) == 0u && guest_read32(0x773EFCu) == 0u && guest_read32(0x773F28u) == 7u);
}

static void test_icon_widget_init(void)
{
    static const uint32_t deltas[8] = {0x100u, 0u, 4u, 8u, 0xCu, 0xFFFFFFFCu, 0xFFFFFFF8u, 0x10u};
    for (unsigned d = 0u; d < 8u; d++) {
        for (unsigned index = 0u; index < 3u; index++) {
            const uint32_t source = 0x00300100u, dest = source + deltas[d];
            const uint32_t arguments[5] = {dest, source, 0x11223344u, 0xFFFFFFFFu, index * 4u + 1u};
            prepare(5u, arguments);
            for (uint32_t i = 0u; i < 0x40u; i += 4u) guest_write32(0x00300000u + i, 0xA0A00000u + i);
            for (uint32_t i = 0u; i < 20u; i++) guest_write32(0x004D2D64u + 24u * i, 0x1000u + i);
            for (uint32_t i = 0u; i < 4u; i++) guest_write32(source + 4u * i, 0xC0DE0001u * (i + 1u));
            /* Reference model of the interleaved reads and writes on a shadow of the arena. */
            static uint8_t shadow[0x200];
            memcpy(shadow, &memory[0x00300000u], sizeof shadow);
            #define SH(address) (*(uint32_t *)&shadow[(address) - 0x00300000u])
            SH(dest) = SH(source);
            SH(dest + 4u) = SH(source + 4u);
            SH(dest + 8u) = SH(source + 8u);
            const uint32_t fourth = SH(source + 0xCu);
            SH(dest + 0xCu) = fourth;
            SH(dest + 0x24u) = 0x1000u + index * 4u + 1u;
            const uint32_t w0 = SH(dest), w1 = SH(dest + 4u), w2 = SH(dest + 8u);
            SH(dest + 0x10u) = w0;
            const uint32_t w3 = SH(dest + 0xCu);
            SH(dest + 0x14u) = w1;
            SH(dest + 0x18u) = w2;
            SH(dest + 0x1Cu) = w3;
            SH(dest + 0x20u) = 0x11223344u;
            SH(dest + 0x28u) = 0xFFFFFFFFu;
            SH(dest + 0x2Cu) = 1u;
            sub_0036EBE0();
            CHECK(calls == 1u && call_log[0].pc == 0x0036EC09u && call_log[0].arg0 == index * 4u + 1u);
            CHECK(guest_read32(dest + 0x24u) == 0x1000u + index * 4u + 1u);
            CHECK(guest_read32(dest + 0x20u) == 0x11223344u && guest_read32(dest + 0x28u) == 0xFFFFFFFFu);
            CHECK(guest_read32(dest + 0x2Cu) == 1u);
            CHECK(guest_read32(dest + 0x10u) == guest_read32(dest) && guest_read32(dest + 0x14u) == guest_read32(dest + 4u));
            CHECK(guest_read32(dest + 0x18u) == guest_read32(dest + 8u) && guest_read32(dest + 0x1Cu) == guest_read32(dest + 0xCu));
            if (d == 0u) {
                for (uint32_t i = 0u; i < 4u; i++) CHECK(guest_read32(dest + 4u * i) == 0xC0DE0001u * (i + 1u));
            }
            CHECK(memcmp(shadow, &memory[0x00300000u], sizeof shadow) == 0);
            #undef SH
            check_preserved_state();
        }
    }
    /* Overlap order: dest == src + 4 reads src[1] after dest[0] is written (src[0]). */
    const uint32_t arguments[5] = {0x00300104u, 0x00300100u, 0u, 0u, 0u};
    prepare(5u, arguments);
    for (uint32_t i = 0u; i < 4u; i++) guest_write32(0x00300100u + 4u * i, 0xC0DE0001u * (i + 1u));
    for (uint32_t i = 0u; i < 20u; i++) guest_write32(0x004D2D64u + 24u * i, 0x1000u + i);
    sub_0036EBE0();
    /* Each store lands on the next source word before it is read: every word becomes src[0]. */
    for (uint32_t i = 0u; i < 4u; i++) CHECK(guest_read32(0x00300104u + 4u * i) == 0xC0DE0001u);
    CHECK(guest_read32(0x00300100u) == 0xC0DE0001u);
}

static void test_key_pair_lookup(void)
{
    static const uint32_t keys[4] = {0xAB000000u, 0xAB000007u, 0xAB000092u, 0x7FFF1234u};
    static const uint32_t tags[6] = {0xFFFFFFFFu, 0x80000000u, 3u, 100u, 2u, 1u};
    for (uint32_t row = 0u; row < 147u; row++) {
        guest_write32(0x00521A18u + 0x30u * row + 4u, 0x11000000u + row);
        memory[0x521A18u + 0x30u * row + 1u] = (uint8_t)(row % 5u);
    }
    guest_write32(0x00521A18u + 4u, 0xAB000000u);
    memory[0x521A18u + 1u] = 3u;
    guest_write32(0x00521A18u + 0x30u * 7u + 4u, 0xAB000007u);
    memory[0x521A18u + 0x30u * 7u + 1u] = 1u;
    guest_write32(0x00521A18u + 0x30u * 8u + 4u, 0xAB000007u);
    memory[0x521A18u + 0x30u * 8u + 1u] = 2u;
    guest_write32(0x00521A18u + 0x30u * 146u + 4u, 0xAB000092u);
    memory[0x521A18u + 0x30u * 146u + 1u] = 0xFEu;
    for (unsigned k = 0u; k < 4u; k++) {
        for (unsigned t = 0u; t < 6u; t++) {
            const uint32_t arguments[2] = {keys[k], tags[t]};
            prepare(2u, arguments);
            sub_002BE5E0();
            uint32_t expected = 0u;
            for (uint32_t row = 0u; row < 147u && expected == 0u; row++) {
                const uint32_t address = 0x00521A18u + 0x30u * row;
                if (guest_read32(address + 4u) == keys[k] &&
                    ((int32_t)tags[t] < 0 || (uint32_t)(int32_t)(int8_t)memory[address + 1u] == tags[t])) {
                    expected = address + 0x14u;
                }
            }
            CHECK(g_eax == expected && calls == 1u && call_log[0].pc == 0x002BE5EFu);
            CHECK(call_log[0].arg0 == keys[k] && call_log[0].arg1 == tags[t]);
            CHECK(g_ecx == 0x2222u);
            check_preserved_state();
        }
    }
}

static void test_mode_object_lookup(void)
{
    static const uint32_t modes[8] = {1u, 0x11u, 0u, 2u, 0x10u, 0x12u, 0xFFFFFFFFu, 0x7FFFFFFFu};
    static const uint32_t tags[3] = {0x19Cu, 0x23Bu, 0x100u};
    static const uint32_t payloads[3] = {0x11111111u, 0u, 0xFFFFFFFFu};
    const uint32_t object = 0x00300000u, inner = 0x00300100u;
    for (unsigned m = 0u; m < 8u; m++) {
        for (unsigned t = 0u; t < 3u; t++) {
            for (unsigned p = 0u; p < 3u; p++) {
                prepare(1u, &object);
                guest_write32(0x0079094Cu, modes[m]);
                guest_write32(object + 4u, inner);
                guest_write32(inner + 0xD4u, tags[t]);
                guest_write32(inner + 0x124u, payloads[p]);
                sub_002875F0();
                const int hit = (modes[m] == 1u && tags[t] == 0x19Cu) || (modes[m] == 0x11u && tags[t] == 0x23Bu);
                const unsigned expected_calls = modes[m] == 1u || modes[m] == 0x11u ? 1u : 0u;
                CHECK(g_eax == (hit ? payloads[p] : 0u) && calls == expected_calls);
                if (calls != 0u) {
                    CHECK(call_log[0].pc == (modes[m] == 1u ? 0x00287622u : 0x0028760Fu));
                    CHECK(call_log[0].arg0 == object && call_log[0].arg1 == (modes[m] == 1u ? 0x19Cu : 0x23Bu));
                    CHECK(g_ecx == tags[t]);
                }
                check_preserved_state();
            }
        }
    }
}

static void test_current_player(void)
{
    static const uint32_t values[3] = {0u, 0x100u, 0xFFFFFFE0u};
    for (unsigned x = 0u; x < 4u; x++) {
        for (unsigned slot = 0u; slot < 4u; slot++) {
            for (unsigned flag = 0u; flag < 2u; flag++) {
                for (unsigned v = 0u; v < 3u; v++) {
                    prepare(0u, NULL);
                    guest_write32(0x007B0C7Cu, 0x00300000u);
                    guest_write32(0x00300000u, x);
                    guest_write32(0x00300004u, slot);
                    for (unsigned entry = 0u; entry < 4u; entry++) {
                        guest_write32(0x00761C48u + 4u * entry, 0x00301000u + 0x40u * entry);
                        guest_write32(0x00301000u + 0x40u * entry, entry == slot ? flag : 0x54u);
                    }
                    guest_write32(0x007B0CC0u, values[v]);
                    guest_write32(0x00600000u, 0xDEADu);
                    sub_002E4CB0();
                    CHECK(guest_read32(0x007BA9E0u) == slot);
                    CHECK(calls == (flag != 0u ? 2u : 1u));
                    if (flag != 0u) {
                        CHECK(call_log[0].pc == 0x002E4CD2u && call_log[0].arg0 == x);
                        CHECK(guest_read32(0x00600000u) == x);
                    } else {
                        CHECK(guest_read32(0x00600000u) == 0xDEADu);
                    }
                    CHECK(call_log[calls - 1u].pc == 0x002E4CE7u && call_log[calls - 1u].arg0 == values[v] + 0x224u);
                    CHECK(guest_read32(0x007E71C8u) == values[v] + 0x224u);
                    CHECK(g_ecx == values[v] + 0x224u && g_edx == values[v] + 0x224u && g_eax == 0x55D8u);
                    check_preserved_state();
                }
            }
        }
    }
}

static void test_effect_flag(void)
{
    static const uint32_t counts[8] = {1u, 2u, 3u, 4u, 5u, 0u, 0x7FFFFFFFu, 0x80000000u};
    static const uint32_t table[6] = {0x02000000u, 0u, 0xFDFFFFFFu, 0xFFFFFFFFu, 0x01000000u, 0x03000000u};
    const uint32_t object = 0x00300000u;
    for (unsigned c = 0u; c < 8u; c++) {
        for (unsigned t = 0u; t < 6u; t++) {
            prepare(1u, &object);
            guest_write32(object + 0xF54u, counts[c]);
            guest_write32(0x00703460u + (counts[c] - 1u) * 4u, table[t]);
            sub_003381D0();
            const uint32_t expected = ((table[t] >> 25) & 1u) == 0u ? 1u : 0u;
            CHECK(g_eax == expected && g_ecx == expected && g_edx == 0x33445566u);
            CHECK(calls == 1u && call_log[0].pc == 0x003381E1u && call_log[0].arg0 == counts[c] - 1u);
            check_preserved_state();
        }
    }
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    test_slot_flag_gates();
    test_mode_a_pair();
    test_selected_index();
    test_pad_masks();
    test_store_target_name();
    test_icon_widget_init();
    test_key_pair_lookup();
    test_mode_object_lookup();
    test_current_player();
    test_effect_flag();
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
