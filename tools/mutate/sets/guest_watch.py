# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the guest write watch (T1741): the pure filter `src/host/guest_watch_filter.c` and the option parsing and
help in `host_options.c`. Kill suite: ctest `test_guest_watch` (plain build, no XBE, no GPU).

Not mutated, by design: `guest_watch.c` itself (signal handlers, page protection and the poller run only inside `tsfp_host`,
which is not a ctest binary, so a mutant there could not be killed by any suite here, see docs/t-ai-enemy-encounter.md).
"""

FILTER = "src/host/guest_watch_filter.c"
OPTIONS_C = "src/host/host_options.c"
SUITE = ["test_guest_watch"]


def _m(mutation_id: str, file: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"gw-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _m(
        "length-limit",
        FILTER,
        "set->ranges[index].length > GUEST_WATCH_MAX_LENGTH",
        "set->ranges[index].length > GUEST_WATCH_MAX_LENGTH + 1u",
        "a 65 byte range must be refused, the record buffers hold 64.",
    ),
    _m(
        "count-limit",
        FILTER,
        "set->count > GUEST_WATCH_MAX_RANGES",
        "set->count > GUEST_WATCH_MAX_RANGES + 1u",
        "a fifth range must be refused, the slot tables hold four.",
    ),
    _m(
        "changed-is-hit",
        FILTER,
        "    if (changed) {\n        return true;\n    }",
        "    if (changed && false) {\n        return true;\n    }",
        "a store that changed the bytes must be logged even when the fault address lies past the range (straddling store).",
    ),
    _m(
        "range-start",
        FILTER,
        "return fault >= low && (uint64_t)fault < (uint64_t)low + length;",
        "return fault + 1u >= low && (uint64_t)fault < (uint64_t)low + length;",
        "a neighbour store just below the range that left it unchanged is not a hit.",
    ),
    _m(
        "range-end",
        FILTER,
        "(uint64_t)fault < (uint64_t)low + length",
        "(uint64_t)fault <= (uint64_t)low + length",
        "the byte just past the range is not part of it.",
    ),
    _m(
        "opt-help-marker",
        OPTIONS_C,
        '"  --watch-write SPEC     T1741, PASSIVE write watch:',
        '"  --watch-write SPEC     PASSIVE write watch:',
        "the help carries the T1741 marker.",
    ),
    _m(
        "opt-max-lower-bound",
        OPTIONS_C,
        "!parse_bounded(argv[i], 10, 1u, GUEST_WATCH_HARD_RECORDS, &value)",
        "!parse_bounded(argv[i], 10, 0u, GUEST_WATCH_HARD_RECORDS, &value)",
        "--watch-write-max 0 must be refused, a ring of no records logs nothing silently.",
    ),
    _m(
        "opt-range-check",
        OPTIONS_C,
        "!guest_watch_check(&out->watch_write_set, NULL, 0u)",
        "false",
        "an oversize watch range must be refused at parse time (fail closed).",
    ),
]
