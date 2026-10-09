/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 second batch caller controls. Callees are native transcriptions that check the exact
 * return PC, incoming registers and frame words; the differential proofs use the authentic
 * lifted callees instead.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
static uint32_t expected_pc, player_result, entry_esp = 0x00010000u;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define ROOT_STACK 0x00010000u
#define NETWORK 0x0076B108u
#define MODE 0x0079094Cu
#define STATE 0x0076B140u

static void leaf_enter(void)
{
    CHECK(g_esp == entry_esp - 4u);
    CHECK(guest_read32(g_esp) == expected_pc);
    calls++;
}
static void network_getter(void)
{
    leaf_enter();
    g_eax = guest_read32(NETWORK);
    g_esp += 4u;
}
static void command_leaf(void)
{
    leaf_enter();
    if (guest_read32(MODE) == 0x65u) {
        g_ecx = guest_read32(STATE);
        g_eax = g_ecx == 2u ? 1u : 0u;
    } else {
        g_eax = guest_read32(NETWORK) != 0u && guest_read32(STATE) == 2u ? 1u : 0u;
    }
    g_esp += 4u;
}
static void player_context(void)
{
    leaf_enter();
    CHECK(g_edx == 0x33445566u);
    g_eax = player_result;
    g_ecx = 0x7777u;
    g_edx = 0x8888u;
    g_esp += 4u;
}
static void live_flag(void)
{
    leaf_enter();
    g_ecx = guest_read32(0x00774024u);
    g_eax = g_ecx == 1u ? 1u : 0u;
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x003533F0u: return network_getter;
    case 0x00355F10u: return command_leaf;
    case 0x00069640u: return player_context;
    case 0x0035E960u: return live_flag;
    default: return NULL;
    }
}
extern void sub_003568C0(void), sub_0035E6A0(void), sub_002BFEC0(void);
extern void sub_002EF160(void), sub_0035EAF0(void), sub_00356990(void);

static void prepare(unsigned arguments, const uint32_t *values)
{
    g_esp = ROOT_STACK;
    entry_esp = ROOT_STACK;
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

static void test_slot_connected(void)
{
    static const uint32_t slots[5] = {0u, 1u, 15u, 16u, 0xFFFFFFFFu};
    for (unsigned network = 0u; network < 2u; network++) {
        for (unsigned s = 0u; s < 5u; s++) {
            for (unsigned flag = 0u; flag < 2u; flag++) {
                for (unsigned mode = 0u; mode < 2u; mode++) {
                    for (unsigned record = 0u; record < 3u; record++) {
                        const uint32_t slot = slots[s];
                        prepare(1u, &slot);
                        expected_pc = 0x003568C5u;
                        guest_write32(NETWORK, network);
                        guest_write32(MODE, mode == 0u ? 0x65u : 0x64u);
                        guest_write8(0x0077728Cu + slot * 0x124u, (uint8_t)(0xA4u | flag));
                        guest_write32(0x007356D8u, record == 0u ? 0u : 0x00300000u);
                        guest_write32(0x00300000u + slot * 0xEA0u, record == 2u ? 7u : 0u);
                        guest_write32(0x007DE30Cu, 0x5A5A5A5Au);
                        sub_003568C0();
                        const int small = (int32_t)slot < 16;
                        const uint32_t expected =
                            network == 0u ? 1u
                            : !small ? 0u
                            : flag == 0u ? 0u
                            : mode == 0u ? 1u
                            : record == 2u ? 1u : 0u;
                        CHECK(g_eax == expected && calls == 1u);
                        if (network != 0u && small) {
                            CHECK(g_ecx == (flag == 0u || mode == 0u ? slot * 0x124u
                                            : (record == 0u ? 0u : 0x00300000u)));
                        } else {
                            CHECK(g_ecx == 0x22334455u);
                        }
                        CHECK(g_edx == 0x33445566u);
                        CHECK(guest_read32(ROOT_STACK - 4u) == 0x003568C5u);
                        check_preserved_state();
                    }
                }
            }
        }
    }
    /* Product wraps to zero: no record read, returns 0. */
    const uint32_t slot = 1u;
    prepare(1u, &slot);
    expected_pc = 0x003568C5u;
    guest_write32(NETWORK, 1u);
    guest_write32(MODE, 0x64u);
    guest_write8(0x0077728Cu + 0x124u, 1u);
    guest_write32(0x007356D8u, (uint32_t)(0u - 0xEA0u));
    sub_003568C0();
    CHECK(g_eax == 0u && g_ecx == (uint32_t)(0u - 0xEA0u));
    check_preserved_state();
}

static void test_slot_counts(void)
{
    static const uint32_t thirds[5] = {1u, 0u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u};
    for (unsigned live = 0u; live < 2u; live++) {
        for (unsigned t = 0u; t < 5u; t++) {
            const uint32_t args[3] = {0x11223344u, 0x55667788u, thirds[t]};
            prepare(3u, args);
            expected_pc = 0x0035E6B8u;
            guest_write32(MODE, 0x65u);
            guest_write32(STATE, live != 0u ? 2u : 0u);
            guest_write32(0x0076BBC0u, 0xAAAA5555u);
            sub_0035E6A0();
            CHECK(guest_read32(0x0076BBBCu) == 0x11223344u);
            CHECK(guest_read32(0x0076BBC4u) == 0x55667788u);
            CHECK(guest_read32(0x0076BBC0u) ==
                  (live != 0u && (int32_t)thirds[t] > 0 ? thirds[t] : 0xAAAA5555u));
            CHECK(g_eax == (live != 0u ? thirds[t] : 0u));
            CHECK(g_ecx == (live != 0u ? 2u : 0u) && calls == 1u);
            check_preserved_state();
        }
    }
    /* Third argument slot aliases the second store: it must be read after the stores. */
    const uint32_t args[3] = {0x11223344u, 0x55667788u, 1u};
    prepare(3u, args);
    g_esp = 0x0076BBB8u;
    entry_esp = g_esp;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(0x0076BBBCu, 0x11223344u);
    guest_write32(0x0076BBC0u, 0x55667788u);
    guest_write32(0x0076BBC4u, 1u);
    guest_write32(MODE, 0x65u);
    guest_write32(STATE, 2u);
    expected_pc = 0x0035E6B8u;
    sub_0035E6A0();
    CHECK(g_eax == 0x55667788u && guest_read32(0x0076BBC0u) == 0x55667788u);
}

static void test_context_roots(void)
{
    static const uint32_t fields[4] = {0u, 3u, 10u, 0x80000000u};
    static const uint32_t arguments[5] = {0xFFFFFFFFu, 0u, 6u, 9u, 0x7FFFFFFFu};
    for (unsigned byte = 0u; byte < 2u; byte++) {
        for (unsigned f = 0u; f < 4u; f++) {
            for (unsigned a = 0u; a < 5u; a++) {
                prepare(1u, &arguments[a]);
                expected_pc = 0x002BFECEu;
                player_result = 0x00400000u;
                guest_write8(0x0078ADAEu, byte == 0u ? 0x13u : 0x12u);
                guest_write32(player_result + 0xCu, fields[f]);
                sub_002BFEC0();
                if (byte == 1u) {
                    CHECK(g_eax == 0u && calls == 0u && g_ecx == 0x22334455u);
                } else {
                    const uint32_t limit = 9u - fields[f];
                    CHECK(calls == 1u && g_ecx == limit && g_edx == fields[f]);
                    CHECK(g_eax == ((int32_t)arguments[a] < (int32_t)limit ? 0u : 1u));
                }
                check_preserved_state();
            }
        }
    }
    static const uint32_t table[5] = {0u, 1u, 2u, 3u, 0xFFFFFFFFu};
    for (unsigned t = 0u; t < 5u; t++) {
        for (unsigned flag = 0u; flag < 2u; flag++) {
            prepare(0u, NULL);
            expected_pc = 0x002EF165u;
            player_result = 0x00400000u;
            guest_write8(0x007DE455u, 2u);
            guest_write8(0x007DE456u, 3u);
            guest_write32(0x004D1F84u + 96u, table[t]);
            guest_write32(player_result + 40u + 0x24u, flag != 0u ? 0x80000000u : 0u);
            sub_002EF160();
            CHECK(calls == 1u && g_ecx == player_result + 40u + 0x24u && g_edx == 96u);
            CHECK(g_eax == (t == 1u || (t == 2u && flag != 0u) ? 1u : 0u));
            check_preserved_state();
        }
    }
}

static void test_session_counter(void)
{
    static const uint8_t values[6] = {0u, 1u, 0x7Fu, 0xFDu, 0xFEu, 0xFFu};
    for (unsigned gate = 0u; gate < 3u; gate++) {
        for (unsigned done = 0u; done < 2u; done++) {
            for (unsigned v = 0u; v < 6u; v++) {
                prepare(0u, NULL);
                expected_pc = 0x0035EAF5u;
                guest_write32(0x00774024u, gate == 0u ? 1u : (gate == 1u ? 0u : 2u));
                guest_write32(0x0076BD6Cu, done != 0u ? 0x100u : 0u);
                guest_write8(0x0076BD68u, values[v]);
                sub_0035EAF0();
                check_preserved_state();
                if (gate != 0u) {
                    CHECK(g_eax == 0u && guest_read8(0x0076BD68u) == values[v]);
                } else if (done != 0u) {
                    CHECK(g_eax == 0x100u && guest_read8(0x0076BD68u) == values[v]);
                } else {
                    CHECK(g_eax == values[v] / 254u && g_ecx == 0xFEu);
                    CHECK(g_edx == values[v] % 254u + 1u);
                    CHECK(guest_read8(0x0076BD68u) == values[v] % 254u + 1u);
                    CHECK(guest_read32(0x0076BD6Cu) == 1u);
                }
            }
        }
    }
}

static void test_role_state(void)
{
    static const uint32_t marks[3] = {0xAu, 0u, 0xBu};
    for (unsigned mode = 0u; mode < 2u; mode++) {
        for (unsigned state = 0u; state < 2u; state++) {
            for (unsigned network = 0u; network < 2u; network++) {
                for (unsigned m = 0u; m < 3u; m++) {
                    prepare(0u, NULL);
                    expected_pc = 0x003569BFu;
                    guest_write32(MODE, mode == 0u ? 0x65u : 0x64u);
                    guest_write32(STATE, state == 0u ? 2u : 3u);
                    guest_write32(NETWORK, network);
                    guest_write32(0x0076B1A4u, marks[m]);
                    sub_00356990();
                    const int live = mode == 0u || network != 0u;
                    CHECK(g_eax == (live && state == 0u && marks[m] == 0xAu ? 1u : 0u));
                    CHECK(calls == (mode == 0u ? 0u : 1u));
                    CHECK(g_ecx == (mode == 0u ? (state == 0u ? 2u : 3u) : 0x22334455u));
                    check_preserved_state();
                }
            }
        }
    }
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    test_slot_connected();
    test_slot_counts();
    test_context_roots();
    test_session_counter();
    test_role_state();
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
