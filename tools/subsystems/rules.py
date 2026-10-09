# SPDX-License-Identifier: GPL-3.0-or-later
"""The rule table that places a function in a subsystem (tree path), conservatively.

Everything here is explicit data plus two small pure functions, `classify_game` and
`classify_library`. A function no rule justifies is `unknown`. Nothing is guessed.

GAME NAMES. A name is split into lower case tokens, a leading `game` token is dropped, and
the ordered `RULES` table is searched top to bottom: the first rule whose `tokens` are a
prefix of the name tokens (and whose `requires_any` tokens, when given, appear later in the
name) wins. A rule target of `None` is an explicit UNKNOWN (a family known to be ambiguous).

* GENERIC GUARD. A first token in `GENERIC_PREFIXES` (the vocabulary of
  `tools.coverage_by_subsystem`: table, get, set, global, static, record, slot, entry,
  event ...) is never classified by prefix. It is checked BEFORE the rule table, so a rule
  can not override it (the tests also forbid a rule starting with a generic token). Only an
  anchor (`anchors.py`) or library evidence can place such a function.
* KEYWORDS. The function_names.csv evidence text is searched for the few phrases in
  `KEYWORDS`. They never place a function on their own (evidence text mentions callees and
  doc names, which is noisy). They only (1) corroborate a prefix rule, (2) refine
  `network/session` to `network/syslink` when the text says system link or LAN, and
  (3) turn the row UNKNOWN (conflict) when the keyword names a different TOP LEVEL umbrella
  than the prefix rule. Siblings below one umbrella (for example ui/hud versus ui/menu) are
  not a conflict. The system link keyword is refine-only (`REFINE_ONLY`): a menu page that
  mentions system link stays a menu page, it never conflicts.
* CONFIDENCE. A rule gives INFERRED. MEASURED is possible only through `name_measured`
  (the function_names.csv confidence column of the row was MEASURED) or through an anchor.
  `classify_game` never returns MEASURED otherwise.

LIBRARY. `classify_library` maps the evidence that made a function library code to
library/crt, library/xdk or library/math. MEASURED needs a signature match (.XTLID or
FLIRT) or a recorded classification row with a reason (`tools/data/function_classification.csv`).
Section membership and an existing name alone are INFERRED.
"""

from __future__ import annotations

import re
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass

from tools.coverage import (
    EVIDENCE_FLIRT,
    EVIDENCE_NAMED,
    EVIDENCE_OVERRIDE,
    EVIDENCE_SECTION,
    EVIDENCE_XTLID,
    is_placeholder_name,
)
from tools.coverage_by_subsystem import GENERIC_PREFIXES
from tools.subsystems.tree import UNKNOWN_PATH

MEASURED = "MEASURED"
INFERRED = "INFERRED"
UNKNOWN = "UNKNOWN"
CONFIDENCES = (MEASURED, INFERRED, UNKNOWN)

#: Token that matches an address-looking token: `0x51`, `78b594`, `75f5e8` (needs a digit).
HEX_TOKEN = "#hex"
_HEX_RE = re.compile(r"^(?:0x[0-9a-f]+|(?=[0-9a-f]*\d)[0-9a-f]{5,8})$")

GROUP_BASE = "base"
GROUP_BOUNDARY = "boundary"


@dataclass(frozen=True)
class Rule:
    """One placement rule.

    `tokens`: prefix of the name tokens. A token is an exact word, `#hex` (an address-looking
    token) or `word*` (starts with). `requires_any`: at least one of these phrases (underscore
    joined word runs) occurs in the rest of the name. `excludes_any`: none of these phrases does.
    `va_range`: half open address range the function must lie in. `target` None is an explicit
    UNKNOWN. `group` tags where the rule came from, for the hit statistics.
    """

    tokens: tuple[str, ...]
    target: str | None
    why: str = ""
    requires_any: tuple[str, ...] = ()
    excludes_any: tuple[str, ...] = ()
    va_range: tuple[int, int] | None = None
    group: str = GROUP_BASE

    @property
    def label(self) -> str:
        text = "_".join(self.tokens) or "*"
        if self.requires_any:
            text += "[" + "|".join(self.requires_any) + "]"
        if self.excludes_any:
            text += "[!" + "|".join(self.excludes_any) + "]"
        if self.va_range:
            text += f"@{self.va_range[0]:x}-{self.va_range[1]:x}"
        return text


def rule(
    spec: str,
    target: str | None,
    *requires: str,
    why: str = "",
    excludes: tuple[str, ...] = (),
    va_range: tuple[int, int] | None = None,
    group: str = GROUP_BASE,
) -> Rule:
    tokens = tuple(spec.split("_")) if spec else ()
    return Rule(tokens, target, why, tuple(requires), excludes, va_range, group)


def family(target: str | None, prefixes: str, **kwargs: object) -> list[Rule]:
    """One rule per space separated prefix, all with the same target."""
    return [rule(prefix, target, **kwargs) for prefix in prefixes.split()]  # type: ignore[arg-type]


def boundary(spec: str, target: str | None, *requires: str, **kwargs: object) -> Rule:
    """A rule from the decomp-tree boundary decisions (supervisor, 2026-10-09)."""
    return rule(spec, target, *requires, group=GROUP_BOUNDARY, **kwargs)  # type: ignore[arg-type]


#: Checked BEFORE the generic guard: the few decided cases whose first token is a generic
#: prefix (`event`, `wrapper`) and the name-wide `stub_noop` pattern. Everything else generic
#: stays UNKNOWN.
GENERIC_OVERRIDES: tuple[Rule, ...] = (
    boundary("", "dev", "stub_noop", why="no-op debug stub"),
    boundary("event", "data/missions", why="game events are mission/script events"),
    boundary("wrapper_7d390*", "ui/framework", why="wrappers of the widget helper 0x7d390"),
)

_NET_DISCOVERY = (0x2D980, 0x2F400)
_AUDIO_BOUNDARY = (0x385000, 0x390000)
_MAPEDIT_STORAGE = ("serialize", "deserialize", "anicemap")
#: Narrower than the boundary list (kind/palette/weapon_set/tileset/brick/prop/spawn): a spot check
#: of 14 `kind` and `palette` hits found every one was editor record plumbing (record kinds, colour
#: palettes), so those two tokens stay in mapmaker/editor.
_MAPEDIT_CONTENT = ("weapon_set", "tileset", "brick", "bricks", "prop", "props", "spawn", "theme")

# T1704 finite reviewed exceptions: names retain historical HUD/UI prefixes.
# Do not sweep unreviewed future members. Evidence: docs/t-ui-framework-membership.md.
_UI_SHARED_TEXT = (
    0x3F8D0,
    0x3FAA0,
    0x3FCF0,
    0x403D0,
    0x405C0,
    0x405F0,
    0x40620,
    0x40700,
    0x40740,
    0x407B0,
    0x40970,
    0x409D0,
    0x40A40,
    0x40AD0,
    0x40B60,
    0x40C10,
    0x40F00,
    0x40F30,
    0x40FA0,
    0x41CD0,
    0x41D20,
    0x41DC0,
    0x41E30,
    0x42010,
    0x42130,
    0x42190,
    0x42320,
    0x42420,
    0x42490,
)
_UI_MENU_DRAWS = (
    0x1958B0,
    0x195940,
    0x2CE580,
    0x2CE710,
    0x2CEE60,
    0x2D3D30,
    0x2D4360,
    0x2E9590,
    0x2E97C0,
)
_UI_EDITOR_DRAWS = (
    0x30C440,
    0x30CB30,
    0x3246A0,
    0x3266B0,
    0x3267C0,
    0x3268C0,
    0x326990,
    0x3276F0,
)

#: Ordered. A more specific rule (more tokens, or `requires_any`, or `va_range`) MUST precede
#: the plainer rule of the same first token. `tests/test_subsystems_rules.py` enforces this.
RULES: tuple[Rule, ...] = (
    # --- explicit UNKNOWN: families whose first token does not mean what it says ------------
    rule("mode_#hex", None, why="object class or global block id, not a game mode"),
    boundary("input_driven", None, why="boundary: driven input, owner not decided"),
    boundary(
        "audio_iface",
        "library/xdk",
        va_range=_AUDIO_BOUNDARY,
        why="T1729 XVoice full-body matches; docs/t1729-audio-boundary.md",
    ),
    boundary(
        "audio_iface",
        "library/xdk",
        va_range=(0x3847D2, 0x3847D3),
        why="T1729 XAPI shutdown notification full-body match",
    ),
    boundary(
        "audio_decoder",
        "library/xdk",
        va_range=_AUDIO_BOUNDARY,
        why="T1729 XVoice codec full-body matches; docs/t1729-audio-boundary.md",
    ),
    boundary(
        "scene_object",
        None,
        why="unreviewed object behaviour; finite T1723 anchors decide reviewed members",
    ),
    boundary("scalar_deleting", None, why="C++ scalar deleting destructor, not math"),
    boundary("scalar_object", None, why="object lookup, not math"),
    # --- mapmaker (boundary: storage, modes, ui, content, else editor) ------------------------
    boundary("mapedit", "mapmaker/storage", *_MAPEDIT_STORAGE),
    boundary("mapedit_save", "mapmaker/storage", excludes=("view", "camera", "scale")),
    boundary("mapedit", "mapmaker/modes", "assault", "story", "validation"),
    boundary("mapedit_draw", "mapmaker/ui"),
    boundary("mapedit_build", "mapmaker/ui", "page", "widget"),
    boundary("mapedit_format", "mapmaker/ui", "message_text"),
    boundary("mapedit", "mapmaker/content", *_MAPEDIT_CONTENT),
    rule("mapedit", "mapmaker/editor"),
    boundary("mapmaker", "mapmaker/storage", why="mapmaker_* is save and label handling"),
    # --- network -----------------------------------------------------------------------------
    boundary("menu_online", "network/live"),
    boundary("menu_net", "network/live"),
    boundary("menu_messenger", "network/live"),
    boundary("menu_clan", "network/live"),
    boundary("fesl_game_list", "network/syslink", why="LAN/UDP game list"),
    *[
        boundary(f"fesl_{w}", "network/syslink", why="LAN/UDP probe, ping, qos or peer")
        for w in ("udp", "ping", "qos", "peer")
    ],
    boundary("net", "network/syslink", va_range=_NET_DISCOVERY, why="LAN/UDP discovery sockets"),
    *family("network/live", "fesl online messenger clan xbl xlive"),
    *family("network/session", "net session lobby packet"),
    # --- ui ----------------------------------------------------------------------------------
    *[
        boundary(
            "ui_widget",
            path,
            va_range=(va, va + 1),
            why="T1704 screen state callback; docs/t-ui-framework-membership.md",
        )
        for va, path in (
            (0x2D85D0, "ui/menu"),
            (0x2E7E50, "ui/settings"),
            (0x3192E0, "mapmaker/ui"),
            (0x324C00, "mapmaker/ui"),
        )
    ],
    *[
        boundary(
            "ui_draw",
            path,
            va_range=(va, va + 1),
            why="T1704 screen-owned draw; docs/t-ui-framework-membership.md",
        )
        for addresses, path in ((_UI_MENU_DRAWS, "ui/menu"), (_UI_EDITOR_DRAWS, "mapmaker/ui"))
        for va in addresses
    ],
    *[
        boundary(
            "hud_text",
            "ui/framework",
            va_range=(va, va + 1),
            why="T1704 shared font layout/render; docs/t-ui-framework-membership.md",
        )
        for va in _UI_SHARED_TEXT
    ],
    *[
        boundary(
            "text",
            "ui/framework",
            va_range=(va, va + 1),
            why="T1704 shared font state; docs/t-ui-framework-membership.md",
        )
        for va in (0x3F9C0, 0x3F9F0, 0x3FA10)
    ],
    boundary("menu_audio", "ui/settings"),
    boundary("menu_settings", "ui/settings"),
    boundary("menu_build_controls", "ui/settings"),
    boundary("menu_build_settings", "ui/settings", why="the settings page builders"),
    boundary("player_options_menu", "ui/settings"),
    boundary("options_menu", "ui/settings"),
    boundary("ui_build_player_options_menu", "ui/settings"),
    boundary("input_mapping_menu", "ui/settings"),
    boundary("save_menu", "ui/menu"),
    boundary("save_screen", "ui/menu"),
    boundary("ui_curved_menu", "ui/menu"),
    boundary("ui_widget", "ui/framework"),
    boundary("ui_text", "ui/framework"),
    boundary("ui_draw", "ui/framework"),
    *family("ui/hud", "hud scoreboard radar crosshair"),
    rule("ingame_menu", "ui/menu", why="pause / in-game menu screens"),
    *family("ui/menu", "menu frontend attract credits"),
    *family("ui/framework", "ui widget popup overlay loadbar font dialog"),
    # --- engine ------------------------------------------------------------------------------
    boundary("level_room", "engine/render/scene"),
    *family("engine/audio", "audio sound music dsp codec sfx crossfade dsound voice"),
    *family("engine/video", "xmv movie"),
    boundary("draw", None, why="mixed draw family; finite T1722 anchors decide reviewed members"),
    *family("engine/render/d3d", "d3d viewport"),
    # T1723: draw/update/render names mix state callbacks, effects and HUD.
    # Only the finite reviewed address anchors place scene members.
    boundary("scene", None, why="mixed scene family; reviewed T1723 members have address anchors"),
    *family("engine/render/scene", "camera model texture mesh light lighting sky"),
    *family("engine/render", "render"),
    *family("engine/render/effects", "effect particle weather decal clouds post fx explosion"),
    *family("engine/physics", "collision physics sweep raycast constraint"),
    *family("engine/anim", "anim skeleton bone"),
    boundary("pool_6b83c8", "engine/memory"),
    *family("engine/object", "object objects component pool entity"),
    boundary("memory_unit", "data/save"),
    *family("engine/input", "input pad controller"),
    *family("engine/core", "launch init boot"),
    *family("engine/memory", "alloc memory heap arena"),
    *family("engine/math", "math matrix matrix4x4 vec vec3 vector quat scalar float"),
    # --- ai (boundary: pathfinding, then behaviour, then characters) ---------------------------
    boundary("ai_nav", "ai/pathfinding"),
    *[
        boundary(first, "ai/pathfinding", "nav_node", "route_step")
        for first in ("ai", "char", "actor")
    ],
    boundary("char_class", "ai/behavior"),
    *[boundary(first, "ai/behavior", "alert", "target_select") for first in ("char", "actor")],
    rule("ai", "ai/behavior"),
    *family("ai/characters", "char actor npc"),
    rule("pathfind", "ai/pathfinding"),
    # --- gameplay ----------------------------------------------------------------------------
    rule("weapon", "gameplay/inventory", "selectable", "owned", "inventory", "grant", "loadout"),
    rule("player_loadout", "gameplay/inventory"),
    rule("player_weapon", "gameplay/weapons", why="the name says weapon"),
    *family("gameplay/player", "player players"),
    *family("gameplay/weapons", "weapon hit pickup ammo damage nuke projectile bullet grenade"),
    rule("vehicle", "gameplay/vehicles"),
    *family("gameplay/modes", "mode match results ingame"),
    # --- data --------------------------------------------------------------------------------
    boundary("level_xbr", "data/assets"),
    boundary("level_pak", "data/assets"),
    *family("data/missions", "level script scripted terminal enter room sequence"),
    rule("cutscene", "data/cutscenes"),
    *family("data/text", "text string"),
    *family("data/assets", "pak file load preload"),
    *family("data/save", "save profile unlock"),
    # --- library code that lives in the game region as wrappers or copies --------------------
    boundary("x87", "library/math", why="CRT x87 helpers (3 rows named without game_)"),
    *family("library/crt", "crt eh"),
    # --- dev ---------------------------------------------------------------------------------
    rule("debug", "dev"),
)

#: The `crt`, `eh`, `x87` families map GAME region functions (`game_crt_*`) to library paths.
#: They are game side wrappers or copies, not the library code itself, so they stay INFERRED
#: and the evidence text says so.
GAME_LIBRARY_NOTE = "game side wrapper or copy, not the library code itself"

#: Phrases searched (regex, case sensitive) in the function_names.csv evidence text.
#: (label, regex, target path)
KEYWORDS: tuple[tuple[str, str, str], ...] = (
    ("Map Maker", r"\bMap ?Maker\b", "mapmaker"),
    ("HUD", r"\bHUD\b", "ui/hud"),
    ("scoreboard", r"\bscoreboard\b", "ui/hud"),
    ("Controls", r"\bCONTROLS\b|\bControls\b", "ui/settings"),
    ("system link", r"\b[Ss]ystem [Ll]ink\b|\bLAN\b", "network/syslink"),
)

#: Keyword targets that can only refine a prefix rule, never conflict with one.
REFINE_ONLY: frozenset[str] = frozenset(("network/syslink",))

#: (prefix rule target, keyword target) -> refined target. Only these refinements exist.
REFINEMENTS: Mapping[tuple[str, str], str] = {
    ("network/session", "network/syslink"): "network/syslink",
}


@dataclass(frozen=True)
class Decision:
    subsystem: str
    confidence: str
    evidence: str
    #: Which rule decided (for statistics only, not written to the table).
    rule: str = ""
    group: str = ""

    @property
    def classified(self) -> bool:
        return self.subsystem != UNKNOWN_PATH


def unknown(evidence: str, rule_label: str = "", group: str = "") -> Decision:
    return Decision(UNKNOWN_PATH, UNKNOWN, evidence, rule_label, group)


def name_tokens(name: str) -> tuple[str, ...]:
    """Lower case tokens of a name without the leading `game` token."""
    tokens = tuple(name.lower().split("_"))
    return tokens[1:] if tokens and tokens[0] == "game" and len(tokens) > 1 else tokens


def _token_matches(pattern: str, token: str) -> bool:
    if pattern == HEX_TOKEN:
        return bool(_HEX_RE.match(token))
    if pattern.endswith("*"):
        return token.startswith(pattern[:-1])
    return pattern == token


def _has_phrase(tokens: Sequence[str], phrases: Iterable[str]) -> bool:
    haystack = "_" + "_".join(tokens) + "_"
    return any(f"_{phrase}_" in haystack for phrase in phrases)


def rule_matches(candidate: Rule, tokens: Sequence[str], va: int | None = None) -> bool:
    size = len(candidate.tokens)
    if len(tokens) < size:
        return False
    if candidate.va_range is not None:
        if va is None or not candidate.va_range[0] <= va < candidate.va_range[1]:
            return False
    if not all(_token_matches(p, t) for p, t in zip(candidate.tokens, tokens, strict=False)):
        return False
    rest = tokens[size:]
    if candidate.requires_any and not _has_phrase(rest, candidate.requires_any):
        return False
    return not (candidate.excludes_any and _has_phrase(rest, candidate.excludes_any))


def first_rule(
    tokens: Sequence[str], rules: Iterable[Rule] = RULES, va: int | None = None
) -> Rule | None:
    for candidate in rules:
        if rule_matches(candidate, tokens, va):
            return candidate
    return None


def top_level(path: str) -> str:
    return path.split("/", 1)[0]


def keyword_hits(
    evidence_text: str, keywords: Iterable[tuple[str, str, str]] = KEYWORDS
) -> tuple[tuple[str, str], ...]:
    """(label, target) of every keyword found in the text, in table order."""
    return tuple(
        (label, target) for label, pattern, target in keywords if re.search(pattern, evidence_text)
    )


def classify_game(
    name: str,
    *,
    evidence_text: str = "",
    name_measured: bool = False,
    va: int | None = None,
    rules: Iterable[Rule] = RULES,
    overrides: Iterable[Rule] = GENERIC_OVERRIDES,
    generic: frozenset[str] = GENERIC_PREFIXES,
    keywords: Iterable[tuple[str, str, str]] = KEYWORDS,
    refinements: Mapping[tuple[str, str], str] = REFINEMENTS,
    refine_only: frozenset[str] = REFINE_ONLY,
) -> Decision:
    """Place one game function by name and name evidence. See the module docstring."""
    if is_placeholder_name(name):
        return unknown("unnamed placeholder", "placeholder")
    tokens = name_tokens(name)
    if not tokens or not tokens[0]:
        return unknown("empty name", "empty")
    matched = first_rule(tokens, overrides, va)
    if matched is None:
        if tokens[0] in generic:
            return unknown(f"generic prefix {tokens[0]}", "generic")
        matched = first_rule(tokens, rules, va)
    if matched is None:
        return unknown(f"prefix {tokens[0]} not in rule table", "no-rule")
    if matched.target is None:
        return unknown(
            f"rule {matched.label}: {matched.why}".rstrip(": "), matched.label, matched.group
        )
    target = matched.target
    notes: list[str] = []
    for label, keyword_target in keyword_hits(evidence_text, keywords):
        refined = refinements.get((target, keyword_target))
        if refined is not None:
            target = refined
            notes.append(f"keyword {label} refines to {refined}")
        elif keyword_target in refine_only:
            notes.append(f"keyword {label} noted, refines nothing here")
        elif top_level(keyword_target) != top_level(target):
            return unknown(
                f"conflict: prefix {matched.label} -> {matched.target} vs "
                f'keyword "{label}" -> {keyword_target}',
                "conflict",
                matched.group,
            )
        else:
            notes.append(f"keyword {label} agrees at umbrella {top_level(target)}")
    reason = f"prefix {matched.label}"
    if matched.why:
        reason += f" ({matched.why})"
    if matched.target.startswith("library/"):
        notes.append(GAME_LIBRARY_NOTE)
    if name_measured:
        reason = f"name MEASURED, {reason}"
    evidence = "; ".join([reason, *notes])
    return Decision(
        target, MEASURED if name_measured else INFERRED, evidence, matched.label, matched.group
    )


# --------------------------------------------------------------------------- #
# Library code.
# --------------------------------------------------------------------------- #

LIB_CRT = "library/crt"
LIB_XDK = "library/xdk"
LIB_MATH = "library/math"

#: Floating point support names of the CRT (`__fload`, `__87except`, `__ctrandisp2` ...).
_MATH_RE = re.compile(
    r"^(?:87except|CopyMan|IncMan|IsZeroMan|RoundMan|ShrMan|ZeroTail|abstract_cw|clrfp|"
    r"control87|controlfp|ctrlfp|c?intrindisp\d|c?trandisp\d|fload(?:_withFB)?|fpclass|fpmath|"
    r"fptrap|frnd|handle_qnan1|hw_cw|math_exit|powhlp|set_exp|set_statfp|setdefaultprecision|"
    r"startOneArgErrorHandling|statfp|umatherr|ceil_default|floor_default|ftol2?(?:_sse)?|"
    r"CI(?:sin|cos|tan|atan|atan2|exp|log|log10|pow|sqrt|fmod)|sin|cos|tan|atan2?|asin|acos|exp|"
    r"log10?|pow|sqrt|floor|ceil|fmod|fabs|modf|frexp|ldexp)$"
)
_XDK_RE = re.compile(
    r"^(?:X[A-Za-z]*[A-Z0-9_]|XDEVICE|D3D|IDirect3D|CDirectSound|IDirectSound|DirectSound|"
    r"DSound|CMcpx|CDevice|CMiniport|CEngine|WSA|Rtl|Dbg|Ex[A-Z]|Nt[A-Z]|Hal[A-Z]|Mm[A-Z]|"
    r"Ke[A-Z]|Io[A-Z]|Ob[A-Z]|Ps[A-Z]|Cc[A-Z]|"
    r"(?:Create|Delete|Find|Get|Set|Open|Close|Wait|Release|Switch|Suspend|Resume|Is|Output|"
    r"Query|Read|Write|Remove|Flush|Copy|Move|Lock|Unlock|Sleep|Terminate|Duplicate|Compare|"
    r"Unmount|Mount)[A-Z]\w*|"
    r"lstr\w+|FileTime\w*|inet_\w+|ntoh[sl]|hton[sl]|socket|bind|connect|send|sendto|recv|"
    r"recvfrom|listen|accept|closesocket|ioctlsocket|select|shutdown|getsockname|"
    r"getpeername|setsockopt|getsockopt|gethostbyname|__WSAFDIsSet|GetTypeInformation\w*|"
    r"BuildUpdateFilePath|HexDigitToChar|UTF8ToUnicode|UnicodeToUTF8|ValidateRead|"
    r"ValidateWrite|xCompareStringA|FID_\w+)"
)
_CRT_UPPER_RE = re.compile(
    r"^(?:\$I10_OUTPUT|SEH_\w+|NLG_\w+|FF_MSGBANNER|NMSG_WRITE|CxxThrowException|"
    r"CxxFrameHandler|InternalCxxFrameHandler|DestructExceptionObject|FrameUnwindToState|"
    r"CallSettingFrame)$"
)
_EH_MANGLE_RE = re.compile(r"^\?")
_XDK_MANGLE_RE = re.compile(r"@(?:D3D|XACT|DirectSound|XTemplate)@|XTemplate@|@Math@DirectSound@")
_SECTION_XDK = frozenset(("D3D", "XGRPH", "DSOUND", "XONLINE", "XNET", "XMV", "XPP", "XVOICE"))
#: Reason texts name a library either by section tag (`CRT`, `XGRPH`...) or, in the T1641 libsig
#: rows, by the lower case stem of the linked library file (`libc`, `xvoice`, `xapilib`...).
_REASON_CRT_RE = re.compile(r"\bCRT\b|\b(?:libc|libcmt)\b")
_REASON_XDK_RE = re.compile(
    r"\b(?:XGRPH|D3D|DSOUND|XNET|XONLINE|XMV|XPP|XVOICE|XDK)\b"
    r"|\b(?:xapilib|d3d8|xgraphics|dsound|xonlines|xvoice|xmv)\b"
)


def normalise_library_name(name: str) -> str:
    """Strip `FID_conflict:`, `thunk_`, leading underscores and a trailing `@N`."""
    text = name.removeprefix("FID_conflict:").removeprefix("thunk_")
    if text.startswith("?"):
        return text
    text = text.lstrip("_")
    return re.sub(r"@\d+$", "", text)


def library_target_from_name(name: str) -> str | None:
    """library/crt, library/xdk or library/math from a recovered name, or None."""
    if is_placeholder_name(name):
        return None
    core = normalise_library_name(name)
    if core.startswith("lib_crt_"):
        return LIB_CRT  # T1641 renamed game_crt_* (crt prefix rule) to lib_crt_*
    if core.startswith("lib_x87"):
        return LIB_MATH  # and x87_* (x87 boundary rule) to lib_x87_*
    if core.startswith("lib_"):
        return None  # generic repo-assigned names (lib_eh_unwind_funclet_*) say nothing
    if core.startswith("?"):
        return LIB_XDK if _XDK_MANGLE_RE.search(core) else LIB_CRT
    if _MATH_RE.match(core):
        return LIB_MATH
    if _CRT_UPPER_RE.match(core):
        return LIB_CRT
    if _XDK_RE.match(core):
        return LIB_XDK
    if core[:1].islower() or core[:1].isdigit():
        return LIB_CRT
    return None


def target_from_reason(reason: str) -> str | None:
    """library/crt or library/xdk named by a classification row's reason text."""
    crt = bool(_REASON_CRT_RE.search(reason))
    xdk = bool(_REASON_XDK_RE.search(reason))
    if crt and xdk:
        return None
    return LIB_CRT if crt else LIB_XDK if xdk else None


def classify_library(
    name: str,
    *,
    evidence: str,
    section: str = "",
    reason: str = "",
) -> Decision:
    """Place one library function. `evidence` is tools.coverage's EVIDENCE_* string."""
    named = None if is_placeholder_name(name) else library_target_from_name(name)
    if evidence == EVIDENCE_SECTION:
        if section in _SECTION_XDK:
            return Decision(LIB_XDK, INFERRED, f"library: outside .text in XDK section {section}")
        return unknown(f"library outside .text in section {section or '?'}, not an XDK section")
    if evidence == EVIDENCE_XTLID:
        return Decision(LIB_XDK, MEASURED, "library: .XTLID record (XDK title library id)")
    if evidence == EVIDENCE_FLIRT:
        if named is None:
            return unknown("library: FLIRT match with a name no library rule covers")
        return Decision(named, MEASURED, f"library: FLIRT signature match {name}")
    if evidence == EVIDENCE_OVERRIDE:
        from_reason = target_from_reason(reason)
        if from_reason == LIB_CRT and named == LIB_MATH:
            # libc/libcmt also hold the x87 helpers, the math leaf is the finer placement
            return Decision(LIB_MATH, INFERRED, f"library: classification row, x87 name {name}")
        if from_reason is not None:
            return Decision(from_reason, MEASURED, f"library: classification row, {reason[:90]}")
        if named is not None:
            return Decision(named, INFERRED, f"library: classification row, name {name}")
        return unknown("library: classification row names no library")
    if evidence == EVIDENCE_NAMED:
        if named is None:
            return unknown("library: existing name no library rule covers")
        return Decision(named, INFERRED, f"library: existing name {name}")
    return unknown(f"library: evidence {evidence!r} places no library")
