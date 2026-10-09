/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 third batch caller controls. Callees are native transcriptions that check the exact
 * return PC and frame and log every call; the differential proofs use the authentic lifted
 * callees instead. Expected values are computed from the original disassembly, not from the
 * adapters.
 */
#include "game_replace.h"
#include <stdio.h>

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
__thread uint32_t g_ebx, g_ebp, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x00800000];
static unsigned checks, failures, calls;
static uint32_t call_log[64], entry_esp = 0x00010000u, seen_esi, seen_arg, player_result;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)
#define ROOT_STACK 0x00010000u
#define NETWORK 0x0076B108u
#define MODE 0x0079094Cu
#define STATE 0x0076B140u
#define LIMIT 0x00790950u
#define SESSION 0x00774028u
#define RECORDS 0x007356D8u
#define REGISTER_ECX 0x22334455u
#define REGISTER_EDX 0x33445566u

static void enter(unsigned argument_words)
{
    CHECK(g_esp < entry_esp);
    if (calls < 64u) call_log[calls] = guest_read32(g_esp);
    calls++;
    seen_arg = argument_words != 0u ? guest_read32(g_esp + 4u) : 0u;
}
static uint32_t ref_355ec0(void)
{
    if (guest_read32(MODE) == 0x65u) return guest_read32(STATE) == 1u;
    return guest_read32(NETWORK) == 0u ? 1u : guest_read32(STATE) == 1u;
}
static void leaf_3533f0(void) { enter(0u); g_eax = guest_read32(NETWORK); g_esp += 4u; }
static void leaf_35e970(void) { enter(0u); g_eax = guest_read32(0x0076BBC8u); g_esp += 4u; }
static void leaf_355ec0(void)
{
    enter(0u);
    if (guest_read32(MODE) == 0x65u) {
        g_ecx = guest_read32(STATE);
    } else if (guest_read32(NETWORK) != 0u) {
        g_ecx = guest_read32(STATE);
    }
    g_eax = ref_355ec0();
    g_esp += 4u;
}
static void leaf_69640(void)
{
    enter(0u);
    g_eax = player_result;
    g_ecx = 0x7777u;
    g_edx = 0x8888u;
    g_esp += 4u;
}
static void leaf_35af40(void)
{
    enter(0u);
    seen_esi = g_esi;
    g_eax = g_esi != 0u ? (g_esi ^ 0x5Au) : 0x77u;
    g_ecx = 0xA1A1u;
    g_edx = 0xB2B2u;
    g_esp += 4u;
}
static void leaf_389b0(void)
{
    enter(1u);
    CHECK(seen_arg == 0xFFFFFFFFu);
    g_esp += 4u;
}
static uint32_t connected_mask;
static void leaf_3568c0(void)
{
    enter(1u);
    g_eax = (connected_mask >> (seen_arg & 31u)) & 1u;
    g_ecx = 0x4444u;
    g_esp += 4u;
}
game_guest_function recomp_lookup(uint32_t va)
{
    switch (va) {
    case 0x003533F0u: return leaf_3533f0;
    case 0x0035E970u: return leaf_35e970;
    case 0x00355EC0u: return leaf_355ec0;
    case 0x00069640u: return leaf_69640;
    case 0x0035AF40u: return leaf_35af40;
    case 0x000389B0u: return leaf_389b0;
    case 0x003568C0u: return leaf_3568c0;
    default: return NULL;
    }
}
extern void sub_002BE9B0(void), sub_002D28F0(void), sub_00358130(void), sub_00358160(void);
extern void sub_00356080(void), sub_00356870(void), sub_00358560(void), sub_0035AFB0(void);
extern void sub_0035B140(void), sub_00359910(void), sub_002E7270(void), sub_003560E0(void);

static void prepare(unsigned arguments, const uint32_t *values)
{
    g_esp = ROOT_STACK;
    entry_esp = ROOT_STACK;
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
static void set_net(uint32_t network, uint32_t mode, uint32_t state)
{
    guest_write32(NETWORK, network);
    guest_write32(MODE, mode);
    guest_write32(STATE, state);
}
static void test_setup_availability(void)
{
    static const uint32_t table[5] = {0u, 1u, 2u, 3u, 0xFFFFFFFFu};
    player_result = 0x00400000u;
    for (unsigned t = 0u; t < 5u; t++) {
        for (unsigned flag = 0u; flag < 2u; flag++) {
            for (unsigned mode = 0u; mode < 2u; mode++) {
                for (unsigned network = 0u; network < 2u; network++) {
                    const uint8_t col = t == 3u ? 255u : 1u, row = 3u;
                    prepare(0u, NULL);
                    guest_write8(0x0078ADADu, col);
                    guest_write8(0x0078ADAEu, row);
                    guest_write32(0x004D1F84u + 96u, table[t]);
                    guest_write32(player_result + col * 20u + 0x24u, flag != 0u ? 0x80000000u : 0u);
                    set_net(network, mode == 0u ? 0x65u : 0x64u, 1u);
                    sub_002BE9B0();
                    const int kind_ok = t == 1u || (t == 2u && flag != 0u);
                    const int use_network = kind_ok && mode != 0u;
                    CHECK(g_eax == (kind_ok && (mode == 0u || network != 0u) ? 1u : 0u));
                    CHECK(calls == (use_network ? 2u : 1u));
                    CHECK(call_log[0] == 0x002BE9B5u && (!use_network || call_log[1] == 0x002BE9F0u));
                    CHECK(g_ecx == player_result + col * 20u + 0x24u && g_edx == 96u);
                    check_preserved_state();
                }
            }
        }
    }
}

static void test_descriptor_by_global(void)
{
    static const uint32_t values[6] = {1u, 2u, 3u, 0u, 4u, 0xFFFFFFFFu};
    for (unsigned v = 0u; v < 6u; v++) {
        prepare(0u, NULL);
        guest_write32(0x0076BBC8u, values[v]);
        sub_002D28F0();
        const uint32_t expected = values[v] == 1u ? 0x4E9A14u : (values[v] == 2u || values[v] == 3u ? 0x4E9A24u : 0u);
        CHECK(g_eax == expected);
        CHECK(calls == (values[v] == 1u ? 1u : (values[v] == 2u ? 2u : 3u)));
        CHECK(call_log[0] == 0x002D28F8u && (calls < 2u || call_log[1] == 0x002D2909u));
        CHECK(calls < 3u || call_log[2] == 0x002D291Au);
        check_preserved_state();
    }
}

static void test_pickup_roots(void)
{
    static const uint32_t marks[5] = {6u, 7u, 0u, 5u, 0xFFFFFFFFu};
    for (unsigned network = 0u; network < 2u; network++) {
        for (unsigned mode = 0u; mode < 2u; mode++) {
            for (unsigned state = 0u; state < 3u; state++) {
                const uint32_t m = mode == 0u ? 0x65u : 0x64u;
                for (unsigned flag = 0u; flag < 2u; flag++) {
                    prepare(0u, NULL);
                    set_net(network, m, state);
                    guest_write32(0x00541E9Cu, flag != 0u ? 5u : 0u);
                    sub_00358130();
                    if (network == 0u) {
                        CHECK(g_eax == 0u && calls == 1u && g_ecx == REGISTER_ECX);
                    } else if (ref_355ec0() != 0u) {
                        CHECK(g_eax == 0u && calls == 2u && g_ecx == state);
                        CHECK(guest_read32(0x00541E9Cu) == (flag != 0u ? 5u : 0u));
                    } else {
                        CHECK(g_eax == (flag != 0u ? 5u : 0u) && guest_read32(0x00541E9Cu) == 1u);
                    }
                    CHECK(call_log[0] == 0x00358135u && (calls < 2u || call_log[1] == 0x0035813Eu));
                    check_preserved_state();
                }
                for (unsigned k = 0u; k < 5u; k++) {
                    prepare(0u, NULL);
                    set_net(network, m, state);
                    guest_write32(0x007DE30Cu, marks[k]);
                    guest_write32(0x00541EA4u, 0x1234u);
                    guest_write32(0x00519750u, 0x7FFFFFFFu);
                    sub_00358160();
                    if (network == 0u) {
                        CHECK(g_eax == 0u && calls == 1u);
                    } else if (ref_355ec0() == 0u && (marks[k] == 6u || marks[k] == 7u)) {
                        CHECK(g_eax == 0x1234u && guest_read32(0x00541EA4u) == 0xFFFFFFFFu);
                        CHECK(g_ecx == state && guest_read32(0x00519750u) == 0x7FFFFFFFu);
                    } else {
                        CHECK(g_eax == 0x7FFFFFFFu && guest_read32(0x00519750u) == 0x80000000u);
                        CHECK(g_ecx == 0x80000000u && guest_read32(0x00541EA4u) == 0x1234u);
                    }
                    CHECK(call_log[0] == 0x00358165u && (calls < 2u || call_log[1] == 0x0035816Eu));
                    check_preserved_state();
                }
            }
        }
    }
}

static void build_session(uint32_t count)
{
    guest_write32(SESSION, 0x00300000u);
    guest_write32(0x00300000u + 0x20u, count);
    for (unsigned i = 0u; i < 4u; i++) {
        guest_write32(0x00300000u + 0x0Cu + 4u * i, 0x00301000u + 0x40u * i);
        guest_write32(0x00301000u + 0x40u * i + 0x3Cu, 0x11u * (i + 1u));
    }
}

static void test_session_lookups(void)
{
    static const uint32_t counts[6] = {0u, 1u, 3u, 4u, 0xFFFFFFFFu, 0x80000000u};
    static const uint32_t keys[6] = {0x11u, 0x22u, 0x44u, 0x55u, 0u, 0xFFFFFFFFu};
    for (unsigned network = 0u; network < 2u; network++) {
        for (unsigned c = 0u; c < 6u; c++) {
            for (unsigned k = 0u; k < 6u; k++) {
                prepare(1u, &keys[k]);
                set_net(network, 0x64u, 1u);
                build_session(counts[c]);
                sub_00356080();
                uint32_t expected = 0xFFFFFFFFu;
                for (uint32_t i = 0u; (int32_t)counts[c] > 0 && i < counts[c] && i < 4u; i++) {
                    if (keys[k] == 0x11u * (i + 1u)) { expected = i; break; }
                }
                if (network == 0u) expected = keys[k];
                CHECK(g_eax == expected && calls == 1u && call_log[0] == 0x00356085u);
                if (network != 0u && (int32_t)counts[c] > 0) {
                    CHECK(g_edx == counts[c] && g_ecx >= 0x00300000u + 0x0Cu);
                }
                check_preserved_state();
            }
        }
    }
    /* Slot lookups: offline, no session, empty slot and filled slot (index -1 reads +8). */
    static const uint32_t slots[5] = {0u, 1u, 3u, 0xFFFFFFFFu, 4u};
    for (unsigned s = 0u; s < 5u; s++) {
        for (unsigned mode = 0u; mode < 4u; mode++) {
            prepare(1u, &slots[s]);
            set_net(mode == 0u ? 0u : 1u, 0x64u, 1u);
            guest_write32(SESSION, mode == 1u ? 0u : 0x00300000u);
            for (int i = -1; i < 5; i++) {
                guest_write32(0x00300000u + 0x0Cu + 4u * (uint32_t)i, (i & 1) != 0 ? 0u : 0x00301000u);
            }
            guest_write32(0x00301000u + 0x3Cu, 0x77u + s);
            sub_00356870();
            const uint32_t slot_pointer = guest_read32(0x00300000u + 0x0Cu + 4u * slots[s]);
            uint32_t expected = slots[s];
            unsigned expected_calls = 1u;
            if (mode == 0u) {
                expected_calls = 3u;
            } else if (mode != 1u) {
                expected_calls = slot_pointer == 0u ? 2u : 3u;
                if (slot_pointer != 0u) expected = 0x77u + s;
            }
            CHECK(g_eax == expected && calls == expected_calls);
            check_preserved_state();
        }
    }
}

static void test_replication_guards(void)
{
    static const uint32_t pairs[6][2] = {{5u, 5u}, {4u, 5u}, {6u, 5u}, {0x80000000u, 1u}, {0u, 0x80000000u}, {0x7FFFFFFFu, 0xFFFFFFFFu}};
    const uint32_t pointer = 0x00300000u, inner = 0x00300200u;
    for (unsigned network = 0u; network < 2u; network++) {
        for (unsigned p = 0u; p < 6u; p++) {
            for (unsigned ms = 0u; ms < 4u; ms++) {
                prepare(1u, &pointer);
                set_net(network, ms < 2u ? 0x65u : 0x64u, ms % 2u + 1u);
                guest_write32(pointer + 0x7Cu, inner);
                guest_write32(inner + 8u, pairs[p][0]);
                guest_write32(LIMIT, pairs[p][1]);
                sub_00358560();
                const uint32_t difference = pairs[p][0] - pairs[p][1];
                uint32_t expected = 1u;
                if (network != 0u) expected = (difference & 0x80000000u) != 0u ? 0u : ref_355ec0();
                CHECK(g_eax == expected);
                CHECK(call_log[0] == 0x00358565u);
                if (network != 0u) {
                    CHECK(g_edx == difference && g_ecx == (((difference & 0x80000000u) != 0u) ? inner : guest_read32(STATE)));
                }
                check_preserved_state();
            }
        }
    }
    /* 35AFB0: esi carries [arg + 0x14] into the 35AF40 leaf and is restored. */
    const uint32_t object = 0x00300400u;
    for (unsigned network = 0u; network < 2u; network++) {
        for (unsigned flag = 0u; flag < 2u; flag++) {
            for (unsigned e = 0u; e < 2u; e++) {
                prepare(1u, &pointer);
                set_net(network, 0x64u, 1u);
                guest_write32(0x0076B180u, flag);
                guest_write32(pointer + 0x14u, e == 0u ? 0u : object);
                seen_esi = 0xDEADu;
                sub_0035AFB0();
                if (network == 0u || flag != 0u) {
                    CHECK(g_eax == 1u && seen_esi == 0xDEADu && calls == 1u);
                } else {
                    CHECK(calls == 2u && call_log[1] == 0x0035AFCFu);
                    CHECK(seen_esi == (e == 0u ? 0u : object));
                    CHECK(g_eax == (e == 0u ? 0x77u : (object ^ 0x5Au)));
                    CHECK(g_ecx == 0xA1A1u && g_edx == 0xB2B2u);
                }
                check_preserved_state();
            }
        }
    }
    /* 35B140: two arguments, kind word of [arg0 + 0x7C] + 0xC, ESI = the second word. */
    static const uint32_t kinds[3] = {0x20u, 1u, 5u};
    for (unsigned network = 0u; network < 2u; network++) {
        for (unsigned flag = 0u; flag < 2u; flag++) {
            for (unsigned k = 0u; k < 3u; k++) {
                for (unsigned ms = 0u; ms < 4u; ms++) {
                    const uint32_t arguments[2] = {pointer, object};
                    prepare(2u, arguments);
                    set_net(network, ms < 2u ? 0x65u : 0x64u, ms % 2u + 1u);
                    guest_write32(0x0076B16Cu, flag);
                    guest_write32(pointer + 0x7Cu, inner);
                    guest_write32(inner + 0xCu, kinds[k]);
                    seen_esi = 0xDEADu;
                    sub_0035B140();
                    if (network == 0u || flag != 0u) {
                        CHECK(g_eax == 1u && calls == 1u && seen_esi == 0xDEADu);
                    } else if (k < 2u) {
                        CHECK(g_eax == ref_355ec0() && calls == 2u && call_log[1] == 0x0035B178u);
                        CHECK(seen_esi == 0xDEADu);
                    } else {
                        CHECK(calls == 2u && call_log[1] == 0x0035B171u && seen_esi == object);
                        CHECK(g_eax == (object ^ 0x5Au) && g_ecx == 0xA1A1u && g_edx == 0xB2B2u);
                    }
                    check_preserved_state();
                }
            }
        }
    }
}

static void test_slot_scans(void)
{
    const uint32_t key_object = 0x00300000u, same = 0x00310000u, other = 0x00310100u;
    static const uint32_t masks[4] = {0u, 0x0008u, 0x8208u, 0xFFFFu};
    for (unsigned m = 0u; m < 4u; m++) {
        for (unsigned equal = 0u; equal < 3u; equal++) {
            prepare(1u, &key_object);
            connected_mask = masks[m];
            guest_write32(RECORDS, 0x00320000u);
            guest_write32(key_object, 0x1234u);
            guest_write32(same, 0x1234u);
            guest_write32(other, 0x1235u);
            int32_t best = 0x7FFFFFFF;
            for (uint32_t slot = 0u; slot < 16u; slot++) {
                const uint32_t pointer = (equal == 1u || (equal == 2u && slot == 3u)) ? same : other;
                guest_write32(0x00777244u + slot * 0x124u, pointer);
                const int16_t weight = (int16_t)(slot == 15u ? -300 : (int16_t)(1000 - 37 * (int)slot));
                memcpy(&memory[0x00320000u + slot * 0xEA0u + 0xA60u], &weight, 2);
                if (((masks[m] >> slot) & 1u) != 0u && pointer == other && weight < best) best = weight;
            }
            sub_00359910();
            CHECK(g_eax == (uint32_t)best && calls == 16u);
            CHECK(call_log[0] == 0x00359928u && call_log[15] == 0x00359928u);
            check_preserved_state();
        }
    }
    /* 2E7270: roster entries, negative ids, null base and the value compare. */
    for (unsigned scenario = 0u; scenario < 7u; scenario++) {
        const uint32_t wanted = 0x40u;
        prepare(1u, &wanted);
        connected_mask = 0x3Cu;
        guest_write32(0x007A2958u, 2u);
        guest_write32(LIMIT, 2u);
        guest_write32(RECORDS, 0x00340000u);
        static const uint32_t ids[4] = {1u, 0xFFFFFFFFu, 2u, 3u};
        for (uint32_t i = 0u; i < 4u; i++) {
            guest_write32(0x007DE4C0u + 4u * i, ids[i]);
            const uint32_t record = 0x00340000u + ids[i] * 0xEA0u;
            if ((int32_t)ids[i] >= 0) {
                guest_write32(record + 8u, i + 2u);
                guest_write32(record + 0x84u, scenario == i ? wanted + 1u : wanted);
            }
        }
        sub_002E7270();
        unsigned expected = 0u, expected_calls = 0u;
        for (uint32_t i = 0u; i < 4u; i++) {
            if ((int32_t)ids[i] < 0) continue;
            expected_calls++;
            if (((0x3Cu >> (i + 2u)) & 1u) != 0u && scenario != i) expected++;
        }
        CHECK(g_eax == expected && calls == expected_calls);
        CHECK(g_ecx == 2u);
        check_preserved_state();
    }
    /* Null record: id 0 with a zero base is skipped without a call. */
    {
        const uint32_t one = 0x40u;
        prepare(1u, &one);
        guest_write32(0x007A2958u, 1u);
        guest_write32(LIMIT, 0u);
        guest_write32(RECORDS, 0u);
        guest_write32(0x007DE4C0u, 0u);
        sub_002E7270();
        CHECK(g_eax == 0u && calls == 0u && g_ecx == 0u);
        check_preserved_state();
    }
    /* Empty roster: no loop, ECX is the limit word. */
    const uint32_t wanted = 0x40u;
    prepare(1u, &wanted);
    guest_write32(0x007A2958u, 0u);
    guest_write32(LIMIT, 0u);
    sub_002E7270();
    CHECK(g_eax == 0u && calls == 0u && g_ecx == 0u && g_edx == REGISTER_EDX);
    check_preserved_state();
}

static void test_record_init(void)
{
    static const uint32_t indices[6] = {0u, 1u, 15u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u};
    for (unsigned i = 0u; i < 6u; i++) {
        for (unsigned flag = 0u; flag < 3u; flag++) {
            const uint32_t arguments[2] = {indices[i], 0x12345678u};
            const uint32_t flags[3] = {0u, 0x10u, 0xFFFFFFFFu};
            prepare(2u, arguments);
            const uint32_t base = indices[i] * 0x124u;
            guest_write32(base + 0x0077728Cu, flags[flag]);
            sub_003560E0();
            CHECK(calls == 1u && call_log[0] == 0x003560E7u);
            CHECK(guest_read32(base + 0x0077727Cu) == indices[i]);
            CHECK(guest_read32(base + 0x00777284u) == indices[i]);
            CHECK(guest_read32(base + 0x00777244u) == 0x12345678u);
            CHECK(guest_read32(base + 0x0077728Cu) == (flags[flag] | 1u));
            CHECK(guest_read32(base + 0x00777360u) == 0u);
            CHECK(guest_read32(base + 0x00777348u) == 0xFFFFFFFFu);
            CHECK(g_eax == base && g_ecx == (flags[flag] | 1u) && g_edx == 0x12345678u);
            check_preserved_state();
        }
    }
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    test_setup_availability();
    test_descriptor_by_global();
    test_pickup_roots();
    test_session_lookups();
    test_replication_guards();
    test_slot_scans();
    test_record_init();
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0u;
}
