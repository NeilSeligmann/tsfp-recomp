# ruff: noqa: E501  (the anchors are verbatim source lines)
"""Mutations for the event driven route (T1633): the event grammar, the route player's event waits, the host observers,
the record's `# wait:` / `# mark-info:` lines, the option rules, the file observer and the Python mirror.

Kill suites: `test_route_events` (grammar, route player with a fake clock and the real probe, probe, record lines, options,
identity), `test_kernel_save_over` (the real NtCreateFile / NtReadFile / NtWriteFile feeding the observer) and the pytest
files `tests/test_t1633_*.py` (the differential grammar test against the C parser, the proposer, the launcher checks).

Not mutated, justified: the cooperative safepoint hook in `src/host/main.c` and the present observer glue (`tsfp_host` is not a
ctest binary, they were exercised by real headless runs, docs/input-replay.md "Event driven route"), the sampler thread loop
(`mem_main`, the pass it runs is `route_probe_mem_sample` which is mutated), and the array bound guards of `add_cond` and
`parse_waits` (a mutant overruns a fixed array, a crash is not a verdict).

Every `old` here occurs exactly once (`tests/test_mutation_anchors.py`).
"""

ROUTE = "src/input/xinput_route.c"
SPEC = "src/input/xinput_route_spec.c"
PROBE = "src/host/route_probe.c"
HOSTROUTE = "src/host/host_route.c"
RECORD = "src/input/xinput_record.c"
OPTIONS = "src/host/host_options.c"
KFILE = "src/xbox/kernel_file.c"
WAIT_PY = "tools/play/route_wait.py"
EVENTS_PY = "tools/route_events.py"
CLI_PY = "tools/play/cli.py"
SUITE = ["test_route_events"]
PY_WAITS = ["pytest:tests/test_t1633_route_waits.py"]
PY_EVENTS = ["pytest:tests/test_t1633_route_events.py"]
PY_PLAY = ["pytest:tests/test_t1633_play_waits.py"]


def _m(
    mutation_id: str, file: str, old: str, new: str, why: str, targets: list[str] | None = None
) -> dict:
    return {
        "id": f"route-events-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets or SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ---- the route player ----
    _m(
        "any-is-all",
        ROUTE,
        "if (wait->any && holds) return true;",
        "if (holds) return true;",
        "AND semantics turn into OR: a wait with several conditions holds on the first one",
    ),
    _m(
        "any-is-and",
        ROUTE,
        "if (!wait->any && !holds) return false;",
        "if (!holds) return false;",
        "`any` would need every condition: the alternative never helps",
    ),
    _m(
        "no-segment-baseline",
        ROUTE,
        "if (rt->watch[c] >= 0 && route->hooks.probe.watch_state != NULL)",
        "if (rt->watch[c] >= 0 && route->hooks.probe.watch_state != NULL && false)",
        "counters would be measured from process start: an open before the first pad poll satisfies mark 1",
    ),
    _m(
        "no-rearm-at-mark",
        ROUTE,
        "arm_for_mark(route, number + 1u); /* the next segment begins at this mark */",
        "(void)number; /* mutant: no re-arm */",
        "mark 2 would count the activity of segment 1",
    ),
    _m(
        "timeout-ms-ignored",
        ROUTE,
        "(wait->timeout_ms != 0u && spent >= wait->timeout_ms)",
        "(wait->timeout_ms != 0u && spent >= wait->timeout_ms && false)",
        "a wall clock timeout never fails the route",
    ),
    _m(
        "max-polls-ignored",
        ROUTE,
        "(wait->max_polls != 0u && route->stalled >= wait->max_polls)",
        "(wait->max_polls != 0u && route->stalled >= wait->max_polls && false)",
        "an event wait ignores its poll ceiling",
    ),
    _m(
        "min-ms-ignored",
        ROUTE,
        "if (wait->min_ms != 0u && now_ms(route) - start_ms < wait->min_ms) return false;",
        "if (wait->min_ms != 0u && now_ms(route) - start_ms < wait->min_ms && false) return false;",
        "min-ms does not gate a condition that already holds",
    ),
    _m(
        "progress-every-poll",
        ROUTE,
        "if (wait->event && now_ms(route) - route->last_progress_ms >= XINPUT_ROUTE_PROGRESS_MS) {",
        "if (wait->event && now_ms(route) - route->last_progress_ms >= 0u) {",
        "a progress line on every stalled poll",
    ),
    _m(
        "idle-without-io",
        ROUTE,
        "holds = count != 0u && quiet >= cond->amount;",
        "holds = quiet >= cond->amount;",
        "file-idle holds before any I/O happened: the quiet before the load counts as the load finishing",
    ),
    _m(
        "no-frame-baseline",
        ROUTE,
        "if (route->hooks.probe.frame_state != NULL) route->hooks.probe.frame_state(&rt->base_frame, route->hooks.probe.user);",
        "if (route->hooks.probe.frame_state != NULL && false) route->hooks.probe.frame_state(&rt->base_frame, route->hooks.probe.user);",
        "a frame change before the segment counts as a change in it (EQUIVALENT mutant not used: dropping the changes != 0 test of "
        "frame-stable cannot change a result because the quiet time is already 0 without a change)",
    ),
    _m(
        "call-count-ignored",
        ROUTE,
        "case RCOND_CALL: holds = count >= cond->amount;",
        "case RCOND_CALL: holds = count >= 1u;",
        "call=VA@N holds at the first call",
    ),
    _m(
        "read-bytes-ignored",
        ROUTE,
        "holds = bytes >= cond->amount;",
        "holds = count >= 1u;",
        "file-read@BYTES holds at the first read of any size",
    ),
    _m(
        "null-pointer-followed",
        SPEC,
        "if (!read(cond->address, 4u, &pointer, user) || pointer == 0u) return false;",
        "if (!read(cond->address, 4u, &pointer, user)) return false;",
        "a null pointer is followed to low memory",
    ),
    _m(
        "ne-is-eq",
        SPEC,
        "case RCMP_NE: return left != right;",
        "case RCMP_NE: return left == right;",
        "!= compares equal",
    ),
    _m(
        "ge-is-gt",
        SPEC,
        "case RCMP_GE: return left >= right;",
        "case RCMP_GE: return left > right;",
        ">= excludes the bound",
    ),
    _m(
        "mask-ignored",
        ROUTE,
        "holds = compare(cond->cmp, value & cond->mask, cond->value);",
        "holds = compare(cond->cmp, value, cond->value);",
        "the mask and the width are not applied to the value read",
    ),
    # ---- the grammar ----
    _m(
        "default-timeout",
        SPEC,
        "if (out->timeout_ms == 0u && out->max_polls == 0u) out->timeout_ms = XINPUT_ROUTE_DEFAULT_TIMEOUT_MS;",
        "if (out->timeout_ms == 0u && out->max_polls == 0u) out->timeout_ms = 0u;",
        "an event wait without a timeout could wait forever",
    ),
    _m(
        "no-lowercase",
        SPEC,
        "out[i] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);",
        "out[i] = (char)c;",
        "substrings are matched case sensitively: AniceMap.MKR never matches anicemap.mkr",
    ),
    _m(
        "any-needs-two",
        SPEC,
        "if (out->any && out->cond_count < 2u) {",
        "if (out->any && out->cond_count < 2u && false) {",
        "`any` with one condition is accepted",
    ),
    _m(
        "min-ms-above-timeout",
        SPEC,
        "if (out->timeout_ms != 0u && out->min_ms > out->timeout_ms) {",
        "if (out->timeout_ms != 0u && out->min_ms > out->timeout_ms && false) {",
        "a min-ms the timeout can never reach is accepted",
    ),
    _m(
        "indirect-alignment",
        SPEC,
        "if (address % 4u != 0u || address + 4u > 0x04000000ull) return false; /* the pointer dword */",
        "if (address + 4u > 0x04000000ull) return false; /* the pointer dword */",
        "an unaligned pointer dword is accepted",
    ),
    _m(
        "mask-hides-value",
        SPEC,
        "if ((cmp == RCMP_EQ) && (value & ~mask) != 0u) return false; /* a value the mask hides can never match */",
        "if (false) return false;",
        "a value the mask hides is accepted",
    ),
    # ---- the observers ----
    _m(
        "probe-negation",
        PROBE,
        "if (negated ? found : !found) continue;",
        "if (!found) continue;",
        "`!SUBSTR` selects the files that contain it",
    ),
    _m(
        "probe-open-counts-reads",
        PROBE,
        "if (w->kind == RCOND_FILE_OPEN && kind != KERNEL_FILE_EVENT_OPEN) continue;",
        "if (w->kind == RCOND_FILE_OPEN && kind != KERNEL_FILE_EVENT_OPEN && false) continue;",
        "file-open counts reads and writes",
    ),
    _m(
        "probe-read-counts-writes",
        PROBE,
        "if (w->kind == RCOND_FILE_READ && kind != KERNEL_FILE_EVENT_READ) continue;",
        "if (w->kind == RCOND_FILE_READ && kind != KERNEL_FILE_EVENT_READ && false) continue;",
        "file-read counts opens and writes",
    ),
    _m(
        "probe-facts-not-reset",
        PROBE,
        "    memset(&seg, 0, sizeof seg);\n    seg.start_ms = now;",
        "    seg.start_ms = now;",
        "the facts of a mark include everything since the run started",
    ),
    _m(
        "probe-log-not-throttled",
        PROBE,
        "slot->log_next_ms = now + 250u;\n            }\n        }\n    } else {",
        "slot->log_next_ms = now;\n            }\n        }\n    } else {",
        "every read is logged",
    ),
    _m(
        "probe-frame-dedupe",
        PROBE,
        "if (atomic_exchange(&frame_last_fp, fingerprint) == fingerprint && atomic_load(&frame_last_change_ms) != 0u) return;",
        "if (atomic_exchange(&frame_last_fp, fingerprint) == fingerprint && atomic_load(&frame_last_change_ms) != 0u && false) return;",
        "an identical frame signature counts as a change",
    ),
    _m(
        "probe-mem-always-logs",
        PROBE,
        "if (!entry->valid || value != entry->last) {",
        "if (true) {",
        "an unchanged value is logged on every sample",
    ),
    _m(
        "probe-watch-dedupe",
        PROBE,
        'if (watches[i].kind == kind && watches[i].va == va && strcmp(watches[i].text, text != NULL ? text : "") == 0) {',
        "if (watches[i].kind == kind && watches[i].va == va && false) {",
        "two equal conditions get two watches",
    ),
    # ---- the record, host_route, options, identity ----
    _m(
        "record-mark-facts",
        RECORD,
        "if (mark_facts_fn != NULL) { /* T1633:",
        "if (mark_facts_fn != NULL && false) { /* T1633:",
        "the recorder never writes the mark-info line",
    ),
    _m(
        "route-no-override",
        HOSTROUTE,
        "for (size_t j = 0u; j < command_line_waits; j++) overridden = overridden || waits[j].mark == wait.mark;",
        "for (size_t j = 0u; j < command_line_waits; j++) overridden = overridden && waits[j].mark == wait.mark;",
        "the record's wait is not replaced by the command line's",
    ),
    _m(
        "route-record-wait-line",
        HOSTROUTE,
        "snprintf(error, error_size, \"record '# wait:' line %zu: %s\", i + 1u, parse_error);",
        'snprintf(error, error_size, "record wait %zu: %s", i + 1u, parse_error);',
        "a bad record wait is not located",
    ),
    _m(
        "options-mem-needs-log",
        OPTIONS,
        "(out->route_log_mem_count == 0u || out->route_event_log != NULL) &&",
        "(true) &&",
        "--route-log-mem without its log is accepted",
    ),
    _m(
        "options-wait-needs-replay",
        OPTIONS,
        "out->route_event_wait_count == 0u)) &&",
        "true)) &&",
        "--route-wait-event without --replay-input is accepted",
    ),
    _m(
        "identity-log-mem",
        RECORD,
        '{"--route-event-log", 1}, {"--route-log-mem", 1},',
        '{"--route-event-log", 1},',
        "--route-log-mem becomes a game flag: a route recorded without it is refused",
    ),
    # ---- the kernel file observer ----
    _m(
        "kfile-open-event",
        KFILE,
        "    if (file_observer != NULL) {\n        file_observer(KERNEL_FILE_EVENT_OPEN, slot->state.path, 0u);\n    }",
        "    if (file_observer != NULL && false) {\n        file_observer(KERNEL_FILE_EVENT_OPEN, slot->state.path, 0u);\n    }",
        "opens are not reported",
        ["test_kernel_save_over"],
    ),
    _m(
        "kfile-write-event",
        KFILE,
        "observe_handle(KERNEL_FILE_EVENT_WRITE, handle, *out_written);",
        "observe_handle(KERNEL_FILE_EVENT_READ, handle, *out_written);",
        "a write is reported as a read",
        ["test_kernel_save_over"],
    ),
    _m(
        "kfile-read-event",
        KFILE,
        "const bool ok = read_backing(handle, offset, buffer, length, out_read, false);\n    if (ok && out_read != NULL) {",
        "const bool ok = read_backing(handle, offset, buffer, length, out_read, false);\n    if (ok && out_read != NULL && false) {",
        "reads are not reported",
        ["test_kernel_save_over"],
    ),
]


def _p(mutation_id: str, file: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"route-events-py-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _p(
        "any-needs-two",
        WAIT_PY,
        "if wait.any and len(wait.conds) < 2:",
        "if wait.any and len(wait.conds) < 2 and False:",
        "the Python mirror accepts `any` with one condition, the C parser does not",
        PY_WAITS,
    ),
    _p(
        "default-timeout",
        WAIT_PY,
        "wait.timeout_ms = DEFAULT_TIMEOUT_MS",
        "wait.timeout_ms = 0",
        "the mirror's default timeout differs from the C parser's",
        PY_WAITS,
    ),
    _p(
        "no-lowercase",
        WAIT_PY,
        'out.append(char.lower() if "A" <= char <= "Z" else char)',
        "out.append(char)",
        "the mirror keeps the case of a substring",
        PY_WAITS,
    ),
    _p(
        "mask-hides-value",
        WAIT_PY,
        'if operator == "==" and (value & ~mask & 0xFFFFFFFF) != 0:',
        'if operator == "==" and False:',
        "the mirror accepts a value the mask hides",
        PY_WAITS,
    ),
    _p(
        "call-needs-safepoint",
        WAIT_PY,
        '            and "--headless-second-vblank" not in host_flags\n            and "--headless-first-vblank" not in host_flags\n',
        "            and False\n",
        "a call= wait is accepted although no cooperative safepoint runs",
        PY_WAITS,
    ),
    _p(
        "command-replaces-record",
        WAIT_PY,
        "if wait.mark not in command_marks:",
        "if True:",
        "the record's wait is validated as if the command line did not replace it",
        PY_WAITS,
    ),
    _p(
        "poke-mark-check",
        WAIT_PY,
        "if match and not 1 <= int(match.group(1)) <= len(record.marks):",
        "if False:",
        "a poke trigger on a mark the record lacks passes the pre-launch check",
        PY_WAITS + PY_PLAY,
    ),
    _p(
        "idle-closeness",
        EVENTS_PY,
        "if idle_ms is not None and idle_ms >= MIN_IDLE_MS * 2:",
        "if idle_ms is not None:",
        "an idle condition is proposed although the mark followed the last I/O closely",
        PY_EVENTS,
    ),
    _p(
        "refused-is-touch",
        EVENTS_PY,
        '            if event.kind not in (\n                "refused",\n                "dir",\n            ):  # a refused create or a listing did not touch the file\n',
        "            if True:\n",
        "a refused create counts as touching the file, so the real open in the next segment is not proposed",
        PY_EVENTS,
    ),
    _p(
        "backup",
        EVENTS_PY,
        'shutil.copyfile(record, record.with_name(record.name + ".t1633.bak"))',
        "pass",
        "--apply rewrites the record without keeping a copy",
        PY_EVENTS,
    ),
    _p(
        "insert-position",
        EVENTS_PY,
        "position += 1\n",
        "position += 0\n",
        "the wait lands before the mark-info line instead of after it",
        PY_EVENTS,
    ),
    _p(
        "volume-filter",
        EVENTS_PY,
        'return f"@{match.group(1).lower()}:\\\\"',
        'return ""',
        "the proposal has no save-volume filter and never holds while the disc streams",
        PY_EVENTS,
    ),
    _p(
        "wait-check-called",
        CLI_PY,
        "    return wait_refusal(args, command)",
        "    return None",
        "tools.play never validates the waits before launching",
        PY_PLAY,
    ),
    _p(
        "event-flag-passed",
        CLI_PY,
        '            extra += ["--route-wait-event", spec]',
        "            pass",
        "--route-wait-event is accepted and silently dropped",
        PY_PLAY,
    ),
    _p(
        "identity-event-flag",
        "tools/play/route_check.py",
        '    "--route-wait-event": 1,\n',
        "",
        "--route-wait-event becomes a game flag in the Python identity",
        PY_PLAY + ["pytest:tests/test_t1618_route_flags.py"],
    ),
]
