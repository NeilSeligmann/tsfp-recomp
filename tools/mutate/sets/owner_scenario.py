# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutations of the owner scenario renderer and its use by the guided session (T1767): option and condition handling,
repeat order and resume point, phase lookup and fallback, the generic-text warnings, the hotkey fallback, the file
checks. Run with `python tools/mutate/py_mutants.py --set owner_scenario` in a clean worktree.

Kill suite: `tests/test_t1767_owner_scenario.py`. Every `old` occurs exactly once (tests/test_mutation_anchors.py).
"""

MUTATIONS: list[dict] = []  # c_suites needs a native list, this set has none

_SCENARIO = "tools/owner_scenario.py"
_PROFILE = "tools/action_profile.py"
_TARGET = ["pytest: tests/test_t1767_owner_scenario.py"]


def _m(mutation_id: str, file: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"owner-scenario-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": _TARGET,
        "why": why,
    }


PYTHON_MUTATIONS: list[dict] = [
    _m(
        "multi-as-single",
        _SCENARIO,
        'if "values" in spec and not spec.get("multi"):',
        'if "values" in spec:',
        "a multi option is read as one value, 'batch=2,1' is no list",
    ),
    _m(
        "negation-ignored",
        _SCENARIO,
        'negate = want.startswith("!")',
        "negate = False",
        "a '!value' condition is read as the value itself",
    ),
    _m(
        "condition-inverted",
        _SCENARIO,
        "if hit != negate:",
        "if hit == negate:",
        "every condition is inverted",
    ),
    _m(
        "hotkeys-off-inverted",
        _SCENARIO,
        'if values.get("hotkeys") == "off":',
        'if values.get("hotkeys") != "off":',
        "the chord text shows when hotkeys are off and Enter when they are on",
    ),
    _m(
        "script-name-unchecked",
        _SCENARIO,
        'data["script"] != script',
        'data["script"] == script',
        "a file whose script field names another script is accepted",
    ),
    _m(
        "entry-max-off-by-one",
        _SCENARIO,
        "if not 0 <= value <= ENTRY_MAX:",
        "if not 0 <= value < ENTRY_MAX:",
        "the last weapon entry 0x45 is refused",
    ),
    _m(
        "entry-max-open",
        _SCENARIO,
        "if not 0 <= value <= ENTRY_MAX:",
        "if not 0 <= value <= ENTRY_MAX + 1:",
        "entry 0x46 is accepted",
    ),
    _m(
        "unknown-option-accepted",
        _SCENARIO,
        "if name not in declared:",
        "if False and name not in declared:",
        "a misspelt option is silently ignored",
    ),
    _m(
        "enum-value-unchecked",
        _SCENARIO,
        'bad = [v for v in chosen if v not in spec["values"]]',
        'bad = [v for v in chosen if v in spec["values"]]',
        "a value outside the declared set is accepted, a valid one refused",
    ),
    _m(
        "empty-multi-accepted",
        _SCENARIO,
        "if not chosen:",
        "if False:",
        "an empty multi option is accepted",
    ),
    _m(
        "empty-entries-accepted",
        _SCENARIO,
        "if not split_list(value):",
        "if False:",
        "an empty weapon list is accepted",
    ),
    _m(
        "repeat-order-sorted",
        _SCENARIO,
        "tokens = selected(spec, values[over])",
        "tokens = sorted(selected(spec, values[over]))",
        "the order of the selected batches is not kept",
    ),
    _m(
        "onwards-skips-start",
        _SCENARIO,
        "tokens = keys[keys.index(values[over]) :]",
        "tokens = keys[keys.index(values[over]) + 1 :]",
        "the resume batch itself is not shown",
    ),
    _m(
        "positional-row-lost",
        _SCENARIO,
        'source = table.get(token) or table.get(f"#{index}") or {}',
        "source = table.get(token) or {}",
        "a row keyed by position is never found",
    ),
    _m(
        "count-atleast-off-by-one",
        _SCENARIO,
        '(count < int(text.rstrip("+")))',
        '(count <= int(text.rstrip("+")))',
        "'2+' needs three values",
    ),
    _m(
        "count-exact-loose",
        _SCENARIO,
        "else (count != int(text))",
        "else (count < int(text))",
        "'1' also holds for three values",
    ),
    _m(
        "placeholder-unknown-empty",
        _SCENARIO,
        'raise ScenarioError(f"unknown placeholder {{{key}}}")',
        'return ""',
        "a misspelt placeholder renders as nothing",
    ),
    _m(
        "hotkey-text-fixed",
        _SCENARIO,
        "return hotkey_defaults.hint_line(label)",
        'return "press the key"',
        "the chord text is not the host source",
    ),
    _m(
        "kind-unchecked",
        _SCENARIO,
        'if data["kind"] not in KINDS:',
        'if data["kind"] in KINDS:',
        "an unknown kind is accepted",
    ),
    _m(
        "send-back-optional",
        _SCENARIO,
        'if section in ("steps", "send_back") and not items:',
        'if section in ("steps",) and not items:',
        "a scenario without a SEND ME list is accepted",
    ),
    _m(
        "seconds-not-shown",
        _SCENARIO,
        """+ (f"   [about {node['seconds']} s]" if node["seconds"] else "")""",
        '+ ""',
        "the durations are not printed",
    ),
    _m(
        "phase-step-no-switch",
        _SCENARIO,
        'if phase.get("switch"):',
        'if not phase.get("switch"):',
        "the switch line of a phase step is inverted",
    ),
    _m(
        "phase-step-seconds",
        _SCENARIO,
        '"seconds": entry.get("seconds") or phase.get("seconds"),',
        '"seconds": entry.get("seconds"),',
        "a phase step shows no duration of its phase",
    ),
    _m(
        "phase-exact-lookup-lost",
        _SCENARIO,
        'phase = phases.get(f"{scenario_id}:{label}") if label else None',
        "phase = None",
        "the text of one label is never found, only the id wide one",
    ),
    _m(
        "phase-fallback-lost",
        _SCENARIO,
        "phase = phases.get(scenario_id)",
        "phase = None",
        "a label without its own text gets no id wide text",
    ),
    _m(
        "phase-label-placeholder",
        _SCENARIO,
        'extra = {"label": label or ""}',
        "extra = {}",
        "{label} in an id wide phase text cannot be filled",
    ),
    _m(
        "phase-note-dropped",
        _SCENARIO,
        '"note": fill(phase["note"], values, extra) if phase.get("note") else None,',
        '"note": None,',
        "the note of a phase never reaches the prompt",
    ),
    _m(
        "opts-name-unchecked",
        _SCENARIO,
        "if not sep or not OPTION_RE.fullmatch(name):",
        "if not sep:",
        "an option with an invalid name is accepted",
    ),
    _m(
        "missing-script-unreported",
        _SCENARIO,
        "if fish.stem not in names:",
        "if fish.stem in names:",
        "check misses an owner script without a scenario",
    ),
    _m(
        "specific-not-set",
        _PROFILE,
        "specific=True,",
        "specific=False,",
        "script text is not marked specific, the pistol prefix comes back",
    ),
    _m(
        "seconds-not-applied",
        _PROFILE,
        'if text["seconds"]:',
        "if False:",
        "the script's duration is ignored",
    ),
    _m(
        "missing-phase-silent",
        _PROFILE,
        'elif "warning" not in entry:',
        "elif False:",
        "a phase without script text or a labelled generic phase does not warn",
    ),
    _m(
        "missing-phase-wrong-branch",
        _PROFILE,
        '            if script:\n                entry["warning"] = MISSING_PHASE_WARNING',
        '            if not script:\n                entry["warning"] = MISSING_PHASE_WARNING',
        "the warning kinds are swapped",
    ),
    _m(
        "generic-label-silent",
        _PROFILE,
        'elif label:\n                entry["warning"] = GENERIC_LABEL_WARNING',
        'elif False:\n                entry["warning"] = GENERIC_LABEL_WARNING',
        "a labelled phase with the generic pistol text does not warn",
    ),
    _m(
        "steps-not-skipped",
        _PROFILE,
        'if entry["kind"] in action_advance.STEP_KINDS:\n            result.append(entry)',
        "if False:\n            result.append(entry)",
        "plan steps and questions are treated as phases",
    ),
    _m(
        "instruction-prefix-kept",
        _PROFILE,
        'if scenario.get("specific"):\n        return scenario["instruction"]',
        'if False:\n        return scenario["instruction"]',
        "the generic 'Switch to: label. Do:' prefix is put in front of script text",
    ),
    _m(
        "warning-not-printed",
        _PROFILE,
        'if scenario.get("warning"):\n                say(f"!!! WARNING (T1767)',
        'if False:\n                say(f"!!! WARNING (T1767)',
        "the loud warning is never shown in the session",
    ),
    _m(
        "switch-line-lost",
        _PROFILE,
        "f\"Switch to: {scenario['switch']}  (do this FIRST, during the throwaway step below)\"",
        "f\"Change: {scenario['switch']}  (do this FIRST, during the throwaway step below)\"",
        "the script's switch line is not shown",
    ),
    _m(
        "note-line-lost",
        _PROFILE,
        'if scenario.get("note"):\n                say(f"Note:  ',
        'if False:\n                say(f"Note:  ',
        "the note is not shown before the phase",
    ),
    _m(
        "note-reminder-lost",
        _PROFILE,
        'if scenario.get("note"):\n                    say(f"Remember to note: ',
        'if False:\n                    say(f"Remember to note: ',
        "the note is not repeated after the phase",
    ),
    _m(
        "text-missing-not-recorded",
        _PROFILE,
        'if s.get("warning") and "kind" in s',
        'if not s.get("warning") and "kind" in s',
        "session.json lists the phases WITH text as missing",
    ),
    _m(
        "bad-owner-opt-ignored",
        _PROFILE,
        'raise SystemExit(f"--owner-opt: {error}") from error',
        "return {}",
        "a malformed --owner-opt is silently dropped",
    ),
]
