/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 batch A native tests for the guarded-jump roots of src/game/game_guarded_batch_a.c
 * (included by test_game_replacements.c after test_game_fp_guarded.inc).
 *
 * Expectations are hand-derived from the retail disassembly (register outputs, memory words, call
 * arguments, return VAs and the modelled stack depth), not by running the replacement against
 * itself. The byte table of 0xBE860 is copied from the retail image (docs/evidence/t1576/
 * batch-a.md). Nested guest callees are recorded stubs reached through recomp_lookup while
 * ga_active (entry ESP 0x100, caller return address 0x12345678 written by prepare()).
 */

#define GA_ARENA_REMAINING 0x006B8384u
#define GA_ARENA_TOP 0x006B8388u
#define GA_ARENA_CURSOR_1 0x006B837Cu
#define GA_ARENA_CURSOR_2 0x006B8370u
#define GA_ARENA_BUDGET_2 0x006B8378u
#define GA_ARENA_CURSOR_4 0x006B8390u

typedef struct {
    uint32_t va;
    int set_eax, set_ecx, set_edx;
    uint32_t eax, ecx, edx;
} ga_stub_def;

typedef struct {
    uint32_t va, ret_va, esp, eax, ecx, edx, arg0, arg1, top, remaining;
} ga_call;

#define GA_STUBS 9u
static ga_stub_def ga_stubs[GA_STUBS] = {
    {0x000389B0u}, {0x0003DFA0u}, {0x00018CE0u}, {0x001B99B0u}, {0x000BE860u},
    {0x0003D7B0u}, {0x000D6DD0u}, {0x001CEB00u}, {0x000E0540u}};
static ga_call ga_log[64];
static unsigned ga_log_count;

static void ga_hit(unsigned index)
{
    ga_stub_def *stub = &ga_stubs[index];
    ga_call *entry = &ga_log[ga_log_count < 64u ? ga_log_count : 63u];
    ga_log_count++;
    entry->va = stub->va;
    entry->ret_va = guest_read32(g_esp);
    entry->esp = g_esp;
    entry->eax = g_eax;
    entry->ecx = g_ecx;
    entry->edx = g_edx;
    entry->arg0 = game_stack_arg(0u);
    entry->arg1 = game_stack_arg(1u);
    entry->top = guest_read32(GA_ARENA_TOP);
    entry->remaining = guest_read32(GA_ARENA_REMAINING);
    if (stub->set_eax) g_eax = stub->eax;
    if (stub->set_ecx) g_ecx = stub->ecx;
    if (stub->set_edx) g_edx = stub->edx;
    g_esp += 4u;
}

#define GA_STUB_FN(N) static void ga_stub_##N(void) { ga_hit(N); }
GA_STUB_FN(0) GA_STUB_FN(1) GA_STUB_FN(2) GA_STUB_FN(3) GA_STUB_FN(4) GA_STUB_FN(5)
GA_STUB_FN(6) GA_STUB_FN(7) GA_STUB_FN(8)
static game_guest_function const ga_stub_fns[GA_STUBS] = {
    ga_stub_0, ga_stub_1, ga_stub_2, ga_stub_3, ga_stub_4, ga_stub_5, ga_stub_6, ga_stub_7,
    ga_stub_8};

static game_guest_function ga_lookup(uint32_t xbox_va)
{
    for (unsigned i = 0; i < GA_STUBS; i++)
        if (ga_stubs[i].va == xbox_va) return ga_stub_fns[i];
    return NULL;
}

static ga_stub_def *ga_stub(uint32_t va)
{
    for (unsigned i = 0; i < GA_STUBS; i++)
        if (ga_stubs[i].va == va) return &ga_stubs[i];
    CHECK(0);
    return &ga_stubs[0];
}

static void ga_set(uint32_t va, int set_eax, uint32_t eax, int set_ecx, uint32_t ecx,
                   int set_edx, uint32_t edx)
{
    ga_stub_def *stub = ga_stub(va);
    stub->set_eax = set_eax;
    stub->eax = eax;
    stub->set_ecx = set_ecx;
    stub->ecx = ecx;
    stub->set_edx = set_edx;
    stub->edx = edx;
}

static void ga_begin(void)
{
    prepare();
    ga_log_count = 0u;
    guest_write32(GA_ARENA_REMAINING, 0u);
    guest_write32(GA_ARENA_TOP, 0u);
    guest_write32(GA_ARENA_CURSOR_1, 0u);
    guest_write32(GA_ARENA_CURSOR_2, 0u);
    guest_write32(GA_ARENA_BUDGET_2, 0u);
    guest_write32(GA_ARENA_CURSOR_4, 0u);
}

static void ga_clear_stubs(void)
{
    for (unsigned i = 0; i < GA_STUBS; i++) {
        ga_stubs[i].set_eax = ga_stubs[i].set_ecx = ga_stubs[i].set_edx = 0;
    }
}

static void ga_args(uint32_t a0, uint32_t a1, uint32_t a2)
{
    guest_write32(g_esp + 4u, a0);
    guest_write32(g_esp + 8u, a1);
    guest_write32(g_esp + 12u, a2);
}

static void ga_arena(uint32_t remaining, uint32_t top, uint32_t c1, uint32_t c2, uint32_t b2,
                     uint32_t c4)
{
    guest_write32(GA_ARENA_REMAINING, remaining);
    guest_write32(GA_ARENA_TOP, top);
    guest_write32(GA_ARENA_CURSOR_1, c1);
    guest_write32(GA_ARENA_CURSOR_2, c2);
    guest_write32(GA_ARENA_BUDGET_2, b2);
    guest_write32(GA_ARENA_CURSOR_4, c4);
}

static void ga_check_hooks(unsigned expected)
{
    CHECK(ga_log_count == expected);
    for (unsigned i = 0; i < expected && i < 2u; i++) {
        CHECK(ga_log[i].va == 0x000389B0u);
        CHECK(ga_log[i].arg0 == (i == 0u ? 0xFFFFFFFFu : 1u));
    }
}

/* 0x0003DFA0 cdecl(size, kind): S = (size + 15) & ~15, kinds 0 4 per the T1364 contract. */
static void ga_test_3dfa0(void)
{
    ga_clear_stubs();
    /* kind 1, S = 0x20 (size 0x11), plenty left: old cursor returned, ECX = new cursor. */
    ga_begin();
    ga_args(0x11u, 1u, 0u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    CHECK(g_eax == 0x300000u && g_ecx == 0x300020u && g_edx == 0x33333333u);
    CHECK(guest_read32(GA_ARENA_CURSOR_1) == 0x300020u);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0xFE0u);
    CHECK(guest_read32(GA_ARENA_TOP) == 0x300100u && guest_read32(GA_ARENA_CURSOR_2) == 0x310000u);
    ga_check_hooks(0u);

    /* kind 1 exhausted (S = 0x20 > 0x1F signed): the hook 389B0 runs twice, then the same effects. */
    ga_begin();
    ga_args(0x20u, 1u, 0u);
    ga_arena(0x1Fu, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(2u);
    CHECK(ga_log[0].ret_va == 0x0003E041u && ga_log[0].esp == 0xF4u);
    CHECK(ga_log[1].ret_va == 0x0003E048u && ga_log[1].esp == 0xF0u);
    CHECK(g_eax == 0x300000u && g_ecx == 0x300020u);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0xFFFFFFFFu);

    /* kind 1 with S = 0x80000000: signed compare says it fits (no hook). S = 0x10 against -1: hook. */
    ga_begin();
    ga_args(0x80000000u, 1u, 0u);
    ga_arena(0x10u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(0u);
    CHECK(g_eax == 0x300000u && g_ecx == 0x80300000u);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0x80000010u);
    ga_begin();
    ga_args(0x10u, 1u, 0u);
    ga_arena(0xFFFFFFFFu, 0x300100u, 0x300001u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(2u);
    CHECK(g_eax == 0x300001u && g_ecx == 0x300011u);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0xFFFFFFEFu);

    /* kind 2: separate cursor and budget, no capacity check even with the budget exhausted. */
    ga_begin();
    ga_args(5u, 2u, 0u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x400001u, 0x8u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(0u);
    CHECK(g_eax == 0x400001u && g_ecx == 0x400011u && g_edx == 0x33333333u);
    CHECK(guest_read32(GA_ARENA_CURSOR_2) == 0x400011u);
    CHECK(guest_read32(GA_ARENA_BUDGET_2) == 0xFFFFFFF8u);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0x1000u);

    /* kind 4: cursor 4 only, S = 0 for size 0 (cursor unchanged, still stored) and 0x30 for 0x21. */
    ga_begin();
    ga_args(0u, 4u, 0u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x500000u);
    invoke(0x0003DFA0u, 4u);
    CHECK(g_eax == 0x500000u && g_ecx == 0x500000u);
    CHECK(guest_read32(GA_ARENA_CURSOR_4) == 0x500000u);
    ga_begin();
    ga_args(0x21u, 4u, 0u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x500000u);
    invoke(0x0003DFA0u, 4u);
    CHECK(g_eax == 0x500000u && g_ecx == 0x500030u);
    CHECK(guest_read32(GA_ARENA_CURSOR_4) == 0x500030u && guest_read32(GA_ARENA_REMAINING) == 0x1000u);

    /* kind 0, S = 0x20: header at top - S - 16 ("LOCALMEM" tags, size, self pointer). */
    ga_begin();
    ga_args(0x11u, 0u, 0u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(0u);
    CHECK(guest_read32(GA_ARENA_TOP) == 0x3000D0u);
    CHECK(guest_read32(0x3000D0u) == 0x41434F4Cu && guest_read32(0x3000D4u) == 0x4D454D4Cu);
    CHECK(guest_read32(0x3000D8u) == 0x20u && guest_read32(0x3000DCu) == 0x3000D0u);
    CHECK(g_eax == 0x3000E0u && g_ecx == 0xFFFFFFD0u && g_edx == 0xFD0u);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0xFD0u);
    /* kind 0 exhausted by one byte (S + 16 = 0x30 > 0x2F): hook twice, remaining wraps to -1. */
    ga_begin();
    ga_args(0x11u, 0u, 0u);
    ga_arena(0x2Fu, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(2u);
    CHECK(ga_log[0].ret_va == 0x0003DFD3u && ga_log[1].ret_va == 0x0003DFDAu);
    CHECK(g_eax == 0x3000E0u && g_edx == 0xFFFFFFFFu);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0xFFFFFFFFu);

    /* kind 0 compares S + 16 with the remaining budget SIGNED (cmp / jle): the negative budget
     * -16 is below S + 16 = 0x30, so the hook runs twice (an unsigned compare would skip it). */
    ga_begin();
    ga_args(0x11u, 0u, 0u);
    ga_arena(0xFFFFFFF0u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003DFA0u, 4u);
    ga_check_hooks(2u);

    /* kind 3 and every unsigned kind above 4 (also a negative one): 0, nothing written. */
    const uint32_t zero_kinds[4] = {3u, 5u, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned i = 0; i < 4u; i++) {
        ga_begin();
        ga_args(0x40u, zero_kinds[i], 0u);
        ga_arena(0x1000u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
        invoke(0x0003DFA0u, 4u);
        ga_check_hooks(0u);
        CHECK(g_eax == 0u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
        CHECK(guest_read32(GA_ARENA_REMAINING) == 0x1000u && guest_read32(GA_ARENA_TOP) == 0x300100u);
        CHECK(guest_read32(GA_ARENA_CURSOR_1) == 0x300000u && guest_read32(GA_ARENA_CURSOR_4) == 0x320000u);
    }
}

/* 0x0003E0B0 cdecl(alignment, size, kind): the wrapper around 3DFA0(size, kind). */
static void ga_test_3e0b0(void)
{
    ga_clear_stubs();
    ga_set(0x0003DFA0u, 1, 0xAAAA0000u, 1, 0xC1C1C1C1u, 1, 0xD2D2D2D2u);

    /* kind 1: cursor 0x300001 padded to 0x300010 (pad 0xF debited), then 3DFA0(0x30, 1). */
    ga_begin();
    ga_args(0x10u, 0x30u, 1u);
    ga_arena(0x1000u, 0x300100u, 0x300001u, 0x310000u, 0x80u, 0x320000u);
    invoke(0x0003E0B0u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].va == 0x0003DFA0u && ga_log[0].ret_va == 0x0003E0FBu);
    CHECK(ga_log[0].esp == 0xF0u && ga_log[0].arg0 == 0x30u && ga_log[0].arg1 == 1u);
    CHECK(ga_log[0].remaining == 0xFF1u);
    CHECK(guest_read32(GA_ARENA_CURSOR_1) == 0x300010u && guest_read32(GA_ARENA_REMAINING) == 0xFF1u);
    CHECK(g_eax == 0xAAAA0000u && g_ecx == 0xC1C1C1C1u && g_edx == 0xD2D2D2D2u);

    /* kind 2: cursor 0x400002 aligned to 4 (pad 2) against budget 0x80. */
    ga_begin();
    ga_args(4u, 0x10u, 2u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x400002u, 0x80u, 0x320000u);
    invoke(0x0003E0B0u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].ret_va == 0x0003E136u);
    CHECK(ga_log[0].arg0 == 0x10u && ga_log[0].arg1 == 2u);
    CHECK(guest_read32(GA_ARENA_CURSOR_2) == 0x400004u && guest_read32(GA_ARENA_BUDGET_2) == 0x7Eu);
    CHECK(guest_read32(GA_ARENA_REMAINING) == 0x1000u);
    CHECK(g_eax == 0xAAAA0000u && g_ecx == 0xC1C1C1C1u);

    /* kind 0 with a size that is a multiple of the alignment: up(0x20) = 0x20, not 0x40 (the
     * rounding adds alignment - 1). Top 0x300000 is already aligned: pad 0, D = -0x20. */
    ga_begin();
    ga_args(0x20u, 0x20u, 0u);
    ga_arena(0x1000u, 0x300000u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    guest_write32(0x2FFFE0u + 8u, 0x20u);
    invoke(0x0003E0B0u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].arg0 == 0x20u && ga_log[0].arg1 == 0u);
    CHECK(ga_log[0].top == 0x2FFFE0u && ga_log[0].remaining == 0xFE0u);
    CHECK(guest_read32(0x2FFFE8u) == 0x20u + 0x20u);

    /* kind 4: cursor 0x500001 aligned to 0x20 (0x500020), then the delegation falls through. */
    ga_begin();
    ga_args(0x20u, 0x44u, 4u);
    ga_arena(0x1000u, 0x300100u, 0x300000u, 0x310000u, 0x80u, 0x500001u);
    invoke(0x0003E0B0u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].ret_va == 0x0003E1BDu);
    CHECK(ga_log[0].arg0 == 0x44u && ga_log[0].arg1 == 4u && ga_log[0].esp == 0xF0u);
    CHECK(guest_read32(GA_ARENA_CURSOR_4) == 0x500020u);

    /* kind 3 and kind 7: delegated unchanged, no arena word written. */
    const uint32_t delegated[2] = {3u, 7u};
    for (unsigned i = 0; i < 2u; i++) {
        ga_begin();
        ga_args(0x20u, 0x55u, delegated[i]);
        ga_arena(0x1000u, 0x300100u, 0x300001u, 0x310001u, 0x80u, 0x500001u);
        invoke(0x0003E0B0u, 4u);
        CHECK(ga_log_count == 1u && ga_log[0].ret_va == 0x0003E1BDu);
        CHECK(ga_log[0].arg0 == 0x55u && ga_log[0].arg1 == delegated[i]);
        CHECK(guest_read32(GA_ARENA_CURSOR_1) == 0x300001u && guest_read32(GA_ARENA_CURSOR_4) == 0x500001u);
        CHECK(guest_read32(GA_ARENA_REMAINING) == 0x1000u && guest_read32(GA_ARENA_TOP) == 0x300100u);
    }

    /* kind 0: top 0x300007, alignment 0x20, size 0x11: pad 0x19, D = -7, top 0x300000, remaining
     * 0xFF9 before the call 3DFA0(up(size) = 0x20, 0). The header size at top + 8 (0x20 written by the
     * callee) then grows by alignment - pad = 7. */
    ga_begin();
    ga_args(0x20u, 0x11u, 0u);
    ga_arena(0x1000u, 0x300007u, 0x300000u, 0x310000u, 0x80u, 0x320000u);
    guest_write32(0x300008u, 0x20u);
    invoke(0x0003E0B0u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].ret_va == 0x0003E183u && ga_log[0].esp == 0xECu);
    CHECK(ga_log[0].arg0 == 0x20u && ga_log[0].arg1 == 0u);
    CHECK(ga_log[0].top == 0x300000u && ga_log[0].remaining == 0xFF9u);
    CHECK(guest_read32(0x300008u) == 0x27u);
    CHECK(g_eax == 0xAAAA0000u && g_ecx == 0x300000u && g_edx == 0x27u);
}

/* 0x0008E380 cdecl(blob): marks each record key unloaded, tail jmp to the bare RET 389B0. */
static void ga_test_8e380(void)
{
    ga_clear_stubs();
    const uint32_t blob = 0x200000u;
    const uint32_t entries = 0x200100u;
    ga_set(0x00018CE0u, 1, 0x18CE0AAAu, 1, 0x18CE0CCCu, 1, 0x18CE0DDDu);

    /* null blob and null entry array return at once, registers as they entered. */
    ga_begin();
    guest_write32(g_esp + 4u, 0u);
    invoke(0x0008E380u, 4u);
    CHECK(ga_log_count == 0u && g_eax == 0x11111111u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    ga_begin();
    guest_write32(g_esp + 4u, blob);
    guest_write32(blob + 0x10u, 0u);
    invoke(0x0008E380u, 4u);
    CHECK(ga_log_count == 0u && g_eax == 0x11111111u && g_ecx == 0x22222222u);

    /* first key already -1: no 18CE0, straight into the tail (the caller's return address). */
    ga_begin();
    guest_write32(g_esp + 4u, blob);
    guest_write32(blob + 0x10u, entries);
    guest_write32(entries, 0xFFFFFFFFu);
    invoke(0x0008E380u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].va == 0x000389B0u);
    CHECK(ga_log[0].ret_va == 0x12345678u && ga_log[0].esp == 0x100u);
    CHECK(g_eax == 0x11111111u && g_ecx == 0x22222222u && g_edx == 0x33333333u);
    CHECK(guest_read32(entries) == 0xFFFFFFFFu);

    /* keys 5 and 6 then -1: 18CE0(5), 18CE0(6) (return VA 8E3A8, ESP 0xEC), keys rewritten to
     * 0x40000000, then the tail. EAX is the terminator, ECX the array pointer, EDX the callee's. */
    ga_begin();
    guest_write32(g_esp + 4u, blob);
    guest_write32(blob + 0x10u, entries);
    guest_write32(entries, 5u);
    guest_write32(entries + 0x10u, 6u);
    guest_write32(entries + 0x20u, 0xFFFFFFFFu);
    invoke(0x0008E380u, 4u);
    CHECK(ga_log_count == 3u);
    CHECK(ga_log[0].va == 0x00018CE0u && ga_log[0].arg0 == 5u && ga_log[0].ret_va == 0x0008E3A8u);
    CHECK(ga_log[1].va == 0x00018CE0u && ga_log[1].arg0 == 6u && ga_log[1].esp == 0xECu);
    CHECK(ga_log[2].va == 0x000389B0u && ga_log[2].ret_va == 0x12345678u && ga_log[2].esp == 0x100u);
    CHECK(guest_read32(entries) == 0x40000000u && guest_read32(entries + 0x10u) == 0x40000000u);
    CHECK(guest_read32(entries + 0x20u) == 0xFFFFFFFFu);
    CHECK(g_eax == 0xFFFFFFFFu && g_ecx == entries && g_edx == 0x18CE0DDDu);
}

static void ga_fill_type_mask_table(void)
{
    /* Retail byte table at BE950 (mask - 1 -> slot, 6 = no entry). */
    uint8_t table[0x40];
    memset(table, 6, sizeof table);
    table[0] = 0;
    table[1] = 1;
    table[7] = 2;
    table[15] = 3;
    table[31] = 4;
    table[63] = 5;
    memcpy(&memory[0x000BE950u], table, sizeof table);
}

/* 0x000BE860 cdecl(type_mask, index) and 0x000C0E60 which forwards to it. */
static void ga_be860(uint32_t mask, uint32_t index, uint32_t eax, uint32_t ecx)
{
    ga_begin();
    ga_fill_type_mask_table();
    ga_set(0x001B99B0u, 1, 0x7u, 1, 0xB9B9B9B9u, 0, 0u);
    ga_args(mask, index, 0u);
    invoke(0x000BE860u, 4u);
    CHECK(g_eax == eax);
    CHECK(g_ecx == ecx);
}

static void ga_test_be860(void)
{
    ga_be860(1u, 2u, 0x4DC2C0u, 0u);
    ga_be860(2u, 1u, 0x4FB1E4u, 1u);
    ga_be860(8u, 0u, 0x4DBA38u, 2u);
    ga_be860(0x10u, 3u, 0x4DBF24u, 3u);
    ga_be860(0x20u, 1u, 0x4DC6BCu, 4u);
    ga_be860(3u, 5u, 0u, 6u);
    ga_be860(0x41u, 5u, 0u, 0x40u);
    ga_be860(0u, 5u, 0u, 0xFFFFFFFFu);
    ga_be860(0x80000000u, 5u, 0u, 0x7FFFFFFFu);
    ga_be860(5u, 0xFFFFFFFFu, 0u, 0x22222222u);
    ga_be860(0x80u, 2u, 0x4DC468u, 0x80u);
    ga_be860(0x100u, 1u, 0x4DC7BCu, 0x100u);
    ga_be860(0x1000u, 1u, 0x4DC7BCu, 0x1000u);
    ga_be860(0x200u, 1u, 0x4DDB3Cu, 0x200u);
    ga_be860(0x400u, 1u, 0x4DC564u, 0x400u);
    ga_be860(0x800u, 9u, 0x4DC568u, 0x800u);
    ga_be860(0x81u, 1u, 0u, 0x81u);
    ga_be860(0x300u, 1u, 0u, 0x300u);
    ga_be860(0x401u, 1u, 0u, 0x401u);
    ga_be860(0x1001u, 1u, 0u, 0x1001u);
    ga_be860(0x7FFFFFFFu, 1u, 0u, 0x7FFFFFFFu);
    /* mask 0x40: 1B99B0(index) gives the weapon slot, valid 1..0x2B (7 -> 4EAAA4, 0x2B -> 4ECA24). */
    ga_be860(0x40u, 5u, 0x4EAAA4u, 0xB9B9B9B9u);
    CHECK(ga_log_count == 1u && ga_log[0].va == 0x001B99B0u && ga_log[0].arg0 == 5u);
    CHECK(ga_log[0].ret_va == 0x000BE8B3u);
    const uint32_t weapon[5] = {0x2Bu, 0x2Cu, 0u, 0xFFFFFFFFu, 1u};
    const uint32_t expected[5] = {0x4ECA24u, 0u, 0u, 0u, 0x4EA564u};
    for (unsigned i = 0; i < 5u; i++) {
        ga_begin();
        ga_fill_type_mask_table();
        ga_set(0x001B99B0u, 1, weapon[i], 0, 0u, 0, 0u);
        ga_args(0x40u, 5u, 0u);
        invoke(0x000BE860u, 4u);
        CHECK(g_eax == expected[i] && g_ecx == 5u);
    }
}

static void ga_c0e60(uint32_t type, uint32_t index, uint32_t hint, uint32_t weapon, int forwarded)
{
    ga_begin();
    ga_set(0x000BE860u, 1, 0xBE860E0Eu, 0, 0u, 0, 0u);
    guest_write32(g_esp + 4u, type);
    guest_write32(g_esp + 8u, index);
    guest_write32(g_esp + 12u, 0x12121212u);
    guest_write32(g_esp + 16u, hint);
    guest_write32(g_esp + 20u, weapon);
    invoke(0x000C0E60u, 4u);
    if (forwarded) {
        CHECK(ga_log_count == 1u && ga_log[0].va == 0x000BE860u && ga_log[0].ret_va == 0x000C0E9Eu);
        CHECK(ga_log[0].arg0 == type && ga_log[0].arg1 == index && ga_log[0].esp == 0xF4u);
        CHECK(g_eax == 0xBE860E0Eu);
    } else {
        CHECK(ga_log_count == 0u);
        CHECK(g_eax == weapon * 0xE0u + 0x4EA484u);
    }
    CHECK(g_ecx == index && g_edx == type);
}

static void ga_test_c0e60(void)
{
    ga_c0e60(0x40u, 3u, 0u, 5u, 0);
    ga_c0e60(0x40u, 0u, 0u, 0x2Bu, 0);
    ga_c0e60(0x40u, 0x45u, 0u, 1u, 0);
    ga_c0e60(0x40u, 3u, 1u, 5u, 1);
    ga_c0e60(0x40u, 0x46u, 0u, 5u, 1);
    ga_c0e60(0x40u, 0xFFFFFFFFu, 0u, 5u, 1);
    ga_c0e60(0x41u, 3u, 0u, 5u, 1);
    ga_c0e60(0x40u, 3u, 0u, 0u, 1);
    ga_c0e60(0x40u, 3u, 0u, 0x2Cu, 1);
    ga_c0e60(0x40u, 3u, 0u, 0xFFFFFFFFu, 1);
    ga_c0e60(0x10u, 7u, 0u, 5u, 1);
}

/* 0x000379C0 cdecl(kind, flag): the feedback reason text ids through 3D7B0. */
static void ga_379c0(uint32_t kind, uint32_t flag, int called, uint32_t text, uint32_t ret_va)
{
    ga_begin();
    ga_set(0x0003D7B0u, 1, 0x3D7B0001u, 0, 0u, 0, 0u);
    guest_write32(g_esp + 4u, kind);
    guest_write32(g_esp + 8u, flag);
    invoke(0x000379C0u, 4u);
    if (called) {
        CHECK(ga_log_count == 1u && ga_log[0].va == 0x0003D7B0u && ga_log[0].arg0 == text);
        CHECK(ga_log[0].ret_va == ret_va && ga_log[0].esp == 0xF8u);
        CHECK(g_eax == 0x3D7B0001u);
    } else {
        CHECK(ga_log_count == 0u && g_eax == 0u);
    }
    CHECK(g_ecx == 0x22222222u && g_edx == 0x33333333u);
}

static void ga_test_379c0(void)
{
    static const uint32_t texts[10] = {0x1C25u, 0x1C26u, 0x1C28u, 0x1C27u, 0x1C29u,
                                       0x1C2Au, 0x1C2Bu, 0x1C2Cu, 0x1C2Du, 0x1C2Eu};
    static const uint32_t returns[10] = {0x379EAu, 0x379F8u, 0x37A06u, 0x37A14u, 0x37A22u,
                                         0x37A30u, 0x37A3Eu, 0x37A4Cu, 0x37A5Au, 0x37A68u};
    for (uint32_t kind = 0u; kind < 10u; kind++) ga_379c0(kind, 0u, 1, texts[kind], returns[kind]);
    ga_379c0(10u, 0u, 0, 0u, 0u);
    ga_379c0(0xFFFFFFFFu, 0u, 0, 0u, 0u);
    ga_379c0(10u, 1u, 1, 0x1C2Fu, 0x37A82u);
    ga_379c0(11u, 2u, 1, 0x1C8Bu, 0x37A9Cu);
    ga_379c0(9u, 1u, 0, 0u, 0u);
    ga_379c0(11u, 1u, 0, 0u, 0u);
    ga_379c0(10u, 2u, 0, 0u, 0u);
    ga_379c0(10u, 3u, 0, 0u, 0u);
    ga_379c0(0u, 0xFFFFFFFFu, 0, 0u, 0u);
}

/* 0x000D6ED0 cdecl(id) through D6DD0, and 0x00045A70 through 1CEB00. */
static void ga_d6ed0(uint32_t id, uint32_t callee, uint32_t result)
{
    ga_begin();
    ga_set(0x000D6DD0u, 1, callee, 0, 0u, 0, 0u);
    guest_write32(g_esp + 4u, id);
    invoke(0x000D6ED0u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].arg0 == id && ga_log[0].ret_va == 0x000D6EDAu);
    CHECK(g_eax == result && g_ecx == id && g_edx == 0x33333333u);
}

static void ga_45a70(uint32_t callee, uint32_t near_word, uint32_t far_word, uint32_t result,
                     uint32_t ecx)
{
    ga_begin();
    ga_set(0x001CEB00u, 1, callee, 0, 0u, 0, 0u);
    guest_write32(0x0079094Cu, 0x37u);
    guest_write32(0x005236B4u, near_word);
    guest_write32(0x005236B8u, far_word);
    invoke(0x00045A70u, 4u);
    CHECK(ga_log_count == 1u && ga_log[0].arg0 == 0x37u && ga_log[0].ret_va == 0x00045A7Bu);
    CHECK(g_eax == result && g_ecx == ecx && g_edx == 0x33333333u);
}

static void ga_test_small_callers(void)
{
    ga_d6ed0(0x20u, 1u, 0u);
    ga_d6ed0(0x20u, 0u, 1u);
    ga_d6ed0(0x4Du, 0u, 0u);
    ga_d6ed0(0x4Fu, 0u, 0u);
    ga_d6ed0(0x64u, 0u, 0u);
    ga_d6ed0(0x50u, 0u, 1u);
    ga_d6ed0(0xFFFFFFFFu, 0u, 1u);
    ga_d6ed0(0x4Du, 1u, 0u);

    ga_45a70(1u, 7u, 0u, 1u, 0u);
    ga_45a70(1u, 0u, 5u, 0u, 5u);
    ga_45a70(0u, 0u, 7u, 1u, 0u);
    ga_45a70(0u, 3u, 0u, 0u, 3u);
    ga_45a70(0x1234u, 0u, 9u, 0u, 9u);
}

/* 0x000E05B0 cdecl(owner, block): the entries first..last of the 0x22C-byte table at [737BD4]. */
static void ga_test_e05b0(void)
{
    ga_clear_stubs();
    const uint32_t table = 0x300000u;
    const uint32_t block = 0x200000u;
    ga_set(0x000E0540u, 0, 0u, 1, 0xE0E0E0E0u, 0, 0u);
    ga_begin();
    for (uint32_t offset = 0u; offset < 0x1Cu; offset += 4u) guest_write32(block + offset, 0xA5A5A5A5u);
    guest_write32(g_esp + 4u, 0x77u);
    guest_write32(g_esp + 8u, block);
    guest_write32(0x00736680u, 1u);
    guest_write32(0x00737E44u, 3u);
    guest_write32(0x00737BD4u, table);
    guest_write32(table + 1u * 0x22Cu, 0x10u);
    guest_write32(table + 1u * 0x22Cu + 0x158u, 0x77u);
    guest_write32(table + 2u * 0x22Cu, 0xFFFFFFFFu);
    guest_write32(table + 2u * 0x22Cu + 0x158u, 0x77u);
    guest_write32(table + 3u * 0x22Cu, 0x12u);
    guest_write32(table + 3u * 0x22Cu + 0x158u, 0x78u);
    invoke(0x000E05B0u, 4u);
    for (uint32_t offset = 0u; offset < 0x1Cu; offset += 4u) CHECK(guest_read32(block + offset) == 0u);
    CHECK(ga_log_count == 1u && ga_log[0].va == 0x000E0540u && ga_log[0].ret_va == 0x000E060Bu);
    CHECK(ga_log[0].ecx == 0x10u && ga_log[0].eax == block && ga_log[0].esp == 0xF4u);
    CHECK(g_eax == block && g_ecx == 0x12u && g_edx == table + 4u * 0x22Cu);

    /* An empty range (first > last, and first == last + 1) only clears the block: ECX = last + 1. */
    ga_begin();
    for (uint32_t offset = 0u; offset < 0x1Cu; offset += 4u) guest_write32(block + offset, 0xA5A5A5A5u);
    guest_write32(g_esp + 4u, 0x77u);
    guest_write32(g_esp + 8u, block);
    guest_write32(0x00736680u, 5u);
    guest_write32(0x00737E44u, 2u);
    invoke(0x000E05B0u, 4u);
    for (uint32_t offset = 0u; offset < 0x1Cu; offset += 4u) CHECK(guest_read32(block + offset) == 0u);
    CHECK(ga_log_count == 0u && g_eax == block && g_ecx == 3u && g_edx == 0u);
    ga_begin();
    guest_write32(g_esp + 4u, 0x77u);
    guest_write32(g_esp + 8u, block);
    guest_write32(0x00736680u, 3u);
    guest_write32(0x00737E44u, 2u);
    invoke(0x000E05B0u, 4u);
    CHECK(ga_log_count == 0u && g_ecx == 3u && g_edx == 0u);
}

static void test_t1576_guarded_batch_a(void)
{
    ga_active = 1;
    ga_test_3dfa0();
    ga_test_3e0b0();
    ga_test_8e380();
    ga_test_be860();
    ga_test_c0e60();
    ga_test_379c0();
    ga_test_small_callers();
    ga_test_e05b0();
    ga_active = 0;
}
