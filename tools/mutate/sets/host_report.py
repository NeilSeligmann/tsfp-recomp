"""Mutations for `src/host/host_report.c`, the run report and the exit verdict.

WHY THIS SET EXISTS. These lines were in `src/host/main.c` until today, and
`tsfp_host` is not a ctest binary, so NOTHING here could be mutated -- the same gap
T9 closed for the option defaults, closed here for the report (T13). The report is
the deliverable of a bring-up run: it is where fabrication is announced, where a
missing boundary says MISSING, and where the exit status decides what counts as the
expected end of a run. Every mutation below is a plausible-looking change that would
fail no build and no other test while making a run read healthier than it was.
"""

MUTATIONS: list[dict] = [
    {
        "id": "hostrep-real-stop-cannot-displace-an-orderly-exit",
        "file": "src/host/host_report.c",
        "old": "    if (slot == capacity && stop->reason != HOST_STOP_THREAD_EXITED) {",
        "new": "    if (0) {",
        "targets": ["test_host_report"],
        "why": "T1492 REGRESSION: the level 2 run had 15 guest threads and a table of 8 filled "
        "by orderly PsTerminateSystemThread exits, so the main game thread's FINAL stop was "
        "dropped and the report said nothing about why the run ended.",
    },
    {
        "id": "hostrep-verdict-ignores-real-stops-behind-orderly-exits",
        "file": "src/host/host_report.c",
        "old": "        if (table[i].valid && table[i].stop.reason != HOST_STOP_THREAD_EXITED) {",
        "new": "        if (table[i].valid && 0) {",
        "targets": ["test_host_report"],
        "why": "T1492: a fault behind eight clean thread exits must be the run's verdict, not the "
        "first clean exit (which exits 0).",
    },
    {
        "id": "hostrep-main-entry-return-not-annotated",
        "file": "src/host/host_report.c",
        "old": 'strcmp(stop->detail, "entry point returned") == 0',
        "new": "0",
        "targets": ["test_host_report"],
        "why": "T1492: `stopped: guest entry point returned` is the EXPECTED half (the stub only "
        "spawns the main thread). Unannotated it read as the title having quit, which is how the "
        "level 2 report misled.",
    },
    {
        "id": "hostrep-fabricated-zeros-whispered",
        "file": "src/host/host_report.c",
        "old": "            \"FABRICATED zeros    %u -- the title's configuration "
        'from the first of "',
        "new": "            \"fabricated zeros    %u -- the title's configuration "
        'from the first of "',
        "targets": ["test_host_report"],
        "why": "the capitalised FABRICATED is the report's one-word honesty contract: "
        "it is what a reader greps for to see how far our zeros reach into the "
        "title's configuration. Lower-cased, the annotation still parses as prose "
        "and stops standing out, which is the first step to it being skipped.",
    },
    {
        "id": "hostrep-failed-open-reads-as-ok",
        "file": "src/host/host_report.c",
        "old": '                attempt->opened ? "ok" : "ERR", (unsigned)attempt->attempts,',
        "new": '                attempt->opened ? "ok" : "ok ", (unsigned)attempt->attempts,',
        "targets": ["test_host_report"],
        "why": "the ok/ERR column is how an operator tells which volumes a later task "
        "has to provide. With every row reading ok, a run that failed all its opens "
        "looks like one that found its disc.",
    },
    {
        "id": "hostrep-null-icall-target-suppressed-again",
        "file": "src/host/host_report.c",
        "old": '                stop->guest_address == 0u ? "  (a NULL function pointer)" : "");',
        "new": '                stop->guest_address != 0u ? "  (a NULL function pointer)" : "");',
        "targets": ["test_host_report"],
        "why": "REGRESSION TO A FIXED BUG: target 0 is the single most common garbage "
        "icall target, and the old `!= 0` guard printed nothing for exactly that "
        'case -- "we do not know where" when 0 WAS the answer.',
    },
    {
        "id": "hostrep-trace-elision-silently-truncates",
        "file": "src/host/host_report.c",
        "old": "    size_t shown = count < (size_t)limit ? count : (size_t)limit;",
        "new": "    size_t shown = count;",
        "targets": ["test_host_report"],
        "why": "without the limit the elision line never prints, so --trace N stops "
        "meaning anything and a truncated capture cannot be told from a complete one.",
    },
    {
        "id": "hostrep-hung-thread-does-not-decide-the-verdict",
        "file": "src/host/host_report.c",
        "old": "    if (still_running > 0u) {\n        verdict = HOST_STOP_THREAD_TIMEOUT;\n    }",
        "new": "    if (still_running > 1u) {\n        verdict = HOST_STOP_THREAD_TIMEOUT;\n    }",
        "targets": ["test_host_report"],
        "why": "one hung guest thread is the common case, and with this off-by-one its "
        "run inherits whatever verdict a finished thread published -- a hang "
        "reported as an orderly exit, which is the one thing the watchdog exists "
        "to prevent.",
    },
    {
        "id": "hostrep-a-fault-exits-zero",
        "file": "src/host/host_report.c",
        "old": "            (verdict == HOST_STOP_KERNEL_UNIMPLEMENTED ||",
        "new": "            (verdict == HOST_STOP_KERNEL_UNIMPLEMENTED ||\n"
        "             verdict == HOST_STOP_FAULT ||",
        "targets": ["test_host_report"],
        "why": "widening the expected set is the quietest way to make CI green: a "
        "faulting run exits 0 and every caller that trusts the status stops "
        "noticing faults.",
    },
    {
        "id": "hostrep-stop-record-never-becomes-visible",
        "file": "src/host/host_report.c",
        "old": "    table[slot].valid = true;",
        "new": "    /* Mutation: leave the stop record unpublished. */",
        "targets": ["test_host_report"],
        "why": "an unpublished record makes the thread vanish from the report AND "
        "flips the verdict back to the main thread's uninteresting 'entry point "
        "returned' -- the run looks like it never started a thread at all.",
    },
]
