# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the polygon offset and IGNORED counters of the swap replay (T511).

T511 plumbed `gpu_pgraph_report.offset_applied` / `offset_unobserved` and `gpu_pgraph_stats.pairs_ignored` into
`d3d8_swap_replay_stats` and its log (`src/gpu/d3d8_swap_replay.c`). Grouped by what a survivor would let through:

    ignored    `pairs_ignored`: read from the model before any present, folded over the present, kept over a
               `d3d8_gpu_reset` (the new model's counter starts at 0), taken from the right model counter
    pass       the per pass copy of the two report counters, and the fold of the passes into the totals
               (added, not overwritten, over every pass, applied and unobserved not swapped)
    log        the per pass line: only when there is something to say, both counts, in the right order, naming the pass's
               own target

Kills come from `test_d3d8_swap_replay`: the IGNORED counters and the fold run with no device, everything that is drawn
(applied, unobserved, the log, the passes) needs one. `_NEEDS_DEVICE` at the bottom records which are device only,
from a run with the Vulkan loader hidden.

NOT MUTATED, and why: `src/host/main.c` (the host's `polygon offset` line) and `tools/steady_replay.py` (its parser and the
table) because neither is a ctest binary the harness fingerprints. They are covered by `tests/test_steady_replay.py` (the fake
host, the parser, the table, an old host without the line) and were mutated by hand, see docs/tasks.md T511. EQUIVALENT: none
known.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_SWAP = "src/gpu/d3d8_swap_replay.c"
_TEST = ["test_d3d8_swap_replay"]


def _sw(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"gpu-offset-counters-{mutation_id}",
        "file": _SWAP,
        "old": old,
        "new": new,
        "targets": list(_TEST),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _sw(
        "ignored-live-read",
        "        stats.pairs_ignored += model.pairs_ignored - state.pairs_ignored_folded;",
        "        stats.pairs_ignored += 0u;",
        "get_stats does not add what the model counted since the last fold: the pairs decoded at a kick are invisible until the present.",
    ),
    _sw(
        "ignored-live-counter",
        "        stats.pairs_ignored += model.pairs_ignored - state.pairs_ignored_folded;",
        "        stats.pairs_ignored += model.pairs_handled - state.pairs_ignored_folded;",
        "the live read takes the handled pair counter for the ignored one: the count is of every handled pair.",
    ),
    _sw(
        "ignored-fold-delta",
        "    state.stats.pairs_ignored += model.pairs_ignored - state.pairs_ignored_folded;",
        "    state.stats.pairs_ignored += 0u;",
        "the fold adds nothing to the totals, so the pairs vanish from the stats the moment the fold records them as folded.",
    ),
    _sw(
        "ignored-fold-counter",
        "    state.stats.pairs_ignored += model.pairs_ignored - state.pairs_ignored_folded;",
        "    state.stats.pairs_ignored += model.pairs - state.pairs_ignored_folded;",
        "the fold takes the model's all-pairs counter for the ignored one.",
    ),
    _sw(
        "ignored-fold-mark",
        "    state.pairs_ignored_folded = model.pairs_ignored;",
        "    state.pairs_ignored_folded = 0u;",
        "the fold forgets what it folded: the next fold and the live read add the same pairs again.",
    ),
    _sw(
        "ignored-reset-mark",
        "    state.pairs_ignored_folded = 0u;",
        "    state.pairs_ignored_folded += 0u;",
        "a d3d8_gpu_reset keeps the old model's folded mark: the new model starts at 0, so its first pairs wrap below it.",
    ),
    _sw(
        "pass-applied-copy",
        "    pass->offset_applied = report.offset_applied;",
        "    pass->offset_applied = 0u;",
        "the pass does not keep the report's applied count: the stats never see a draw the offset touched.",
    ),
    _sw(
        "pass-unobserved-copy",
        "    pass->offset_unobserved = report.offset_unobserved;",
        "    pass->offset_unobserved = 0u;",
        "the pass does not keep the report's unobserved count: an offset nothing tests depth against is silent.",
    ),
    _sw(
        "pass-copies-swapped",
        "    pass->offset_applied = report.offset_applied;",
        "    pass->offset_applied = report.offset_unobserved;",
        "the pass takes the unobserved count as the applied one.",
    ),
    _sw(
        "total-applied-overwritten",
        "        state.stats.offset_applied += passes[i].offset_applied;",
        "        state.stats.offset_applied = passes[i].offset_applied;",
        "the applied total is the last pass of the last frame, not the sum over frames and passes.",
    ),
    _sw(
        "total-unobserved-overwritten",
        "        state.stats.offset_unobserved += passes[i].offset_unobserved;",
        "        state.stats.offset_unobserved = passes[i].offset_unobserved;",
        "the unobserved total is the last pass of the last frame, not the sum.",
    ),
    _sw(
        "total-applied-first-pass-only",
        "        state.stats.offset_applied += passes[i].offset_applied;",
        "        state.stats.offset_applied += i == 0u ? passes[i].offset_applied : 0u;",
        "only the first pass of a frame is counted: the draws into every other render target are lost.",
    ),
    _sw(
        "total-unobserved-first-pass-only",
        "        state.stats.offset_unobserved += passes[i].offset_unobserved;",
        "        state.stats.offset_unobserved += i == 0u ? passes[i].offset_unobserved : 0u;",
        "only the first pass of a frame is counted for the unobserved offset.",
    ),
    _sw(
        "total-applied-from-unobserved",
        "        state.stats.offset_applied += passes[i].offset_applied;",
        "        state.stats.offset_applied += passes[i].offset_unobserved;",
        "the applied total adds the unobserved count.",
    ),
    _sw(
        "total-unobserved-from-applied",
        "        state.stats.offset_unobserved += passes[i].offset_unobserved;",
        "        state.stats.offset_unobserved += passes[i].offset_applied;",
        "the unobserved total adds the applied count.",
    ),
    _sw(
        "log-needs-both",
        "        if (passes[i].offset_applied != 0u || passes[i].offset_unobserved != 0u) {",
        "        if (passes[i].offset_applied != 0u && passes[i].offset_unobserved != 0u) {",
        "the pass says something only when it has applied AND unobserved draws: an offset that did something stays silent.",
    ),
    _sw(
        "log-applied-only",
        "        if (passes[i].offset_applied != 0u || passes[i].offset_unobserved != 0u) {",
        "        if (passes[i].offset_applied != 0u) {",
        "an offset nothing tests depth against is not logged: the one case the counter exists to show is silent.",
    ),
    _sw(
        "log-unobserved-only",
        "        if (passes[i].offset_applied != 0u || passes[i].offset_unobserved != 0u) {",
        "        if (passes[i].offset_unobserved != 0u) {",
        "an applied offset is not logged.",
    ),
    _sw(
        "log-always",
        "        if (passes[i].offset_applied != 0u || passes[i].offset_unobserved != 0u) {",
        "        if (true) {",
        "every pass of every frame is logged, including the title's zero offset: the default log gets a line per pass.",
    ),
    _sw(
        "log-counts-swapped",
        "(unsigned)passes[i].offset_applied, (unsigned)passes[i].offset_unobserved);",
        "(unsigned)passes[i].offset_unobserved, (unsigned)passes[i].offset_applied);",
        "the log prints the unobserved count where it says applied.",
    ),
    _sw(
        "log-target",
        "(unsigned long long)frame_number, (unsigned)passes[i].target,",
        "(unsigned long long)frame_number, (unsigned)bound,",
        "every pass's line names the presented target, not the target the pass drew into.",
    ),
]

_NEEDS_DEVICE = frozenset(
    {
        "gpu-offset-counters-pass-applied-copy",
        "gpu-offset-counters-pass-unobserved-copy",
        "gpu-offset-counters-pass-copies-swapped",
        "gpu-offset-counters-total-applied-overwritten",
        "gpu-offset-counters-total-unobserved-overwritten",
        "gpu-offset-counters-total-applied-first-pass-only",
        "gpu-offset-counters-total-unobserved-first-pass-only",
        "gpu-offset-counters-total-applied-from-unobserved",
        "gpu-offset-counters-total-unobserved-from-applied",
        "gpu-offset-counters-log-needs-both",
        "gpu-offset-counters-log-applied-only",
        "gpu-offset-counters-log-unobserved-only",
        "gpu-offset-counters-log-always",
        "gpu-offset-counters-log-counts-swapped",
        "gpu-offset-counters-log-target",
    }
)
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
