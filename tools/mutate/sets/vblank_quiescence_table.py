"""Mutations for the default non-reader table of the quiescence predicate (T424, T593).

`src/host/recomp_vblank_quiescence.{c,h}`. The table says which thread start routines the predicate
may skip because the entry barrier proof (`python -m tools.tracegaps threadreach --start ...`) shows
they cannot sample the vblank counter. A wrong entry skips a thread that can read the counter, a
missing one refuses every delivery while the loader or the network poller runs, and a table that
only skips a thread in SOME states re-opens the "is a host timer sleeper parked" question T593 closed
(a proven non-reader is skipped in every state, an unproven sleeper is refused).
"""

# The anchors are exact source lines, some longer than the line limit.
# ruff: noqa: E501
MUTATIONS: list[dict] = [
    {
        "id": "vblank-quiescence-table-drops-loader",
        "file": "src/host/recomp_vblank_quiescence.c",
        "old": "static const uint32_t default_non_reader_starts[] = {RVQ_NON_READER_NET_POLL_START,\n                                                     RVQ_NON_READER_LOADER_START};",
        "new": "static const uint32_t default_non_reader_starts[] = {RVQ_NON_READER_NET_POLL_START};",
        "targets": ["test_recomp_vblank_quiescence"],
        "why": "the loader thread 0x30160 is refused again, every delivery stops while it sleeps 16 ms.",
    },
    {
        "id": "vblank-quiescence-table-drops-net-poll",
        "file": "src/host/recomp_vblank_quiescence.c",
        "old": "static const uint32_t default_non_reader_starts[] = {RVQ_NON_READER_NET_POLL_START,\n                                                     RVQ_NON_READER_LOADER_START};",
        "new": "static const uint32_t default_non_reader_starts[] = {RVQ_NON_READER_LOADER_START};",
        "targets": ["test_recomp_vblank_quiescence"],
        "why": "adding the loader replaced the T424 entry instead of extending the table.",
    },
    {
        "id": "vblank-quiescence-loader-start-off-by-one",
        "file": "src/host/recomp_vblank_quiescence.h",
        "old": "#define RVQ_NON_READER_LOADER_START 0x30160u",
        "new": "#define RVQ_NON_READER_LOADER_START 0x30161u",
        "targets": ["test_recomp_vblank_quiescence"],
        "why": "the table names a start routine nothing runs at, so the proven thread is refused and a stranger is skipped.",
    },
    {
        "id": "vblank-quiescence-loader-start-is-a-reader",
        "file": "src/host/recomp_vblank_quiescence.h",
        "old": "#define RVQ_NON_READER_LOADER_START 0x30160u",
        "new": "#define RVQ_NON_READER_LOADER_START 0x156CB0u",
        "targets": ["test_recomp_vblank_quiescence"],
        "why": "the loading bar worker, a counter reader, would be skipped by the predicate.",
    },
    {
        "id": "vblank-quiescence-non-reader-still-examined",
        "file": "src/host/recomp_vblank_quiescence.c",
        "old": "            is_non_reader(&records[i], non_reader_starts, non_reader_count)) {\n            continue;\n        }\n        verdict.examined++;",
        "new": "            (is_non_reader(&records[i], non_reader_starts, non_reader_count) && false)) {\n            continue;\n        }\n        verdict.examined++;",
        "targets": ["test_recomp_vblank_quiescence"],
        "why": "a proven non-reader sleeping to a host deadline (the 16 ms loader wait) or host stopped refuses the delivery again.",
    },
    {
        "id": "vblank-quiescence-host-timer-counts-as-parked",
        "file": "src/host/recomp_vblank_quiescence.c",
        "old": "            bool now = state == RVQ_UNSTARTED;",
        "new": "            bool now = state == RVQ_UNSTARTED || state == RVQ_HOST_TIMER;",
        "targets": ["test_recomp_vblank_quiescence"],
        "why": "an UNPROVEN thread that wakes at a host deadline is called parked and can sample the counter mid delivery.",
    },
]
