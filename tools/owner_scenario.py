#!/usr/bin/env python3
# ruff: noqa: E501
"""T1767: the text that tells the owner WHAT TO DO in an owner session, one scenario per script.

Why: the owner scripts (`tmp/*.fish`, gitignored) and `tools.action_profile session` used to show ONE generic example (the
phase table of `tools/data/action_scenarios.json`: "Fire the pistol only" at "Story 2401" for every weapon label, or a
hard coded pistol and bat plan in `run_button_dumps.fish`). The real scenario of a run (which map, which weapons in which
spawner, which buttons, how long, what to note, how to stop, what to send back) lives in ONE data file per script:

    tools/data/owner_scenarios/<script>.json        (<script> = the fish file name without `.fish`)

Rendered from fish with `python -m tools.owner_scenario show SCRIPT [--opt KEY=VALUE ...]` and used by
`python -m tools.action_profile session --owner-script SCRIPT [--owner-opt KEY=VALUE ...]` for the per-phase prompts
("Go to / Then / Time / Note") instead of the generic example. How to add one: docs/input-replay.md, section
"Owner scenarios (T1767)".

File format (all strings are plain text, `{name}` placeholders are filled strictly, a literal brace is `{{` or `}}`):

    script        file stem, must match the file name
    title         one line, shown in the banner
    summary       one or two sentences: what this run measures and why
    kind          census-session | live-dump | recording | button-dump | helper | wrapper
    duration      optional "about 25 minutes"
    options       {name: {values: [...], multi: bool, default: "..."} or {free: true, default: "..."} or
                   {entries: true, default: "0x02,0x04"}}  the knobs of the script; `--opt` is validated against them.
                   `entries` = a comma list of weapon entry numbers: a repeat over it adds {entry} {name} {spawner}.
    prerequisites list of LINE, shown first ("Before you start")
    steps         list of STEP, numbered in the order given ("What to do")
    phases        {"ingame-fire:minigun": PHASE, "ingame-idle": PHASE}, the prompt text of one census phase of
                  tools.action_profile (key = `id:label`, else `id`)
    tables        free data for `repeat` (key -> row dict, a row may hold `steps`, the sub steps of that repeat)
    note_at_end   list of LINE ("Write down")
    stop          list of LINE ("How to stop")
    send_back     list of LINE ("SEND ME")

    LINE  = "text" | {"text": "...", "when": COND}
    STEP  = LINE | {"text": "...", "seconds": 20, "when": COND, "substeps": [LINE], "repeat": {"over": OPTION, "table": TABLE}}
    REPEAT row placeholders: {item} the selected value, {n} its position (1 based, the order of the option value is kept),
                  the fields of tables[TABLE][item] (its `steps` become sub steps) and, for an `entries` option,
                  {entry} {name} {spawner}.
    ROW EXTRAS: a row may also hold `items` (list of dicts) with `item_step` (a template filled per dict, plus {slot} 1..n) and
                  `steps_after` (lines added after them). `repeat.onwards: true` repeats over the TABLE KEYS from the
                  selected option value to the end (a resume point). A row is found by the selected value, else by `#<position>`.
    COUNT COND  = {"batch#": "2+"} at least 2 values of the multi option are selected, {"batch#": "1"} exactly 1.
    COND  = {"option": "value" | ["v1", "v2"] | "!value"}   every key must match; for a multi option, any selected value
            {"option#": "2+"} = at least 2 values selected, {"option#": "1"} = exactly 1 (T1767: "only when several batches")
    repeat = {"over": OPTION, "table": TABLE, "onwards": true}   `onwards`: the rows are the table keys from the selected value
            of OPTION to the end of the table (run_weapon_variants: every batch from START on)
    a table row may hold `items` (list of dicts) with `item_step` (a template filled per item with its fields, `{slot}` = 1..n and
            the row fields): the sub steps of that repeat are steps, then one line per item, then `steps_after`
    PHASE = {"title": "...", "go_to": "...", "do": "...", "seconds": 20, "switch": "...", "note": "..."}

`{hotkey:advance}` expands to the chord text of tools.hotkey_defaults (the one source the host uses), or to "Enter in this
terminal" when the option `hotkeys` is `off`. Nothing here starts the game.
"""

from __future__ import annotations

import argparse
import itertools
import json
import re
import string
import sys
import textwrap
from collections.abc import Callable, Iterator
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCENARIO_DIR = Path("tools/data/owner_scenarios")

# every owner script that exists in the main checkout's tmp/ (gitignored); tests require one scenario file for each
OWNER_SCRIPTS: tuple[str, ...] = (
    "dump_now",
    "inventory_poke",
    "poke_now",
    "record_mapmaker_route",
    "run_button_checklist",
    "run_button_dumps",
    "run_audio_record",
    "run_collide",
    "run_cutscene",
    "run_enemy_encounter",
    "run_objective",
    "run_save_reload",
    "run_forced_weapons",
    "run_inventory_session",
    "run_menu_audio_video",
    "run_render_shots",
    "run_settings_dump",
    "run_minigun_mine",
    "run_missing_weapons",
    "run_story_weapons",
    "run_t1642_session4",
    "run_uplink_story",
    "run_weapon_census",
    "run_weapon_variants",
    "run_weaponslot_probe",
    "run_weaponslot_session",
)
KINDS = (
    "census-session",
    "live-dump",
    "recording",
    "button-dump",
    "helper",
    "wrapper",
)
REQUIRED_KEYS = ("script", "title", "summary", "kind", "steps", "send_back")
SECTIONS = (
    ("prerequisites", "BEFORE YOU START"),
    ("steps", "WHAT YOU NEED TO DO (in this order)"),
    ("note_at_end", "WRITE DOWN / NOTE"),
    ("stop", "HOW TO STOP"),
    ("send_back", "SEND ME"),
)
HOTKEY_RE = re.compile(r"\{hotkey:([a-z]+)\}")
SCRIPT_RE = re.compile(r"[a-z0-9_]+")
OPTION_RE = re.compile(r"[a-z][a-z0-9_]*")
ENTRY_MAX = 0x45
ROW_LISTS = ("steps", "steps_after", "items", "item_step")
HOTKEY_FALLBACK = "Enter in this terminal"
DEFAULT_WIDTH = 110


class ScenarioError(ValueError):
    """A missing or malformed scenario, a bad option, or a placeholder that cannot be filled."""


# --------------------------------------------------------------------------------------------- loading
def scenario_path(script: str, root: Path | None = None) -> Path:
    if not SCRIPT_RE.fullmatch(script):
        raise ScenarioError(f"bad script name {script!r} (lowercase letters, digits, underscore)")
    return (root or ROOT) / SCENARIO_DIR / f"{script}.json"


def load_script(script: str, root: Path | None = None) -> dict:
    path = scenario_path(script, root)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ScenarioError(
            f"no owner scenario for {script!r}: create {SCENARIO_DIR}/{script}.json (docs/input-replay.md, Owner scenarios T1767)"
        ) from error
    except (OSError, json.JSONDecodeError) as error:
        raise ScenarioError(f"cannot read {path}: {error}") from error
    problems = validate(data, script)
    if problems:
        raise ScenarioError(f"{path}: " + "; ".join(problems))
    return data


def available_scripts(root: Path | None = None) -> list[str]:
    directory = (root or ROOT) / SCENARIO_DIR
    return sorted(path.stem for path in directory.glob("*.json"))


# --------------------------------------------------------------------------------------------- validation
def validate(data: object, script: str | None = None) -> list[str]:
    """Every structural problem of a scenario file (empty list = fine). Placeholders are checked by rendering."""
    if not isinstance(data, dict):
        return ["the file is not a JSON object"]
    problems: list[str] = []
    for key in REQUIRED_KEYS:
        if key not in data:
            problems.append(f"missing key {key!r}")
    if problems:
        return problems
    if script is not None and data["script"] != script:
        problems.append(f"script is {data['script']!r}, the file is {script!r}")
    if data["kind"] not in KINDS:
        problems.append(f"kind {data['kind']!r} not in {KINDS}")
    for key in ("title", "summary"):
        if not isinstance(data[key], str) or not data[key].strip():
            problems.append(f"{key} must be a non empty string")
    options = data.get("options", {})
    if not isinstance(options, dict):
        return [*problems, "options must be an object"]
    for name, spec in options.items():
        problems += option_problems(name, spec)
    for section, _ in SECTIONS:
        items = data.get(section, [])
        if not isinstance(items, list):
            problems.append(f"{section} must be a list")
            continue
        if section in ("steps", "send_back") and not items:
            problems.append(f"{section} must not be empty")
        for index, item in enumerate(items):
            problems += item_problems(
                f"{section}[{index}]",
                item,
                options,
                data.get("tables", {}),
                data.get("phases", {}) if isinstance(data.get("phases", {}), dict) else {},
            )
    phases = data.get("phases", {})
    if not isinstance(phases, dict):
        problems.append("phases must be an object")
    else:
        for key, phase in phases.items():
            problems += phase_problems(key, phase)
    return problems


def option_problems(name: str, spec: object) -> list[str]:
    if not OPTION_RE.fullmatch(name):
        return [f"option name {name!r} must match {OPTION_RE.pattern}"]
    if not isinstance(spec, dict) or "default" not in spec:
        return [f"option {name}: needs an object with a default"]
    kinds = [key for key in ("values", "free", "entries") if key in spec]
    if len(kinds) != 1:
        return [f"option {name}: exactly one of values, free, entries"]
    if "values" in spec:
        values = spec["values"]
        if (
            not isinstance(values, list)
            or not values
            or not all(isinstance(v, str) for v in values)
        ):
            return [f"option {name}: values must be a non empty list of strings"]
        chosen = split_list(spec["default"]) if spec.get("multi") else [spec["default"]]
        if any(value not in values for value in chosen):
            return [f"option {name}: default {spec['default']!r} not in values"]
    return []


def item_problems(where: str, item: object, options: dict, tables: dict, phases: dict) -> list[str]:
    if isinstance(item, str):
        return [] if item.strip() else [f"{where}: empty text"]
    if not isinstance(item, dict):
        return [f"{where}: needs a text"]
    has_text = isinstance(item.get("text"), str) and bool(item["text"].strip())
    if not has_text and "phase" not in item:
        return [f"{where}: needs a text or a phase"]
    problems: list[str] = []
    if "phase" in item and item["phase"] not in phases:
        problems.append(f"{where}: phase {item['phase']!r} is not in phases")
    unknown = set(item) - {"text", "seconds", "when", "substeps", "repeat", "phase"}
    if unknown:
        problems.append(f"{where}: unknown keys {sorted(unknown)}")
    seconds = item.get("seconds")
    if seconds is not None and (
        not isinstance(seconds, int) or isinstance(seconds, bool) or seconds <= 0
    ):
        problems.append(f"{where}: seconds must be a positive integer")
    when = item.get("when")
    if when is not None:
        if not isinstance(when, dict) or not when:
            problems.append(f"{where}: when must be a non empty object")
        else:
            for key in when:
                if key.rstrip("#") not in options:
                    problems.append(f"{where}: when names the undeclared option {key!r}")
    repeat = item.get("repeat")
    if repeat is not None:
        if not isinstance(repeat, dict) or repeat.get("over") not in options:
            problems.append(f"{where}: repeat.over must name a declared option")
        elif "table" in repeat and repeat["table"] not in tables:
            problems.append(f"{where}: repeat.table {repeat['table']!r} is not in tables")
        elif repeat.get("onwards") and "table" not in repeat:
            problems.append(f"{where}: repeat.onwards needs a table")
    for index, sub in enumerate(item.get("substeps", [])):
        problems += item_problems(f"{where}.substeps[{index}]", sub, options, tables, phases)
    return problems


def phase_problems(key: str, phase: object) -> list[str]:
    if not re.fullmatch(r"[A-Za-z0-9_-]+(:[a-z0-9_]+)?", key):
        return [f"phase key {key!r} must be `id` or `id:label`"]
    if not isinstance(phase, dict):
        return [f"phase {key}: not an object"]
    problems = []
    for field in ("go_to", "do"):
        if not isinstance(phase.get(field), str) or not phase[field].strip():
            problems.append(f"phase {key}: needs a {field}")
    seconds = phase.get("seconds")
    if seconds is not None and (
        not isinstance(seconds, int) or isinstance(seconds, bool) or seconds <= 0
    ):
        problems.append(f"phase {key}: seconds must be a positive integer")
    unknown = set(phase) - {"title", "go_to", "do", "seconds", "switch", "note"}
    if unknown:
        problems.append(f"phase {key}: unknown keys {sorted(unknown)}")
    return problems


# --------------------------------------------------------------------------------------------- options and context
def split_list(text: str) -> list[str]:
    return [part.strip() for part in text.split(",") if part.strip()]


def parse_entry(token: str) -> int:
    try:
        value = int(token, 0)
    except ValueError as error:
        raise ScenarioError(f"bad weapon entry {token!r} (hex 0x.. or decimal)") from error
    if not 0 <= value <= ENTRY_MAX:
        raise ScenarioError(f"weapon entry {token} outside 0..0x{ENTRY_MAX:X}")
    return value


def resolve_options(data: dict, given: dict[str, str] | None) -> dict[str, str]:
    """The option values of one rendering: the declared defaults, overridden by `given` (validated)."""
    declared = data.get("options", {})
    values = {name: str(spec["default"]) for name, spec in declared.items()}
    for name, value in (given or {}).items():
        if name not in declared:
            raise ScenarioError(
                f"unknown option {name!r} for {data['script']} (declared: {sorted(declared)})"
            )
        spec = declared[name]
        if "values" in spec:
            chosen = split_list(value) if spec.get("multi") else [value]
            if not chosen:
                raise ScenarioError(f"option {name} is empty")
            bad = [v for v in chosen if v not in spec["values"]]
            if bad:
                raise ScenarioError(f"option {name}: {bad} not in {spec['values']}")
        elif "entries" in spec:
            for token in split_list(value):
                parse_entry(token)
            if not split_list(value):
                raise ScenarioError(f"option {name} needs at least one weapon entry")
        values[name] = value
    return values


def selected(spec: dict, value: str) -> list[str]:
    if "values" in spec and not spec.get("multi"):
        return [value]
    return split_list(value)


def condition_holds(when: dict | None, data: dict, values: dict[str, str]) -> bool:
    if not when:
        return True
    for name, wanted in when.items():
        if name.endswith("#"):
            count = len(selected(data["options"][name[:-1]], values[name[:-1]]))
            text = str(wanted)
            if (count < int(text.rstrip("+"))) if text.endswith("+") else (count != int(text)):
                return False
            continue
        spec = data["options"][name]
        have = selected(spec, values[name])
        for want in wanted if isinstance(wanted, list) else [wanted]:
            negate = want.startswith("!")
            hit = want.lstrip("!") in have
            if hit != negate:
                break
        else:
            return False
    return True


def hotkey_text(label: str, values: dict[str, str]) -> str:
    if values.get("hotkeys") == "off":
        return HOTKEY_FALLBACK
    from tools import hotkey_defaults

    try:
        return hotkey_defaults.hint_line(label)
    except (KeyError, ValueError) as error:
        raise ScenarioError(f"no hotkey chord for label {label!r}") from error


class _Strict(dict):
    def __missing__(self, key: str) -> str:
        raise ScenarioError(f"unknown placeholder {{{key}}}")


def fill(text: str, values: dict[str, str], extra: dict[str, str] | None = None) -> str:
    context = _Strict({**values, **(extra or {})})
    text = HOTKEY_RE.sub(
        lambda match: hotkey_text(match.group(1), values).replace("{", "{{").replace("}", "}}"),
        text,
    )
    try:
        return string.Formatter().vformat(text, (), context)
    except ScenarioError:
        raise
    except (ValueError, IndexError, AttributeError) as error:
        raise ScenarioError(f"bad placeholder syntax in {text!r}: {error}") from error


def entry_names() -> dict[int, str]:
    from tools import button_dump_poke

    return button_dump_poke.load_names()


# --------------------------------------------------------------------------------------------- expansion
def repeat_rows(item: dict, data: dict, values: dict[str, str]) -> Iterator[dict[str, str]]:
    over = item["repeat"]["over"]
    spec = data["options"][over]
    table = data.get("tables", {}).get(item["repeat"].get("table", ""), {})
    names = entry_names() if "entries" in spec else {}
    if item["repeat"].get("onwards"):
        keys = list(table)
        if values[over] not in keys:
            raise ScenarioError(
                f"repeat onwards: {values[over]!r} is not a key of table {item['repeat'].get('table')!r} ({keys})"
            )
        tokens = keys[keys.index(values[over]) :]
    else:
        tokens = selected(spec, values[over])
    for index, token in enumerate(tokens, start=1):
        row: dict = {"item": token, "n": str(index), "index": str(index)}
        if "entries" in spec:
            entry = parse_entry(token)
            row |= {
                "entry": f"0x{entry:02X}",
                "name": names.get(entry, "(unnamed)"),
                "spawner": str(index),
            }
        source = table.get(token) or table.get(f"#{index}") or {}
        for key, value in source.items():
            if key not in ROW_LISTS and not isinstance(value, (list, dict)):
                row[key] = str(value)
        row["_steps"] = [
            *source.get("steps", []),
            *item_lines(source, values, row),
            *source.get("steps_after", []),
        ]
        yield row


def item_lines(source: dict, values: dict[str, str], row: dict) -> list[str]:
    """One filled line per `items` entry of a table row (template `item_step`), braces escaped for the later fill."""
    template = source.get("item_step")
    if not template:
        return []
    lines = []
    for slot, entry in enumerate(source.get("items", []), start=1):
        context = {**row, **{key: str(value) for key, value in entry.items()}, "slot": str(slot)}
        lines.append(fill(template, values, context).replace("{", "{{").replace("}", "}}"))
    return lines


def phase_step(entry: dict, data: dict) -> dict:
    """A STEP that names a phase: its text and sub steps are built from the phase fields, one source for banner and prompt."""
    key = entry["phase"]
    phase = data["phases"][key]
    head = f"Phase {key.replace(':', '@')}" + (f": {phase['title']}" if phase.get("title") else "")
    if entry.get("text"):
        head = f"{entry['text']} {head}"
    subs = [f"Be at: {phase['go_to']}"]
    if phase.get("switch"):
        subs.append(f"Switch to: {phase['switch']}")
    subs.append(f"Do: {phase['do']}")
    if phase.get("note"):
        subs.append(f"Then note: {phase['note']}")
    return {
        **{k: v for k, v in entry.items() if k not in ("phase", "text", "substeps")},
        "text": head,
        "seconds": entry.get("seconds") or phase.get("seconds"),
        "substeps": [*subs, *entry.get("substeps", [])],
    }


def expand(
    items: list, data: dict, values: dict[str, str], extra: dict[str, str] | None = None
) -> list[dict]:
    """Nodes {text, seconds, subs} of a list of LINEs/STEPs after `when`, `repeat` and placeholder filling."""
    nodes: list[dict] = []
    for item in items:
        entry = {"text": item} if isinstance(item, str) else item
        if not condition_holds(entry.get("when"), data, values):
            continue
        local = extra
        if "phase" in entry:
            local = {**(extra or {}), "label": entry["phase"].partition(":")[2]}
            entry = phase_step(entry, data)
        if "repeat" in entry:
            for row in repeat_rows(entry, data, values):
                row_steps = row.pop("_steps")
                context = {**(local or {}), **row}
                subs = [*entry.get("substeps", []), *row_steps]
                nodes.append(
                    {
                        "text": fill(entry["text"], values, context),
                        "seconds": entry.get("seconds"),
                        "subs": [node["text"] for node in expand(subs, data, values, context)],
                    }
                )
            continue
        nodes.append(
            {
                "text": fill(entry["text"], values, local),
                "seconds": entry.get("seconds"),
                "subs": [
                    node["text"] for node in expand(entry.get("substeps", []), data, values, local)
                ],
            }
        )
    return nodes


# --------------------------------------------------------------------------------------------- rendering
def wrap(text: str, indent: str, hanging: str, width: int) -> list[str]:
    return textwrap.wrap(
        text,
        width=width,
        initial_indent=indent,
        subsequent_indent=hanging,
        break_long_words=False,
        break_on_hyphens=False,
    ) or [indent]


def render(
    script: str,
    given: dict[str, str] | None = None,
    width: int = DEFAULT_WIDTH,
    root: Path | None = None,
) -> list[str]:
    """The owner facing text of one script run, as lines."""
    data = load_script(script, root)
    values = resolve_options(data, given)
    banner = f" WHAT YOU NEED TO DO: {fill(data['title'], values)} "
    lines = ["", banner.center(min(width, 100), "=")]
    lines += wrap(fill(data["summary"], values), "", "", width)
    if data.get("duration"):
        lines += wrap(f"Time needed: {fill(data['duration'], values)}", "", "  ", width)
    chosen = {name: value for name, value in values.items() if name in (given or {})}
    if chosen:
        lines += wrap(
            "This run: " + ", ".join(f"{name}={value}" for name, value in sorted(chosen.items())),
            "",
            "  ",
            width,
        )
    for section, heading in SECTIONS:
        nodes = expand(data.get(section, []), data, values)
        if not nodes:
            continue
        lines += ["", f"{heading}:"]
        for number, node in enumerate(nodes, start=1):
            marker = f"{number:>2}. " if section == "steps" else "  - "
            text = node["text"] + (f"   [about {node['seconds']} s]" if node["seconds"] else "")
            lines += wrap(text, marker, " " * len(marker), width)
            for sub_index, sub in enumerate(node["subs"]):
                letter = string.ascii_lowercase[sub_index % 26]
                lines += wrap(sub, f"      {letter}) ", " " * 10, width)
    lines += ["", "=" * min(width, 100), ""]
    return lines


def phase_text(
    script: str,
    scenario_id: str,
    label: str | None,
    given: dict[str, str] | None = None,
    root: Path | None = None,
) -> dict | None:
    """The prompt fields of one census phase, or None when the scenario has no text for it.

    Looked up as `id:label`, then `id` (a script wide text for every label of that phase id, it may use `{label}`).
    """
    data = load_script(script, root)
    phases = data.get("phases", {})
    phase = phases.get(f"{scenario_id}:{label}") if label else None
    if phase is None:
        phase = phases.get(scenario_id)
    if phase is None:
        return None
    values = resolve_options(data, given)
    extra = {"label": label or ""}
    return {
        "title": fill(phase.get("title", data["title"]), values, extra),
        "starts_at": fill(phase["go_to"], values, extra),
        "instruction": fill(phase["do"], values, extra),
        "seconds": phase.get("seconds"),
        "switch": fill(phase["switch"], values, extra) if phase.get("switch") else None,
        "note": fill(phase["note"], values, extra) if phase.get("note") else None,
    }


def phase_keys(script: str, root: Path | None = None) -> list[str]:
    return list(load_script(script, root).get("phases", {}))


def table_rows(
    script: str, table: str, key: str, path: str, fields: list[str], root: Path | None = None
) -> list[list[str]]:
    """Rows of data[tables][table][key][path] as lists of the named fields (for the fish scripts, one source of truth)."""
    data = load_script(script, root)
    try:
        rows = data["tables"][table][key][path]
    except (KeyError, TypeError) as error:
        raise ScenarioError(f"{script}: no tables.{table}.{key}.{path}") from error
    return [[str(row[field]) for field in fields] for row in rows]


def table_field(script: str, table: str, key: str, field: str, root: Path | None = None) -> str:
    """One text field of tables[table][key] (for the fish scripts, for example the title of a batch)."""
    data = load_script(script, root)
    try:
        return str(data["tables"][table][key][field])
    except (KeyError, TypeError) as error:
        raise ScenarioError(f"{script}: no tables.{table}.{key}.{field}") from error


def option_combinations(data: dict, limit: int = 64) -> Iterator[dict[str, str]]:
    """Option assignments for tests: every enum value (and each single value of a multi option), capped."""
    choices: list[list[tuple[str, str]]] = []
    for name, spec in data.get("options", {}).items():
        if "values" in spec:
            choices.append([(name, value) for value in spec["values"]])
        else:
            choices.append([(name, str(spec["default"]))])
    for count, combo in enumerate(itertools.product(*choices) if choices else [()]):
        if count >= limit:
            return
        yield dict(combo)


# --------------------------------------------------------------------------------------------- command line
def parse_opts(items: list[str]) -> dict[str, str]:
    opts: dict[str, str] = {}
    for item in items:
        name, sep, value = item.partition("=")
        if not sep or not OPTION_RE.fullmatch(name):
            raise ScenarioError(f"bad --opt {item!r}, want KEY=VALUE")
        opts[name] = value
    return opts


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.owner_scenario",
        description="T1767: the script specific 'what you need to do' text of the owner session scripts (tools/data/owner_scenarios/).",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    show = commands.add_parser("show", help="print the scenario of one script")
    show.add_argument("script")
    show.add_argument(
        "--opt",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="a declared option of the scenario (repeatable)",
    )
    show.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    commands.add_parser("list", help="every scenario: script, kind, title")
    check = commands.add_parser(
        "check", help="validate every scenario file and render all option combinations"
    )
    check.add_argument(
        "--scripts-dir",
        type=Path,
        help="also require a scenario for each *.fish in this directory (the owner's tmp/)",
    )
    phases = commands.add_parser("phases", help="the phase keys a scenario has text for")
    phases.add_argument("script")
    rows = commands.add_parser(
        "rows", help="rows of tables.TABLE.KEY.PATH, one per line, fields joined by a colon"
    )
    rows.add_argument("script")
    rows.add_argument("table")
    rows.add_argument("key")
    rows.add_argument("--path", default="items")
    rows.add_argument("--fields", default="label,entry")
    field = commands.add_parser(
        "field", help="one text field of tables.TABLE.KEY (for example the title of a batch)"
    )
    field.add_argument("script")
    field.add_argument("table")
    field.add_argument("key")
    field.add_argument("name")
    return parser


def check_all(scripts_dir: Path | None, out: Callable[[str], None] = print) -> int:
    failures = 0
    names = available_scripts()
    for script in OWNER_SCRIPTS:
        if script not in names:
            out(f"MISSING scenario: {script}")
            failures += 1
    if scripts_dir is not None:
        for fish in sorted(scripts_dir.glob("*.fish")):
            if fish.stem not in names:
                out(f"MISSING scenario for owner script {fish.name}")
                failures += 1
    for script in names:
        try:
            data = load_script(script)
            for combo in option_combinations(data):
                render(script, combo)
        except ScenarioError as error:
            out(f"BAD {script}: {error}")
            failures += 1
    out(f"{len(names)} scenario files, {failures} problem(s)")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "show":
            print("\n".join(render(args.script, parse_opts(args.opt), args.width)))
        elif args.command == "list":
            for script in available_scripts():
                data = load_script(script)
                print(f"{script:26s} {data['kind']:15s} {data['title']}")
        elif args.command == "check":
            return check_all(args.scripts_dir)
        elif args.command == "phases":
            print("\n".join(phase_keys(args.script)))
        elif args.command == "rows":
            fields = args.fields.split(",")
            for row in table_rows(args.script, args.table, args.key, args.path, fields):
                print(":".join(row))
        elif args.command == "field":
            print(table_field(args.script, args.table, args.key, args.name))
    except ScenarioError as error:
        print(f"owner_scenario: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
