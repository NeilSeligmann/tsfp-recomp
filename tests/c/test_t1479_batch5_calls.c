/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 fifth batch caller controls. Callees are native transcriptions that check the exact
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
static struct { uint32_t pc, arg0, arg1; } call_log[64];
static uint32_t player_result = 0x00400000u;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define ROOT_STACK 0x00010000u
#define REGISTER_ECX 0x22334455u
#define REGISTER_EDX 0x33445566u

static void enter(void)
{
    if (calls < 64u) {
        call_log[calls].pc = guest_read32(g_esp);
        call_log[calls].arg0 = guest_read32(g_esp + 4u);
        call_log[calls].arg1 = guest_read32(g_esp + 8u);
    }
    calls++;
}
static void leaf_69640(void)
{
    enter();
    g_eax = player_result;
    g_ecx = 0x7777u;
    g_edx = 0x8888u;
    g_esp += 4u;
}
static void leaf_63ea0(void)
{
    enter();
    g_eax = call_log[calls - 1u].arg0 * 0x68u + 0x006F0B08u;
    g_esp += 4u;
}
static void leaf_15be50(void) { enter(); g_eax = guest_read32(0x007A2BCCu); g_esp += 4u; }
static void leaf_15be60(void) { enter(); g_eax = call_log[calls - 1u].arg0 * 8u + 0x007A2960u; g_esp += 4u; }
static void leaf_69930(void)
{
    enter();
    const uint32_t first = call_log[calls - 1u].arg0, second = call_log[calls - 1u].arg1;
    uint32_t base = 0x006F11C0u;
    if ((guest_read32(0x007DE458u) & 0x800000u) == 0u &&
        guest_read32(first * 0x1F54u + 0x007BEDC0u) != 0u) {
        base = first * 0x1F54u + 0x007C0050u;
    }
    g_eax = base + second * 24u + 0x490u;
    g_ecx = 0x1111u;
    g_esp += 4u;
}
static void leaf_d7550(void)
{
    enter();
    const uint32_t object = call_log[calls - 1u].arg0, value = call_log[calls - 1u].arg1;
    g_eax = value;
    g_ecx = object;
    guest_write32(object + 0x84u, value);
    g_eax = value * 5u;
    g_edx = guest_read32(g_eax * 4u + 0x004D1E28u);
    guest_write32(object + 0xA58u, g_edx);
    g_esp += 4u;
}
static void leaf_31dbc0(void)
{
    enter();
    const uint32_t wanted = call_log[calls - 1u].arg0;
    const uint32_t world = guest_read32(0x007844A8u);
    const uint32_t total = guest_read32(world + 0x81A4u);
    g_ecx = 0u;
    g_eax = 1u;
    for (uint32_t index = 0u; (int32_t)index < (int32_t)total; index++) {
        uint16_t word;
        memcpy(&word, game_host_ptr(world + 0x81B4u + 0xB4u * index), sizeof word);
        if ((uint32_t)(int32_t)(int16_t)word == wanted && guest_read32(world + 0x8250u + 0xB4u * index) != 1u) {
            g_ecx = index;
            g_eax = 0u;
            break;
        }
        g_ecx = index + 1u;
    }
    g_edx = total;
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x00069640u: return leaf_69640;
    case 0x00063EA0u: return leaf_63ea0;
    case 0x0015BE50u: return leaf_15be50;
    case 0x0015BE60u: return leaf_15be60;
    case 0x00069930u: return leaf_69930;
    case 0x000D7550u: return leaf_d7550;
    case 0x0031DBC0u: return leaf_31dbc0;
    default: return NULL;
    }
}
extern void sub_002C3390(void), sub_002E6F80(void), sub_002B2390(void);
extern void sub_002FE0F0(void), sub_0033ACB0(void), sub_00321560(void);

static void prepare(unsigned arguments, const uint32_t *values)
{
    g_esp = ROOT_STACK;
    g_eax = 0x11223344u; g_ecx = REGISTER_ECX; g_edx = REGISTER_EDX;
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

static void test_profile_compare(void)
{
    static const uint32_t offsets[6] = {0x120u, 0x118u, 0x11Cu, 0x12Cu, 0x124u, 0x130u};
    static const uint32_t globals[6] = {0x5236BCu, 0x5236B4u, 0x5236B8u, 0x7613D8u, 0x5236C0u, 0x7613DCu};
    static const uint32_t return_pcs[6] = {0x2C3395u, 0x2C33A8u, 0x2C33BBu, 0x2C33CEu, 0x2C33E1u, 0x2C33F4u};
    for (unsigned mismatch = 0u; mismatch < 7u; mismatch++) {
        for (unsigned variant = 0u; variant < 3u; variant++) {
            prepare(0u, NULL);
            uint32_t values[6];
            for (unsigned step = 0u; step < 6u; step++) {
                values[step] = variant == 0u ? 0x11111111u * (step + 1u) : (variant == 1u ? 0x80000000u >> step : 5u);
                guest_write32(player_result + offsets[step], values[step]);
                guest_write32(globals[step], mismatch == step ? values[step] ^ 1u : values[step]);
            }
            sub_002C3390();
            const unsigned expected_calls = mismatch == 6u ? 6u : mismatch + 1u;
            CHECK(g_eax == (mismatch == 6u ? 0u : 1u) && calls == expected_calls);
            for (unsigned k = 0u; k < expected_calls; k++) CHECK(call_log[k].pc == return_pcs[k]);
            /* Registers as the original leaves them after the last executed step. */
            uint32_t ecx = REGISTER_ECX, edx = REGISTER_EDX;
            for (unsigned step = 0u; step < expected_calls; step++) {
                ecx = 0x7777u; edx = 0x8888u;
                if (step == 1u) ecx = values[1];
                if (step == 2u) edx = values[2];
                if (step == 3u) ecx = mismatch == 3u ? values[3] ^ 1u : values[3];
                if (step == 4u) edx = values[4];
            }
            CHECK(g_ecx == ecx && g_edx == edx);
            if (mismatch == 6u) CHECK(g_eax == 0u);
            check_preserved_state();
        }
    }
}

static void test_roster_weighted_sum(void)
{
    static const uint32_t counts[4][2] = {{0u, 0u}, {1u, 0u}, {2u, 1u}, {1u, 3u}};
    static const uint32_t patterns[5][4] = {{0u, 1u, 2u, 3u}, {0xFFFFFFFFu, 0u, 0u, 1u}, {2u, 2u, 2u, 2u}, {3u, 0xFFFFFFFFu, 1u, 0u}, {1u, 1u, 0xFFFFFFFFu, 0xFFFFFFFFu}};
    static const uint32_t flags[4] = {0u, 0x20u, 0x10u, 0x30u};
    static const uint8_t b0s[3] = {0u, 1u, 0xFFu};
    const uint32_t object = 0x00300000u;
    for (unsigned c = 0u; c < 4u; c++) {
        for (unsigned p = 0u; p < 5u; p++) {
            for (unsigned tag = 0u; tag < 2u; tag++) {
                for (unsigned f = 0u; f < 4u; f++) {
                    for (unsigned b = 0u; b < 3u; b++) {
                        prepare(1u, &object);
                        guest_write32(0x007A2958u, counts[c][0]);
                        guest_write32(0x00790950u, counts[c][1]);
                        guest_write32(0x007DE458u, flags[f]);
                        memory[object] = b0s[b];
                        memory[object + 1u] = tag == 0u ? 5u : 6u;
                        const uint16_t table[3] = {1000u, 0x7FFFu, 0x8000u};
                        memcpy(&memory[object + 2u], table, sizeof table);
                        const uint16_t last = 0xFFFBu;
                        memcpy(&memory[object + 0x4Eu], &last, 2u);
                        for (unsigned i = 0u; i < 4u; i++) {
                            guest_write32(0x007DE4C0u + 4u * i, patterns[p][i]);
                            if ((int32_t)patterns[p][i] >= 0) {
                                const uint32_t record = 0x006F0B08u + patterns[p][i] * 0x68u;
                                memory[record] = (uint8_t[]){0u, 1u, 2u, 0xFFu}[patterns[p][i] % 4u];
                                memory[record + 1u] = patterns[p][i] % 2u == 0u ? 5u : 6u;
                            }
                        }
                        sub_002E6F80();
                        int32_t sum = 0;
                        unsigned expected_calls = 0u;
                        const uint32_t total = counts[c][0] + counts[c][1];
                        for (uint32_t i = 0u; (int32_t)i < (int32_t)total; i++) {
                            if ((int32_t)patterns[p][i] < 0) continue;
                            expected_calls++;
                            const uint32_t record = 0x006F0B08u + patterns[p][i] * 0x68u;
                            if (memory[record + 1u] != memory[object + 1u]) {
                                const int index = (int8_t)memory[record];
                                int16_t word;
                                memcpy(&word, &memory[object + 2u + 2 * index], 2u);
                                sum += word;
                            }
                        }
                        if ((flags[f] & 0x20u) != 0u) {
                            int16_t word;
                            const int index = (int8_t)memory[object];
                            memcpy(&word, &memory[object + 2u + 2 * index], 2u);
                            sum -= word;
                            CHECK(g_ecx == (uint32_t)(int32_t)index && g_edx == (uint32_t)(int32_t)word);
                        } else if ((flags[f] & 0x10u) != 0u) {
                            sum -= (int16_t)0xFFFB;
                        }
                        CHECK(g_eax == (uint32_t)sum && calls == expected_calls);
                        for (unsigned k = 0u; k < expected_calls; k++) CHECK(call_log[k].pc == 0x002E6FB1u);
                        check_preserved_state();
                    }
                }
            }
        }
    }
}

static void test_flag_scan(void)
{
    static const uint32_t counts[5] = {0u, 1u, 2u, 3u, 0xFFFFFFFFu};
    static const uint8_t patterns[5][3] = {{0u, 0u, 0u}, {1u, 0u, 0u}, {0u, 1u, 0u}, {0u, 0u, 1u}, {0xFEu, 2u, 3u}};
    for (unsigned c = 0u; c < 5u; c++) {
        for (unsigned p = 0u; p < 5u; p++) {
            prepare(0u, NULL);
            guest_write32(0x007A2BCCu, counts[c]);
            for (unsigned i = 0u; i < 3u; i++) {
                guest_write32(0x007A2960u + 8u * i + 4u, 0x00300000u + 0x40u * i);
                memory[0x300000u + 0x40u * i + 0x28u] = patterns[p][i];
            }
            sub_002B2390();
            uint32_t expected = 1u, expected_calls = 1u, expected_ecx = REGISTER_ECX;
            for (uint32_t i = 0u; (int32_t)i < (int32_t)counts[c] && i < 3u; i++) {
                expected_calls += 2u;
                expected_ecx = (expected_ecx & 0xFFFFFF00u) | patterns[p][i];
                if ((patterns[p][i] & 1u) != 0u) { expected = 0u; expected_calls -= 1u; break; }
            }
            CHECK(g_eax == expected && calls == expected_calls && g_ecx == expected_ecx);
            CHECK(calls < 1u || call_log[0].pc == 0x002B2398u);
            for (unsigned k = 1u; k < calls; k++) CHECK(call_log[k].pc == (k % 2u == 1u ? 0x002B23A6u : 0x002B23BAu));
            check_preserved_state();
        }
    }
}

static void test_copy_struct(void)
{
    const uint32_t source = 0x00300000u, dest = 0x00300100u, record = 0x006F11C0u + 24u + 0x490u;
    static const uint8_t bytes[16] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 0xFEu, 12u, 0x80u, 0xFFu, 15u, 16u};
    /* Byte path: flag zero. */
    for (unsigned overlap = 0u; overlap < 5u; overlap++) {
        const uint32_t destination = source + (uint32_t[]){0x100u, 4u, 8u, 0xCu, 0xFFFFFFFCu}[overlap];
        const uint32_t arguments[3] = {source, destination, 0u};
        prepare(3u, arguments);
        memcpy(&memory[source], bytes, sizeof bytes);
        static uint8_t shadow[0x200];
        memcpy(shadow, &memory[source - 0x10u], sizeof shadow);
        #define SH32(address) (*(uint32_t *)&shadow[(address) - (source - 0x10u)])
        #define SH8(address) shadow[(address) - (source - 0x10u)]
        uint32_t edx = SH8(source);
        SH32(destination) = edx;
        edx = SH32(source + 4u);
        SH32(destination + 4u) = edx;
        edx = SH32(source + 8u);
        SH32(destination + 8u) = edx;
        edx = SH8(source + 0xCu);
        SH32(destination + 0xCu) = edx;
        const uint32_t eax = SH8(source + 0xDu);
        SH32(destination + 0x10u) = eax;
        sub_002FE0F0();
        CHECK(memcmp(shadow, &memory[source - 0x10u], sizeof shadow) == 0);
        CHECK(g_eax == eax && g_ecx == destination && g_edx == edx && calls == 0u);
        check_preserved_state();
        #undef SH32
        #undef SH8
    }
    /* Record path: flag set, destination separate or overlapping the record. */
    for (unsigned overlap = 0u; overlap < 5u; overlap++) {
        const uint32_t destination = overlap == 0u ? dest : record + (uint32_t[]){0u, 4u, 8u, 0x14u, 0xFFFFFFFCu}[overlap];
        const uint32_t arguments[3] = {source, destination, 0xFFFFFFFFu};
        prepare(3u, arguments);
        guest_write32(0x007DE458u, 0x800000u);
        guest_write32(0x007DE470u, 1u);
        for (uint32_t k = 0u; k < 6u; k++) guest_write32(record + 4u * k, 0xB0001111u * (k + 1u));
        static uint8_t shadow[0x80];
        memcpy(shadow, &memory[record - 0x10u], sizeof shadow);
        #define SHW(address) (*(uint32_t *)&shadow[(address) - (record - 0x10u)])
        const int inside = destination >= record - 0x10u && destination + 0x18u <= record + 0x70u;
        uint32_t edx = 0u;
        if (inside) {
            for (uint32_t k = 0u; k < 5u; k++) {
                edx = SHW(record + 4u * k);
                SHW(destination + 4u * k) = edx;
            }
            SHW(destination + 0x14u) = SHW(record + 0x14u);
        } else {
            edx = 0xB0001111u * 5u;
        }
        sub_002FE0F0();
        if (inside) {
            CHECK(memcmp(shadow, &memory[record - 0x10u], sizeof shadow) == 0);
        } else {
            for (uint32_t k = 0u; k < 5u; k++) CHECK(guest_read32(destination + 4u * k) == 0xB0001111u * (k + 1u));
            CHECK(guest_read32(destination + 0x14u) == 0xB0001111u * 6u);
        }
        CHECK(calls == 1u && call_log[0].pc == 0x002FE105u && call_log[0].arg0 == 0u && call_log[0].arg1 == 1u);
        CHECK(g_ecx == destination && g_edx == edx);
        check_preserved_state();
        #undef SHW
    }
}

static void test_class_sync(void)
{
    static const uint32_t triples[4][3] = {{0u, 1u, 2u}, {1u, 5u, 0xFFFFFFFFu}, {2u, 2u, 2u}, {0xFFFFFFFFu, 0u, 1u}};
    static const uint32_t slots[3][3] = {{0u, 1u, 2u}, {1u, 1u, 3u}, {2u, 0u, 2u}};
    const uint32_t base = 0x00380000u;
    for (unsigned players = 0u; players < 4u; players++) {
        for (unsigned t = 0u; t < 4u; t++) {
            for (unsigned s = 0u; s < 3u; s++) {
                for (unsigned equal = 0u; equal < 2u; equal++) {
                    prepare(0u, NULL);
                    guest_write32(0x00790950u, players);
                    guest_write32(0x007B0C48u, base);
                    for (unsigned player = 0u; player < 3u; player++) {
                        const uint32_t owner = 0x00300000u + 0x400u * player, object = 0x00301000u + 0x400u * player;
                        guest_write32(0x00784034u + 4u * player, triples[t][player]);
                        guest_write32(base + player * 0x1584u + 0x14u, owner);
                        guest_write32(owner + 0x7Cu, object);
                        guest_write32(object + 8u, slots[s][player]);
                        guest_write32(0x004D1E28u + (triples[t][player] + 1u) * 20u, 0x1000u + player);
                        memory[0x7DE424u + slots[s][player]] = equal != 0u ? (uint8_t)(triples[t][player] + 1u) : 0x5Au;
                    }
                    uint8_t model[8];
                    memcpy(model, &memory[0x7DE424u], 8u);
                    uint32_t model_ecx = REGISTER_ECX, model_edx = REGISTER_EDX;
                    for (unsigned player = 0u; player < players; player++) {
                        const uint32_t value = triples[t][player] + 1u, slot = slots[s][player];
                        model_ecx = model[slot];
                        model_edx = value;
                        model[slot] = (uint8_t)value;
                    }
                    sub_0033ACB0();
                    CHECK(calls == players);
                    for (unsigned player = 0u; player < players; player++) {
                        const uint32_t object = 0x00301000u + 0x400u * player;
                        const uint32_t value = triples[t][player] + 1u;
                        CHECK(call_log[player].pc == 0x0033ACDBu && call_log[player].arg0 == object && call_log[player].arg1 == value);
                        CHECK(guest_read32(object + 0x84u) == value);
                        unsigned winner = player;
                        for (unsigned other = 0u; other < 3u; other++) {
                            if (triples[t][other] == triples[t][player]) winner = other;
                        }
                        CHECK(guest_read32(object + 0xA58u) == 0x1000u + winner);
                    }
                    CHECK(memcmp(model, &memory[0x7DE424u], 8u) == 0);
                    CHECK(g_eax == players && g_ecx == model_ecx && g_edx == model_edx);
                    check_preserved_state();
                }
            }
        }
    }
}

static void test_count_records(void)
{
    static const uint32_t pairs[4][2][2] = {{{0u, 1u}, {1u, 0u}}, {{1u, 1u}, {2u, 1u}}, {{5u, 1u}, {0u, 2u}}, {{2u, 0u}, {1u, 1u}}};
    const uint32_t world = 0x00340000u, wanted = 0x77u;
    for (unsigned n1 = 0u; n1 < 4u; n1++) {
        for (unsigned match = 0u; match < 8u; match++) {
            for (unsigned n2 = 0u; n2 < 3u; n2++) {
                for (unsigned p = 0u; p < 4u; p++) {
                    prepare(1u, &wanted);
                    guest_write32(0x007844A8u, world);
                    guest_write32(world + 0xDD10u, n1);
                    guest_write32(world + 0x81A4u, n2);
                    for (unsigned i = 0u; i < 3u; i++) guest_write32(world + 0xDD98u + 0x8Cu * i, ((match >> i) & 1u) != 0u ? wanted : wanted + 1u);
                    for (unsigned j = 0u; j < 2u; j++) {
                        const uint16_t word = (uint16_t)pairs[p][j][0];
                        memcpy(&memory[world + 0x81B4u + 0xB4u * j], &word, 2u);
                        guest_write32(world + 0x8250u + 0xB4u * j, pairs[p][j][1]);
                    }
                    sub_00321560();
                    uint32_t expected = 1u;
                    unsigned expected_calls = 0u;
                    for (uint32_t i = 0u; i < n1; i++) {
                        if (((match >> i) & 1u) == 0u) continue;
                        expected_calls++;
                        int blocked = 0;
                        for (uint32_t j = 0u; j < n2 && !blocked; j++) {
                            blocked = pairs[p][j][0] == i && pairs[p][j][1] != 1u;
                        }
                        if (blocked) expected++;
                    }
                    CHECK(g_eax == expected && calls == expected_calls);
                    for (unsigned k = 0u; k < expected_calls; k++) CHECK(call_log[k].pc == 0x0032158Eu);
                    check_preserved_state();
                }
            }
        }
    }
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    test_profile_compare();
    test_roster_weighted_sum();
    test_flag_scan();
    test_copy_struct();
    test_class_sync();
    test_count_records();
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
