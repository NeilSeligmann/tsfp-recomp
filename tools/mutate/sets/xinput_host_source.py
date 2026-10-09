# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the host keyboard and gamepad pad sources in `src/input/xinput_host_source.c` (T751).

Kill suite: ctest `test_xinput_host_source` (plain build, no XBE), `test_host_options` for the flags. Every `old`
occurs exactly once (`tests/test_mutation_anchors.py`).

Families: the mapping tables (every row is pinned by an independent expectation), stick cancel and
full deflection, trigger scaling and clamping, event timing and per-event raw reports (T731 packet
numbers), the explicit ignore paths, the fake-feed parser refusals, the missing-provider refusal.

Justified non-mutants (equivalent, not killed on purpose): the kind check of `xinput_pad_source_open` (T751 follow-up, it repeats the check of `xinput_pad_source_open_feed` it delegates to, mutated in `pad_sdl_feed.py`), `host <= 0` against `host < 0` in the trigger scale (0 >> 7
is 0 either way) and the final `return true` of `xinput_host_source_fn` against `return false` (every applied event
was already installed as its own raw report, so the final install is a no-op by T731's change comparison).
"""

SRC = "src/input/xinput_host_source.c"
OPTIONS = "src/host/host_options.c"
SUITE = ["test_xinput_host_source"]


def _m(
    mutation_id: str, old: str, new: str, why: str, file: str = SRC, suite: list[str] | None = None
) -> dict:
    return {
        "id": f"xin-host-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(suite or SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _m(
        "key-up-start",
        'DIG("ENTER", XINPUT_BUTTON_START)',
        'DIG("ENTER", XINPUT_BUTTON_BACK)',
        "ENTER is START.",
    ),
    _m(
        "key-back",
        'DIG("BACKSPACE", XINPUT_BUTTON_BACK)',
        'DIG("BACKSPACE", XINPUT_BUTTON_START)',
        "BACKSPACE is BACK.",
    ),
    _m(
        "key-dpad-left",
        'DIG("LEFT", XINPUT_BUTTON_DPAD_LEFT)',
        'DIG("LEFT", XINPUT_BUTTON_DPAD_RIGHT)',
        "LEFT is dpad left.",
    ),
    _m(
        "key-lthumb",
        'DIG("E", XINPUT_BUTTON_LEFT_THUMB)',
        'DIG("E", XINPUT_BUTTON_RIGHT_THUMB)',
        "E is the left thumb.",
    ),
    _m("key-face-a", 'ANA("Z", FACE_A)', 'ANA("Z", FACE_B)', "Z is face A."),
    _m("key-face-white", 'ANA("W", FACE_WHITE)', 'ANA("W", FACE_BLACK)', "W is WHITE."),
    _m(
        "key-left-trigger",
        'ANA("1", XINPUT_ANALOG_LEFT_TRIGGER)',
        'ANA("1", XINPUT_ANALOG_RIGHT_TRIGGER)',
        "1 is the left trigger.",
    ),
    _m(
        "key-stick-up-sign",
        'STK("I", 1u, 1)',
        'STK("I", 1u, -1)',
        "I pushes the left stick up (positive Y).",
    ),
    _m(
        "key-stick-axis", 'STK("L", 0u, 1)', 'STK("L", 1u, 1)', "L pushes the left stick right (X)."
    ),
    _m(
        "key-rstick-axis",
        'STK("H", 2u, 1)',
        'STK("H", 3u, 1)',
        "H pushes the right stick right (RX).",
    ),
    _m(
        "key-unmapped-esc-mapped",
        'NONE("ESCAPE")',
        'DIG("ESCAPE", XINPUT_BUTTON_BACK)',
        "ESCAPE is known and unmapped.",
    ),
    _m("pad-face-lb", 'ANA("LB", FACE_BLACK)', 'ANA("LB", FACE_WHITE)', "LB is BLACK."),
    _m(
        "pad-dpad-down",
        'DIG("DPAD_DOWN", XINPUT_BUTTON_DPAD_DOWN)',
        'DIG("DPAD_DOWN", XINPUT_BUTTON_DPAD_UP)',
        "DPAD_DOWN is dpad down.",
    ),
    _m(
        "pad-rstick",
        'DIG("RSTICK", XINPUT_BUTTON_RIGHT_THUMB)',
        'DIG("RSTICK", XINPUT_BUTTON_LEFT_THUMB)',
        "RSTICK is the right thumb.",
    ),
    _m(
        "pad-guide-mapped",
        'NONE("GUIDE")',
        'DIG("GUIDE", XINPUT_BUTTON_BACK)',
        "GUIDE is known and unmapped.",
    ),
    _m(
        "axis-rt-name",
        '"LX", "LY", "RX", "RY", "LT", "RT"',
        '"LX", "LY", "RX", "RY", "RT", "LT"',
        "axis order is the code.",
    ),
    _m(
        "axis-lt-index",
        "#define AXIS_LT 4u",
        "#define AXIS_LT 3u",
        "the triggers follow the four stick axes.",
    ),
    _m(
        "trigger-scale-shift",
        "(uint8_t)(host >> 7)",
        "(uint8_t)(host >> 6)",
        "255 at full, 32 at 4096.",
    ),
    _m(
        "trigger-sign-ignored",
        "host <= 0 ? 0u : (uint8_t)(host >> 7)",
        "(uint8_t)(host >> 7)",
        "a negative trigger reads 0.",
    ),
    _m(
        "stick-passthrough-lx",
        "state.thumb_left_x = source->axes[0];",
        "state.thumb_left_x = source->axes[1];",
        "LX is LX.",
    ),
    _m(
        "stick-passthrough-ry",
        "state.thumb_right_y = source->axes[3];",
        "state.thumb_right_y = source->axes[2];",
        "RY is RY.",
    ),
    _m(
        "clamp-high",
        "value > 32767 ? 32767 : value",
        "value > 32767 ? 32766 : value",
        "full positive deflection.",
    ),
    _m(
        "clamp-low",
        "value < -32768 ? -32768 : (",
        "value < -32767 ? -32767 : (",
        "full negative deflection.",
    ),
    _m(
        "key-full-negative",
        "return positive ? 32767 : -32768;",
        "return positive ? 32767 : -32767;",
        "negative key is -32768.",
    ),
    _m(
        "key-cancel",
        "if (positive == negative) return 0;",
        "if (positive == negative) return positive ? 32767 : 0;",
        "opposite keys cancel.",
    ),
    _m(
        "key-cancel-one",
        "if (positive == negative) return 0;",
        "if (!positive && !negative) return 0;",
        "opposite keys cancel to 0.",
    ),
    _m(
        "face-analog-255",
        "state.analog[table[i].analog] = 255u;",
        "state.analog[table[i].analog] = 254u;",
        "face buttons report 255.",
    ),
    _m(
        "held-not-cleared",
        "else source->held &= ~bit;",
        "else source->held &= ~0u;",
        "key-up releases.",
    ),
    _m(
        "down-not-held",
        "if (event->action == XINPUT_HOST_DOWN) source->held |= bit;",
        "if (event->action == XINPUT_HOST_UP) source->held |= bit;",
        "key-down presses.",
    ),
    _m(
        "other-device-applied",
        "if (event->device != source->device) {",
        "if (false) {",
        "the other device's events are ignored.",
    ),
    _m(
        "other-device-uncounted",
        "source->other_device++;",
        "(void)0;",
        "ignored events are counted.",
    ),
    _m(
        "unknown-code-accepted",
        "if (event->code >= count) {",
        "if (event->code > count) {",
        "code == count is out of table.",
    ),
    _m(
        "unknown-uncounted",
        '        source->unknown++;\n        fprintf(stderr, "xinput host source: unknown %s code',
        '        fprintf(stderr, "xinput host source: unknown %s code',
        "unknown codes are counted.",
    ),
    _m(
        "unmapped-applied",
        "if (table[event->code].kind == TARGET_NONE) {",
        "if (table[event->code].kind == TARGET_STICK) {",
        "an unmapped key is not applied.",
    ),
    _m("unmapped-uncounted", "source->unmapped++;", "(void)0;", "unmapped keys are counted."),
    _m(
        "axis-keyboard-accepted",
        "if (source->device != XINPUT_HOST_GAMEPAD || event->code >= AXIS_COUNT) {",
        "if (event->code >= AXIS_COUNT) {",
        "the keyboard has no axes.",
    ),
    _m(
        "axis-code-off-by-one",
        "event->code >= AXIS_COUNT) {",
        "event->code > AXIS_COUNT) {",
        "axis code 6 is out of table.",
    ),
    _m("applied-uncounted", "source->applied++;", "(void)0;", "applied events are counted."),
    _m(
        "report-per-event-removed",
        "(void)xinput_hle_set_synthetic_pad_state(0u, compose(source));",
        "(void)0;",
        "each event is a raw report (T731).",
    ),
    _m(
        "event-early",
        "feed->events[feed->next].poll > poll_index) return false;",
        "feed->events[feed->next].poll > poll_index + 1u) return false;",
        "an event is not applied before its poll.",
    ),
    _m(
        "event-late",
        "feed->events[feed->next].poll > poll_index) return false;",
        "feed->events[feed->next].poll >= poll_index) return false;",
        "an event at poll N applies before sample N.",
    ),
    _m(
        "feed-not-consumed",
        "*out = feed->events[feed->next++];",
        "*out = feed->events[feed->next];",
        "events are consumed once.",
    ),
    _m(
        "source-state-dropped",
        "    *out = compose(source);\n    return true;",
        "    (void)out;\n    return true;",
        "the returned state is the composed one.",
    ),
    _m(
        "parse-backwards-allowed",
        "event.poll < state->events[state->count - 1u].poll",
        "false",
        "polls may not go backwards.",
    ),
    _m(
        "parse-backwards-equal",
        "event.poll < state->events[state->count - 1u].poll",
        "event.poll <= state->events[state->count - 1u].poll",
        "equal polls are allowed.",
    ),
    _m(
        "parse-unknown-device-accepted",
        'else { fail(error, size, number, "unknown device", device); return false; }',
        "else event->device = XINPUT_HOST_KEYBOARD;",
        "an unknown device is refused.",
    ),
    _m(
        "parse-unknown-action-accepted",
        'else { fail(error, size, number, "unknown action", action); return false; }',
        "else event->action = XINPUT_HOST_DOWN;",
        "an unknown action is refused.",
    ),
    _m(
        "parse-keyboard-axis",
        "if (event->device != XINPUT_HOST_GAMEPAD) { fail(",
        "if (false) { fail(",
        "the keyboard has no axis.",
    ),
    _m(
        "parse-extra-token",
        'if (value != NULL) { fail(error, size, number, "unexpected extra token"',
        'if (false) { fail(error, size, number, "unexpected extra token"',
        "extra tokens are refused.",
    ),
    _m(
        "parse-name-case",
        "toupper((unsigned char)*a) != toupper((unsigned char)*b)",
        "*a != *b",
        "names are case insensitive.",
    ),
    _m("parse-name-prefix", "return *a == *b;", "return true;", "a name must match completely."),
    _m(
        "parse-poll-range",
        "parse_long(poll, 0, 1000000000L, &number_value)",
        "parse_long(poll, -1, 1000000000L, &number_value)",
        "a negative poll is refused.",
    ),
    _m(
        "parse-raw-code",
        'strncmp(name, "CODE:", 5u) == 0 || strncmp(name, "code:", 5u) == 0',
        "false",
        "CODE:n exercises the unknown-code path.",
    ),
    _m(
        "provider-missing-accepted",
        "if (feed_path == NULL) {",
        "if (false) {",
        "no feed means no provider: refused.",
    ),
    _m(
        "provider-missing-name",
        '"no event provider for the %s source: pass --present window (the SDL3 %s provider, needs a build "',
        '"no event provider for the %s source: pass --present window (the SDL3 %s thing, needs a build "',
        "the refusal names the SDL3 provider.",
    ),
    _m(
        "provider-missing-gamepad-name",
        'keyboard ? "window keyboard" : "gamepad");',
        'keyboard ? "window keyboard" : "pad");',
        "the refusal names the SDL3 gamepad provider.",
    ),
    _m(
        "create-bad-device",
        "if (device != XINPUT_HOST_KEYBOARD && device != XINPUT_HOST_GAMEPAD) return NULL;",
        "(void)device;",
        "an unknown device is refused.",
    ),
    _m(
        "kind-gamepad",
        'strcmp(text, "gamepad") == 0) *kind = XINPUT_PAD_SOURCE_GAMEPAD;',
        'strcmp(text, "gamepad") == 0) *kind = XINPUT_PAD_SOURCE_KEYBOARD;',
        "gamepad parses to gamepad.",
    ),
    _m(
        "kind-any-accepted",
        "    else return false;\n    return true;\n}\nxinput_host_source *xinput_pad_source_open",
        "    else *kind = XINPUT_PAD_SOURCE_KEYBOARD;\n    return true;\n}\nxinput_host_source *xinput_pad_source_open",
        "an unknown source name is refused.",
    ),
    _m(
        "option-source-needs-pad",
        "if (!out->synthetic_pad && !out->controllers) return false;",
        "(void)0;",
        "--pad-source needs --synthetic-pad or --controllers.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "option-script-source-needs-script",
        "return out->pad_script != NULL && out->pad_feed == NULL;",
        "return true;",
        "script source needs --pad-script and no feed.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "option-unknown-source",
        'if (strcmp(out->pad_source, "keyboard") != 0 && strcmp(out->pad_source, "gamepad") != 0) return false;',
        "(void)0;",
        "an unknown source name is refused.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "option-device-source-excludes-script",
        "    return out->pad_script == NULL;\n}",
        "    return true;\n}",
        "keyboard and gamepad exclude --pad-script.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "option-feed-needs-source",
        "if (out->pad_source == NULL) return out->pad_feed == NULL;",
        "if (out->pad_source == NULL) return true;",
        "--pad-feed needs --pad-source.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "option-feed-not-stored",
        "out->pad_feed = argv[i];",
        "(void)argv[i];",
        "the feed path is kept.",
        OPTIONS,
        ["test_host_options"],
    ),
]
