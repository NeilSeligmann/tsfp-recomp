/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SYNTHETIC measured-arity table for tests/c/test_kernel_thunk_arity.c, standing in for the
 * generated generated/lifted/gen/kernel_arity.inc so the quorum, unanimity and tier-order
 * logic in src/host/kernel_thunk.c is tested deterministically and in a fresh clone. The real
 * table moves every time the lifter reruns, so a test pinned to its rows would drift.
 *
 * Row layout: { ordinal, stack_args, sites, unanimous }. Each row's job is in the suite.
 * None of these ordinals may be an ABI_TABLE hand row except 219, which exists to prove the
 * hand tier wins. The suite asserts that at start, so a hand row added later fails loudly.
 */
typedef struct {
    unsigned ordinal;
    unsigned stack_args;
    unsigned sites;
    unsigned char unanimous;
} measured_arity;

static const measured_arity MEASURED_ARITIES[] = {
    {219u, 6u, 11u, 1},  /* hand row says 8: the hand tier must win over a quorate row */
    {12u, 5u, 3u, 1},    /* exactly the quorum: believed. Oracle says 1 */
    {14u, 4u, 2u, 1},    /* one short of the quorum: refused, oracle answers 1 */
    {300u, 7u, 9u, 0},   /* many voters but not unanimous: refused, oracle answers 3 */
    {350u, 6u, 3u, 0},   /* quorum but not unanimous: refused, oracle answers 2 */
    {100u, 9u, 4u, 1},   /* above the quorum: believed. Oracle says 1 */
    {60u, 6u, 3u, 1},    /* agrees with the oracle: only the reported source tells them apart */
    {16u, 3u, 1u, 1},    /* a DATA export with one voter: refused AND the oracle refuses */
};
#define MEASURED_ARITY_COUNT (sizeof(MEASURED_ARITIES) / sizeof(MEASURED_ARITIES[0]))
static const unsigned MEASURED_DATA_ORDINALS[] = {16u, 30u};
#define MEASURED_DATA_ORDINAL_COUNT (sizeof(MEASURED_DATA_ORDINALS) / sizeof(MEASURED_DATA_ORDINALS[0]))
