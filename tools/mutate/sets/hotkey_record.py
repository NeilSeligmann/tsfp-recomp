# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the pad-only recording hotkeys (T1632): the lead rule and the leaked-key observer of the host hotkeys, the
pending tail / trim / mark request of the recorder, the host actions, and the stop request of the window sink.

Kill suites: ctest `test_xinput_hotkey` (pure C), `test_xinput_record` (recorder, replay load), `test_hotkey_actions` (fake hooks and
the real recorder), `test_pad_sdl_feed` (SDL dummy video driver: real events, real feed, real source, real recorder, a replay of
the trimmed route), `test_host_options`. `test_pad_sdl_feed` needs an SDL3 build, without it it skips (77) and kills nothing.

Justified equivalent, not mutated: `item->start + item->frames <= from_poll` as `<` in `xinput_record_suppress_begin` (a run that ends
exactly where the cut starts is split into a zero frame piece with the same state, and the merge pass that follows folds it back).
Not mutated: `src/host/main.c` (the wiring of `hotkey_mark_hook` and `hotkey_stop_hook` is covered only by the container compile),
the stderr texts.

Every `old` here occurs exactly once (`tests/test_mutation_anchors.py`).
"""

HOTKEY = "src/input/xinput_hotkey.c"
HOTKEY_H = "src/input/xinput_hotkey.h"
RECORD = "src/input/xinput_record.c"
ACTIONS = "src/host/hotkey_actions.c"
SOURCE = "src/input/xinput_host_source.c"
SINK = "src/host/present_sink.c"
OPTIONS = "src/host/host_options.c"
HOTKEY_SUITE = ["test_xinput_hotkey"]
RECORD_SUITE = ["test_xinput_record", "test_hotkey_actions", "test_pad_sdl_feed"]
ACTION_SUITE = ["test_hotkey_actions", "test_pad_sdl_feed"]


def _m(
    mutation_id: str,
    file: str,
    old: str,
    new: str,
    why: str,
    suite: list[str] | None = None,
) -> dict:
    return {
        "id": f"hotkey-rec-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(suite or HOTKEY_SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _m(
        "lead-rule-any-two",
        HOTKEY,
        "if (element != 0u && entry->held[0] != 0u) chord = true;",
        "if (held_count(entry) >= 2u) chord = true;",
        "the T1627 rule: any two chord keys swallow the next one, LB+X played normally loses X",
    ),
    _m(
        "lead-rule-swallows-lead",
        HOTKEY,
        "if (element != 0u && entry->held[0] != 0u) chord = true;",
        "if (entry->held[0] != 0u) chord = true;",
        "the lead itself is swallowed when another chord key was pressed first",
    ),
    _m(
        "leak-begin-not-reported",
        HOTKEY,
        "if (hotkeys->leak_fn != NULL) hotkeys->leak_fn(episode->device, episode->name, episode->down_poll, true, hotkeys->leak_user);",
        "(void)0;",
        "the recorder is never told a forwarded chord key belongs to a fired chord",
    ),
    _m(
        "leak-begin-twice",
        HOTKEY,
        "if (episode->tainted || episode->device != entry->spec.device || !locate(entry, episode->name, &element, &alias)) continue;",
        "if (episode->device != entry->spec.device || !locate(entry, episode->name, &element, &alias)) continue;",
        "the same key episode is reported again at every fire of the chord",
    ),
    _m(
        "leak-end-not-reported",
        HOTKEY,
        "if (tainted && hotkeys->leak_fn != NULL) hotkeys->leak_fn(device, name, poll, false, hotkeys->leak_user);",
        "(void)tainted;",
        "the suppression never ends, a later genuine press of the same key is cut from the route",
    ),
    _m(
        "leak-end-untainted",
        HOTKEY,
        "if (tainted && hotkeys->leak_fn != NULL) hotkeys->leak_fn(device, name, poll, false, hotkeys->leak_user);",
        "if ((tainted || leak >= 0) && hotkeys->leak_fn != NULL) hotkeys->leak_fn(device, name, poll, false, hotkeys->leak_user);",
        "a genuine press of a chord member is reported as a leak",
    ),
    _m(
        "leak-episode-never-created",
        HOTKEY,
        "if (forward && !already && is_chord_member(hotkeys, device, name) && leak_find(hotkeys, device, name) < 0) {",
        "if (false && forward && !already && is_chord_member(hotkeys, device, name) && leak_find(hotkeys, device, name) < 0) {",
        "no forwarded chord key is remembered",
    ),
    _m(
        "leak-episode-for-swallowed",
        HOTKEY,
        "if (forward && !already && is_chord_member(hotkeys, device, name) && leak_find(hotkeys, device, name) < 0) {",
        "if (!already && is_chord_member(hotkeys, device, name) && leak_find(hotkeys, device, name) < 0) {",
        "a swallowed key (never seen by the game) is remembered as a leak",
    ),
    _m(
        "leak-down-poll",
        HOTKEY,
        "episode->down_poll = poll;",
        "episode->down_poll = poll + 1u;",
        "the span to cut starts one poll late",
    ),
    _m(
        "action-stop-is-mark",
        HOTKEY,
        'if (names_equal(label, "stop")) return XINPUT_HOTKEY_ACTION_STOP;',
        'if (names_equal(label, "stop")) return XINPUT_HOTKEY_ACTION_MARK;',
        "the stop label places a mark",
    ),
    _m(
        "max-chords",
        HOTKEY_H,
        "#define XINPUT_HOTKEY_MAX 8u",
        "#define XINPUT_HOTKEY_MAX 4u",
        "the four pad and four keyboard default chords do not fit one run",
        suite=["test_xinput_hotkey", "test_host_options"],
    ),
    _m(
        "rec-no-rewrite",
        RECORD,
        "apply_effect(&item->state, effect);",
        "(void)item;",
        "suppress_begin leaves the history alone",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-no-merge",
        RECORD,
        "if (out != 0u && !pending[i].mark && !pending[out - 1u].mark &&",
        "if (false && out != 0u && !pending[i].mark && !pending[out - 1u].mark &&",
        "equal neighbouring runs stay split, the trimmed route is not the text of a clean one",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-future-unfiltered",
        RECORD,
        "for (unsigned i = 0u; i < suppressed_count; i++) apply_effect(&state, &suppressed[i]);",
        "(void)suppressed_count;",
        "the polls after the chord fired still carry the leaked key",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-end-keeps-filter",
        RECORD,
        "suppressed[i] = suppressed[--suppressed_count];",
        "(void)i;",
        "the filter outlives the key, every later press of it is cut",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-no-refcount",
        RECORD,
        "if (--suppressed_refs[i] != 0u) break;",
        "--suppressed_refs[i];",
        "two keys with the same effect: the first release ends the filter for both",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-mark-cap",
        RECORD,
        "if (marks_written >= XINPUT_ROUTE_MAX_MARKS) {",
        "if (false) {",
        "a 17th mark makes the route unreplayable",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-marks-per-recording",
        RECORD,
        "marks_written = 0u; marks_dropped = 0u;",
        "(void)marks_written;",
        "the mark counters carry over to the next recording of the process",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-no-clip",
        RECORD,
        "if (from_poll < record_written_polls) {",
        "if (false) {",
        "a trim older than the pending tail is neither clipped nor reported",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-no-lag",
        RECORD,
        "#define RECORD_TAIL_LAG 4096u",
        "#define RECORD_TAIL_LAG 0u",
        "history is written at once, nothing is left to trim",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-mark-without-recording",
        RECORD,
        "const bool recording = record_file != NULL && !record_failed;",
        "const bool recording = true;",
        "a mark is claimed to be queued with no recording open",
        suite=RECORD_SUITE,
    ),
    _m(
        "rec-close-drops-tail",
        RECORD,
        "drain_pending(pending_count);",
        "(void)pending_count;",
        "the pending tail is never written at close",
        suite=RECORD_SUITE,
    ),
    _m(
        "act-stop-not-requested",
        ACTIONS,
        'if (actions->hooks.stop != NULL) actions->hooks.stop(actions->hooks.user, "stop hotkey (T1632)");',
        "(void)0;",
        "the stop label does nothing",
        suite=ACTION_SUITE,
    ),
    _m(
        "act-mark-result-ignored",
        ACTIONS,
        "if (actions->hooks.mark != NULL && actions->hooks.mark(actions->hooks.user)) {",
        "if (actions->hooks.mark != NULL) {",
        "a mark with no recording open is counted as queued",
        suite=ACTION_SUITE,
    ),
    _m(
        "act-other-device-trimmed",
        ACTIONS,
        "if (key_device != actions->source_device || !xinput_host_key_effect(key_device, name, &digital, &analog, &stick)) {",
        "if (!xinput_host_key_effect(key_device, name, &digital, &analog, &stick)) {",
        "a keyboard key with a gamepad source (never seen by the game) is cut from the route",
        suite=ACTION_SUITE,
    ),
    _m(
        "act-leak-poll",
        ACTIONS,
        "xinput_record_suppress_begin(&effect, poll);",
        "xinput_record_suppress_begin(&effect, poll + 1u);",
        "the cut starts one poll late and leaves the first poll of the leaked key in the route",
        suite=ACTION_SUITE,
    ),
    _m(
        "effect-analog-lost",
        SOURCE,
        "case TARGET_ANALOG: *analog = (int)table[code].analog; return true;",
        "case TARGET_ANALOG: *analog = -1; return true;",
        "an analog button key (LB, X) has no effect, it is never cut",
        suite=ACTION_SUITE,
    ),
    _m(
        "effect-unmapped-accepted",
        SOURCE,
        "default: return false;\n    }\n}\n\nbool xinput_pad_source_kind_parse(",
        "default: return true;\n    }\n}\n\nbool xinput_pad_source_kind_parse(",
        "a key that maps to nothing (GUIDE, CTRL) is reported as trimmed",
        suite=ACTION_SUITE,
    ),
    _m(
        "sink-request-close-noop",
        SINK,
        "void present_video_sink_request_close(present_video_sink *sink, const char *cause)\n{\n    if (sink == NULL) return;",
        "void present_video_sink_request_close(present_video_sink *sink, const char *cause)\n{\n    (void)cause;\n    return;",
        "the stop hotkey cannot close the window sink",
        suite=["test_pad_sdl_feed"],
    ),
    _m(
        "help-marker",
        OPTIONS,
        '"  Host hotkeys (T1627, T1632, T1720, tooling only',
        '"  Host hotkeys (T1627, T1720, tooling only',
        "the T1632 marker the owner scripts grep for is missing from the host help",
        suite=["test_host_options"],
    ),
    _m(
        "help-marker-shot",
        OPTIONS,
        '"  Host hotkeys (T1627, T1632, T1720, tooling only',
        '"  Host hotkeys (T1627, T1632, tooling only',
        "the T1720 marker run_render_shots.fish greps for is missing from the host help",
        suite=["test_host_options"],
    ),
]
