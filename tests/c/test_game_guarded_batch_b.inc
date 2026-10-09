/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 batch B native tests for src/game/game_guarded_batch_b.c (included by
 * test_game_replacements.c after test_game_fp_guarded.inc).
 *
 * 0x00345710 expectations are hand-derived from the retail disassembly, not by running the
 * replacement against itself:
 *   call 0x153B40 (eax = random value); xor edx,edx; mov ecx,5; div ecx (unsigned: eax = quotient,
 *   edx = remainder); cmp edx,3; ja default; jmp [edx*4 + 0x345770]; slot k pushes the index
 *   0x3C2, 0x3BE, 0x3BF, 0x3C1 (default 0x3C3), calls 0x80F20 (retail return VAs 0x345734,
 *   0x345742, 0x345750, 0x34575E, default 0x34576C), add esp,4, ret. The callee result stays in eax.
 * The mocks stand in for the two lifted callees: they check the return address and the argument,
 * record them, and leave ECX/EDX as the replacement set them (5 and the remainder). Edges: every
 * slot, the JA default (remainder 4), values above 2^31 (an idiv would take a different path) and
 * the exact remainder boundaries. */

/* gb_active and the gb_lookup declaration live in test_game_replacements.c. */
static uint32_t gb_rng_value;
static uint32_t gb_seen_return;
static uint32_t gb_seen_argument;
static unsigned gb_rng_calls;
static unsigned gb_table_calls;

static void gb_mock_rng(void)
{
    CHECK(guest_read32(g_esp) == 0x00345715u);
    gb_rng_calls++;
    g_eax = gb_rng_value;
    g_esp += 4u;
}

static void gb_mock_table_entry(void)
{
    gb_seen_return = guest_read32(g_esp);
    gb_seen_argument = game_stack_arg(0u);
    gb_table_calls++;
    g_eax = 0xA0000000u + gb_seen_argument;
    g_esp += 4u;
}

static game_guest_function gb_lookup(uint32_t xbox_va)
{
    if (xbox_va == 0x00153B40u) return gb_mock_rng;
    if (xbox_va == 0x00080F20u) return gb_mock_table_entry;
    return NULL;
}

static void gb_345710(uint32_t random_value, uint32_t remainder, uint32_t index, uint32_t return_va)
{
    prepare();
    gb_rng_value = random_value;
    gb_seen_return = gb_seen_argument = 0u;
    gb_rng_calls = gb_table_calls = 0u;
    gb_active = 1;
    invoke(0x00345710u, 4u);
    gb_active = 0;
    CHECK(gb_rng_calls == 1u && gb_table_calls == 1u);
    CHECK(gb_seen_argument == index && gb_seen_return == return_va);
    CHECK(g_eax == 0xA0000000u + index);
    CHECK(g_ecx == 5u && g_edx == remainder);
}

static void test_t1576_batch_b(void)
{
    gb_345710(0u, 0u, 0x3C2u, 0x00345734u);
    gb_345710(1u, 1u, 0x3BEu, 0x00345742u);
    gb_345710(7u, 2u, 0x3BFu, 0x00345750u);
    gb_345710(13u, 3u, 0x3C1u, 0x0034575Eu);
    gb_345710(4u, 4u, 0x3C3u, 0x0034576Cu);
    gb_345710(0xFFFFFFFFu, 0u, 0x3C2u, 0x00345734u); /* 4294967295 = 5 * 858993459 + 0 */
    gb_345710(0xFFFFFFFEu, 4u, 0x3C3u, 0x0034576Cu); /* 4294967294 = 5 * 858993458 + 4 */
    gb_345710(0x80000002u, 0u, 0x3C2u, 0x00345734u); /* 2147483650 = 5 * 429496730 */
    gb_345710(0x80000003u, 1u, 0x3BEu, 0x00345742u);
    gb_345710(0x80000004u, 2u, 0x3BFu, 0x00345750u);
    gb_345710(0x80000005u, 3u, 0x3C1u, 0x0034575Eu);
    gb_345710(0x80000006u, 4u, 0x3C3u, 0x0034576Cu);
}
