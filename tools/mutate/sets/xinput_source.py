# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the host controller input sources in `src/input/xinput_source.c` (T707).

Kill suite: ctest `test_xinput_source` (plain build, no XBE), `test_host_options` for the flag. Every `old`
occurs exactly once (`tests/test_mutation_anchors.py`).

Families: poll counting and the decline path, the scripted text format (frame counts, token tables, value
ranges, case, comments, error text), run boundaries and the rest state after the end, the adapter hook.
"""

SRC = "src/input/xinput_source.c"
DEV = "src/input/xinput_devices.c"
OPTIONS = "src/host/host_options.c"
SUITE = ["test_xinput_source"]


def _m(
    mutation_id: str, old: str, new: str, why: str, file: str = SRC, suite: list[str] | None = None
) -> dict:
    return {
        "id": f"xin-src-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(suite or SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _m(
        "poll-index-preincrement",
        "const uint64_t index = port_polls[port]++; polls++;",
        "const uint64_t index = ++port_polls[port]; polls++;",
        "the first poll is index 0.",
    ),
    _m(
        "poll-no-source-null-guard",
        "port != 0u || legacy_fn == NULL || !legacy_fn(index, &state, legacy_user)",
        "port != 0u || false || !legacy_fn(index, &state, legacy_user)",
        "no source means no call.",
    ),
    _m(
        "poll-ignores-decline",
        "port != 0u || legacy_fn == NULL || !legacy_fn(index, &state, legacy_user)",
        "port != 0u || legacy_fn == NULL || (!legacy_fn(index, &state, legacy_user) && false)",
        "a declining source leaves the state alone.",
    ),
    _m(
        "poll-wrong-port",
        "bool installed = xinput_hle_set_synthetic_pad_state(port, state);",
        "bool installed = xinput_hle_set_synthetic_pad_state(1u, state);",
        "the pad is port 0.",
    ),
    _m(
        "reset-keeps-count",
        "ports_source_user = NULL; polls = 0u;",
        "ports_source_user = NULL;",
        "reset restarts the poll index.",
    ),
    _m(
        "frames-zero-allowed",
        "parse_number(first, 1, 1000000, &frames)",
        "parse_number(first, 0, 1000000, &frames)",
        "a zero frame run is refused.",
    ),
    _m(
        "frames-max-off-by-one",
        "parse_number(first, 1, 1000000, &frames)",
        "parse_number(first, 1, 1000001, &frames)",
        "the run limit is exact.",
    ),
    _m(
        "analog-max-256",
        "parse_number(value_text, 0, 255, &value)",
        "parse_number(value_text, 0, 256, &value)",
        "an analog byte is 0..255.",
    ),
    _m(
        "analog-min-negative",
        "parse_number(value_text, 0, 255, &value)",
        "parse_number(value_text, -1, 255, &value)",
        "an analog byte is not negative.",
    ),
    _m(
        "axis-min-off-by-one",
        "parse_number(value_text, -32768, 32767, &value)",
        "parse_number(value_text, -32767, 32767, &value)",
        "an axis may be -32768.",
    ),
    _m(
        "axis-max-off-by-one",
        "parse_number(value_text, -32768, 32767, &value)",
        "parse_number(value_text, -32768, 32768, &value)",
        "an axis may not be 32768.",
    ),
    _m(
        "face-pressed-value",
        "state->analog[i] = 255u; return true;",
        "state->analog[i] = 254u; return true;",
        "a pressed face button is full pressure.",
    ),
    _m(
        "face-covers-triggers",
        "for (unsigned i = 0u; i < 6u; i++)",
        "for (unsigned i = 0u; i < 8u; i++)",
        "only six analog entries are bare button tokens, LT and RT need a value.",
    ),
    _m(
        "digital-start-is-back",
        '{"START", XINPUT_BUTTON_START}',
        '{"START", XINPUT_BUTTON_BACK}',
        "token START sets the START bit.",
    ),
    _m(
        "digital-left-is-right",
        '{"LEFT", XINPUT_BUTTON_DPAD_LEFT}',
        '{"LEFT", XINPUT_BUTTON_DPAD_RIGHT}',
        "token LEFT sets the LEFT bit.",
    ),
    _m(
        "digital-drops-or",
        "state->digital_buttons |= digital[i].bit;",
        "state->digital_buttons = digital[i].bit;",
        "buttons accumulate within a line.",
    ),
    _m(
        "case-sensitive",
        "*c = (char)toupper((unsigned char)*c);",
        "*c = *c;",
        "tokens are case insensitive.",
    ),
    _m(
        "comment-not-stripped",
        "        if (hash != NULL) *hash = '\\0';",
        "        (void)hash;",
        "# starts a comment.",
    ),
    _m(
        "trigger-index-swapped",
        'static const char *const analog[XINPUT_ANALOG_COUNT] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};',
        'static const char *const analog[XINPUT_ANALOG_COUNT] = {"A", "B", "X", "Y", "BLACK", "WHITE", "RT", "LT"};',
        "LT is analog entry 6, RT 7 (measured).",
    ),
    _m(
        "axis-names-swapped",
        'static const char *const axes[4] = {"LX", "LY", "RX", "RY"};',
        'static const char *const axes[4] = {"LY", "LX", "RX", "RY"};',
        "LX is the left thumb X.",
    ),
    _m(
        "total-counts-entries",
        "script->total += (uint64_t)frames;",
        "script->total += 1u;",
        "the total is in frames.",
    ),
    _m(
        "run-end-inclusive",
        "if (poll_index < base + script->entries[i].frames) return script->entries[i].state;",
        "if (poll_index <= base + script->entries[i].frames) return script->entries[i].state;",
        "a run covers exactly its frames.",
    ),
    _m(
        "end-holds-last",
        "    return rest;\n}\nstatic bool script_source",
        "    return script->count ? script->entries[script->count - 1u].state : rest;\n}\nstatic bool script_source",
        "after the end the pad is at rest.",
    ),
    _m(
        "error-without-line",
        "snprintf(error, size, \"line %u: %s '%s'\", line, what, token);",
        "snprintf(error, size, \"line %u: %s '%s'\", line + 1u, what, token);",
        "errors name the right line.",
    ),
    _m(
        "hook-polls-removed",
        "if (!pad_removed) (void)xinput_source_poll();",
        "(void)xinput_source_poll();",
        "a removed pad is not polled.",
        DEV,
    ),
    _m(
        "hook-missing",
        "if (!pad_removed) (void)xinput_source_poll();",
        "(void)0;",
        "GetState consults the source.",
        DEV,
    ),
    _m(
        "option-needs-pad",
        "(out->pad_script == NULL || out->synthetic_pad || out->controllers) &&",
        "true &&",
        "--pad-script needs --synthetic-pad or --controllers.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "option-not-stored",
        "out->pad_script = argv[i];\n            out->pad_script_live = false;",
        "(void)argv[i];\n            out->pad_script_live = false;",
        "the path is kept.",
        OPTIONS,
        ["test_host_options"],
    ),
    _m(
        "live-bad-line-not-counted",
        "else live->refused++;",
        "else live->refused += 0u;",
        "T1222: a line that does not parse is counted and keeps the previous state.",
    ),
    _m(
        "live-parse-drops-state",
        "    *out = state;\n    return true;\n}\nxinput_live *xinput_live_open",
        "    memset(out, 0, sizeof(*out));\n    return true;\n}\nxinput_live *xinput_live_open",
        "T1222: the parsed line is what the pad reports.",
    ),
    _m(
        "live-comment-not-stripped",
        "    char *hash = strchr(line, '#');\n    if (hash != NULL) *hash = '\\0';\n    xinput_pad_state state;",
        "    xinput_pad_state state;",
        "T1222: a # starts a comment in the live line.",
    ),
    _m(
        "live-reads-wrong-offset",
        "pread(live->fd, buffer, sizeof(buffer), 0)",
        "pread(live->fd, buffer, sizeof(buffer), 1)",
        "T1222: the live line is read from the start of the file.",
    ),
    _m(
        "live-option-not-live",
        "out->pad_script = argv[i];\n            out->pad_script_live = true;",
        "out->pad_script = argv[i];\n            out->pad_script_live = false;",
        "T1222: --pad-script-live selects the live pad.",
        OPTIONS,
        ["test_host_options"],
    ),
]
