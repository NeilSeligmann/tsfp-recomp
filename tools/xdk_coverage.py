# SPDX-License-Identifier: GPL-3.0-or-later
"""Classify XDK handler implementation coverage beyond registration (T120).

The native ``xdk_report`` binary answers one question: which of the 236 measured
surface addresses have a registered handler. It deliberately leaves
``full_implementation`` null, because a registered handler may implement only
some branches of the original function and refuse the rest through preflight
guards. This tool machine-extracts that refusal surface from the handler C
sources and joins it with the native registration report, producing THREE
buckets rather than one blended percentage:

  - ``refusals_enumerated``: registered, and at least one guard explicitly
    refuses a branch citing this address (``*_hle_fatal``, a local ``noreturn``
    refuse wrapper, or an announced ``d3d8_hle_note_unmodelled`` omission).
  - ``no_known_refusals``: registered, statically mapped to its handler, and no
    guard anywhere cites the address. This is NOT proof of completeness: it
    means no refusal admits an omission.
  - ``unknown``: registered per the native report but either no static
    registration was found, or its translation unit contains refusal sites this
    extraction could not attribute to an address.

Unregistered surface addresses stay a separate count. Refusals that cite
unregistered surface addresses or non-surface addresses (internal library
functions on a registered handler's path, game-side frames, seam guards) are
listed in their own sections instead of being silently dropped.

Extraction is static and deterministic: no guest code runs, output is sorted,
and paths are repo relative, so two runs over the same tree diff clean.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_SOURCE_DIRS = ("src/gpu", "src/audio", "src/input")

# Seeded refusal primitives: name -> (address parameter index or None, category).
SEED_PRIMITIVES: dict[str, tuple[int | None, str]] = {
    "d3d8_hle_fatal": (0, "refusal"),
    "xgrph_hle_fatal": (0, "refusal"),
    "d3d8_hle_note_unmodelled": (0, "unmodelled"),
}

REGISTER_FUNCTIONS = {
    "d3d8_hle_register": "D3D8",
    "dsound_hle_register": "DSOUND",
    "xinput_hle_register": "XAPI input",
    "xgrph_hle_register": "XGRPH",
    "xgrph_hle_register_preserving_eax": "XGRPH",
}

# Core files that DEFINE the primitives and registries; their own bodies are
# machinery, not handler guards.
CORE_FILES = {"d3d8_hle.c", "dsound_hle.c", "xinput_hle.c", "xgrph_hle.c"}

MAX_PROPAGATION_DEPTH = 8

CAVEATS = [
    "no_known_refusals means no guard cites the address. It does not prove the "
    "handler models every original branch, so full implementation coverage stays "
    "unknown rather than claimed.",
    "A guard may cite an internal library address on a registered handler's call "
    "path. Those refusals appear under refusals_citing_unregistered_surface and "
    "refusals_citing_non_surface, not under the handler.",
    "Render and sampler state are written inline by game code and are not call "
    "boundaries, so no bucket here covers them (docs/d3d8-usage.md section 9).",
    "unattributed_sites are guards whose cited address is computed at run time "
    "(unreadable memory operands, corrupt frames). They refuse corrupt state on "
    "any path, so they are reported but assigned to no single handler. A blocked "
    "entry-like expression instead marks the blocked file's handlers unknown.",
    "These three buckets must never be collapsed into one scoreboard number (T15).",
]


def strip_comments(text: str) -> str:
    """Replace comments with spaces, preserving offsets and newlines."""
    out = list(text)
    i = 0
    n = len(text)
    in_string: str | None = None
    while i < n:
        c = text[i]
        if in_string is not None:
            if c == "\\":
                i += 2
                continue
            if c == in_string:
                in_string = None
            i += 1
            continue
        if c in "\"'":
            in_string = c
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def mask_strings(text: str) -> str:
    """Replace string/char literal contents with spaces, preserving offsets."""
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    if out[i] != "\n":
                        out[i] = " "
                    i += 1
                if i < n and out[i] != "\n":
                    out[i] = " "
                i += 1
            i += 1
            continue
        i += 1
    return "".join(out)


@dataclass
class FunctionDef:
    name: str
    params: list[str]
    first_param_is_u32: bool
    is_static: bool
    body_start: int
    body_end: int


@dataclass
class MacroDef:
    name: str
    params: list[str]
    start: int
    end: int


@dataclass
class SourceFile:
    path: Path
    rel: str
    text: str
    code: str  # comments stripped, strings masked
    constants: dict[str, int] = field(default_factory=dict)
    functions: list[FunctionDef] = field(default_factory=list)
    macros: list[MacroDef] = field(default_factory=list)
    noreturn_names: set[str] = field(default_factory=set)

    def line_of(self, offset: int) -> int:
        return self.code.count("\n", 0, offset) + 1

    def function_at(self, offset: int) -> FunctionDef | None:
        for fn in self.functions:
            if fn.body_start <= offset < fn.body_end:
                return fn
        return None

    def macro_at(self, offset: int) -> MacroDef | None:
        for macro in self.macros:
            if macro.start <= offset < macro.end:
                return macro
        return None


CONST_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+\(?(0[xX][0-9a-fA-F]+|\d+)[uU]?\)?[ \t]*$"
    r"|^[ \t]*static[ \t]+const[ \t]+uint32_t[ \t]+(\w+)[ \t]*=[ \t]*"
    r"(0[xX][0-9a-fA-F]+|\d+)[uU]?[ \t]*;",
    re.MULTILINE,
)

NORETURN_RE = re.compile(
    r"\b(\w+)\s*\([^;{}]*\)\s*__attribute__\s*\(\s*\(\s*noreturn\s*\)\s*\)"
    r"|_Noreturn\s+(?:static\s+)?void\s+(\w+)\s*\("
    r"|static\s+_Noreturn\s+void\s+(\w+)\s*\("
)

# An unresolved address expression that NAMES an entry is an attribution failure:
# somebody meant a guest entry address and this extraction lost it, so the blocked
# file's handlers must classify unknown. Every other unresolved expression is a
# dynamic value guard (an unreadable memory operand, a corrupt frame), which refuses
# corrupt state at run time rather than admitting an unported branch.
ENTRY_LIKE_RE = re.compile(r"(?i)^\(?\s*(?:\(uint32_t\)\s*)?\w*entry\w*\s*\)?$")

MACRO_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^)\n]*)\)((?:[^\n]*\\\n)*[^\n]*)",
    re.MULTILINE,
)

LOCAL_U32_TEMPLATE = r"\buint32_t\s+{name}\s*=\s*([^;]+);"

TERNARY_RE = re.compile(
    r"^[^?]*\?\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?\s*:\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?\s*$"
)

KEYWORDS = {"if", "for", "while", "switch", "do", "else", "return", "sizeof"}

SIGNATURE_RE = re.compile(r"([A-Za-z_]\w*)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*$", re.DOTALL)


def parse_functions(code: str) -> list[FunctionDef]:
    functions: list[FunctionDef] = []
    depth = 0
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == "{":
            if depth == 0:
                prefix = code[max(0, i - 600) : i].rstrip()
                match = SIGNATURE_RE.search(prefix)
                if match is not None and match.group(1) not in KEYWORDS:
                    before = prefix[: match.start(1)]
                    # An initializer list, not a function body.
                    if not before.rstrip().endswith("="):
                        decl_line_start = before.rfind("\n") + 1
                        declaration = before[decl_line_start:]
                        params = split_params(match.group(2))
                        first_u32 = bool(params) and "uint32_t" in params[0][0]
                        end = matching_brace(code, i)
                        functions.append(
                            FunctionDef(
                                name=match.group(1),
                                params=[p[1] for p in params],
                                first_param_is_u32=first_u32,
                                is_static="static" in declaration.split(),
                                body_start=i,
                                body_end=end,
                            )
                        )
                        depth = 0
                        i = end
                        continue
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
        i += 1
    return functions


def matching_brace(code: str, start: int) -> int:
    depth = 0
    for i in range(start, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(code)


def split_params(params: str) -> list[tuple[str, str]]:
    """Split a parameter list into (full text, trailing identifier) pairs."""
    pieces: list[str] = []
    depth = 0
    current = ""
    for c in params:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == "," and depth == 0:
            pieces.append(current)
            current = ""
        else:
            current += c
    if current.strip():
        pieces.append(current)
    out: list[tuple[str, str]] = []
    for piece in pieces:
        piece = piece.strip()
        if piece in ("void", ""):
            continue
        tokens = re.findall(r"\w+", piece)
        out.append((piece, tokens[-1] if tokens else ""))
    return out


def split_args(code: str, open_paren: int) -> tuple[list[tuple[int, str]], int]:
    """Split the argument list opening at ``open_paren`` into (offset, text) pairs."""
    args: list[tuple[int, str]] = []
    depth = 0
    current_start = open_paren + 1
    i = open_paren
    while i < len(code):
        c = code[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append((current_start, code[current_start:i]))
                return args, i
        elif c == "," and depth == 1:
            args.append((current_start, code[current_start:i]))
            current_start = i + 1
        i += 1
    return args, len(code)


def call_sites(source: SourceFile, name: str) -> list[int]:
    """Offsets of calls to ``name`` inside function or macro bodies, not its own."""
    sites: list[int] = []
    for match in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*\(", source.code):
        enclosing = source.function_at(match.start())
        if enclosing is not None and enclosing.name == name:
            continue
        if enclosing is None and source.macro_at(match.start()) is None:
            continue
        sites.append(match.start())
    return sites


LITERAL_RE = re.compile(r"^\(?\s*(?:\(uint32_t\)\s*)?(0[xX][0-9a-fA-F]+|\d+)[uU]?\s*\)?$")
IDENT_RE = re.compile(r"^\(?\s*(?:\(uint32_t\)\s*)?([A-Za-z_]\w*)\s*\)?$")


@dataclass
class Resolution:
    addresses: list[tuple[int, str]] = field(default_factory=list)  # (address, chain)
    unresolved: list[tuple[str, str, int]] = field(default_factory=list)  # (expr, rel, line)


class Extractor:
    def __init__(self, root: Path, source_dirs: tuple[str, ...] = DEFAULT_SOURCE_DIRS) -> None:
        self.root = root
        self.files: list[SourceFile] = []
        for directory in source_dirs:
            for path in sorted((root / directory).glob("*.c")):
                raw = path.read_text(encoding="utf-8")
                code = mask_strings(strip_comments(raw))
                source = SourceFile(
                    path=path,
                    rel=str(path.relative_to(root)),
                    text=strip_comments(raw),
                    code=code,
                )
                # Literal constants from directly included repository headers
                # share this translation unit's registration expressions. Do
                # not pool unrelated headers or guess conflicting definitions.
                ambiguous: set[str] = set()
                for include in re.finditer(
                    r'^\s*#\s*include\s+"([^"\n]+)"', source.text, re.MULTILINE
                ):
                    header = (path.parent / include.group(1)).resolve()
                    if not header.is_relative_to(root.resolve()) or not header.is_file():
                        continue
                    header_code = mask_strings(strip_comments(header.read_text(encoding="utf-8")))
                    for match in CONST_RE.finditer(header_code):
                        name = match.group(1) or match.group(3)
                        value = int(match.group(2) or match.group(4), 0)
                        if name in source.constants and source.constants[name] != value:
                            ambiguous.add(name)
                        source.constants[name] = value
                for name in ambiguous:
                    source.constants.pop(name, None)
                for match in CONST_RE.finditer(code):
                    name = match.group(1) or match.group(3)
                    value = match.group(2) or match.group(4)
                    source.constants[name] = int(value, 0)
                source.functions = parse_functions(code)
                for match in MACRO_RE.finditer(code):
                    source.macros.append(
                        MacroDef(
                            name=match.group(1),
                            params=[p.strip() for p in match.group(2).split(",") if p.strip()],
                            start=match.start(),
                            end=match.end(),
                        )
                    )
                source.noreturn_names = {
                    m.group(1) or m.group(2) or m.group(3) for m in NORETURN_RE.finditer(code)
                }
                self.files.append(source)
        self.by_name = {f.path.name: f for f in self.files}
        self._call_cache: dict[tuple[str, str], list[int]] = {}

    def primitives(self) -> list[tuple[str, int | None, str, str | None]]:
        """(name, address param index or None, category, defining file or None)."""
        found: list[tuple[str, int | None, str, str | None]] = [
            (name, index, category, None)
            for name, (index, category) in sorted(SEED_PRIMITIVES.items())
        ]
        for source in self.files:
            if source.path.name in CORE_FILES:
                continue
            for fn in source.functions:
                if fn.name not in source.noreturn_names:
                    continue
                body = source.code[fn.body_start : fn.body_end]
                calls_abort = re.search(r"\babort\s*\(", body) is not None
                calls_seed = any(
                    re.search(r"\b" + re.escape(seed) + r"\s*\(", body) for seed in SEED_PRIMITIVES
                )
                if calls_abort or calls_seed:
                    index = 0 if fn.first_param_is_u32 else None
                    found.append((fn.name, index, "refusal", source.path.name))
        return found

    def calls_of(self, source: SourceFile, name: str) -> list[int]:
        key = (source.rel, name)
        if key not in self._call_cache:
            self._call_cache[key] = call_sites(source, name)
        return self._call_cache[key]

    def resolve(
        self,
        source: SourceFile,
        offset: int,
        expr: str,
        chain: tuple[str, ...],
    ) -> Resolution:
        result = Resolution()
        expr = expr.strip()
        line = source.line_of(offset)
        if len(chain) > MAX_PROPAGATION_DEPTH:
            result.unresolved.append((expr, source.rel, line))
            return result
        literal = LITERAL_RE.match(expr)
        if literal is not None:
            result.addresses.append((int(literal.group(1), 0), "->".join(chain)))
            return result
        ident = IDENT_RE.match(expr)
        if ident is None:
            result.unresolved.append((expr, source.rel, line))
            return result
        name = ident.group(1)
        if name in source.constants:
            result.addresses.append((source.constants[name], "->".join(chain)))
            return result
        enclosing = source.function_at(offset)
        if enclosing is not None:
            body = source.code[enclosing.body_start : enclosing.body_end]
            local = re.search(LOCAL_U32_TEMPLATE.format(name=re.escape(name)), body)
            if local is not None:
                value = local.group(1).strip()
                literal = LITERAL_RE.match(value)
                if literal is not None:
                    result.addresses.append((int(literal.group(1), 0), "->".join(chain)))
                    return result
                ternary = TERNARY_RE.match(value)
                if ternary is not None:
                    for group in (1, 2):
                        result.addresses.append((int(ternary.group(group), 0), "->".join(chain)))
                    return result
                token = IDENT_RE.match(value)
                if token is not None and token.group(1) in source.constants:
                    result.addresses.append((source.constants[token.group(1)], "->".join(chain)))
                    return result
                result.unresolved.append((expr, source.rel, line))
                return result
        if enclosing is not None and name in enclosing.params:
            index = enclosing.params.index(name)
            if enclosing.name in chain:
                result.unresolved.append((expr, source.rel, line))
                return result
            scope = [source] if enclosing.is_static else self.files
            forwarded_any = False
            for caller_file in scope:
                for call in self.calls_of(caller_file, enclosing.name):
                    open_paren = caller_file.code.index("(", call)
                    args, _ = split_args(caller_file.code, open_paren)
                    if index >= len(args):
                        continue
                    forwarded_any = True
                    arg_offset, arg_text = args[index]
                    nested = self.resolve(
                        caller_file,
                        arg_offset,
                        arg_text,
                        chain + (enclosing.name,),
                    )
                    result.addresses.extend(nested.addresses)
                    result.unresolved.extend(nested.unresolved)
            if not forwarded_any:
                result.unresolved.append((expr, source.rel, line))
            return result
        macro = source.macro_at(offset)
        if macro is not None and name in macro.params:
            index = macro.params.index(name)
            if macro.name in chain:
                result.unresolved.append((expr, source.rel, line))
                return result
            expanded_any = False
            for call in re.finditer(r"(?<![\w.>])" + re.escape(macro.name) + r"\s*\(", source.code):
                if macro.start <= call.start() < macro.end:
                    continue  # the definition itself
                open_paren = source.code.index("(", call.start())
                args, _ = split_args(source.code, open_paren)
                if index >= len(args):
                    continue
                expanded_any = True
                arg_offset, arg_text = args[index]
                nested = self.resolve(source, arg_offset, arg_text, chain + (macro.name,))
                result.addresses.extend(nested.addresses)
                result.unresolved.extend(nested.unresolved)
            if not expanded_any:
                result.unresolved.append((expr, source.rel, line))
            return result
        result.unresolved.append((expr, source.rel, line))
        return result

    def reason_of(self, source: SourceFile, args: list[tuple[int, str]], index: int) -> str:
        """The format-string argument at ``index``, read from unmasked text."""
        if index >= len(args):
            return "<none>"
        start = args[index][0]
        end = start + len(args[index][1])
        raw = source.text[start:end].strip()
        strings = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
        if strings:
            return "".join(strings)
        return f"<dynamic: {raw}>"

    def refusal_sites(self) -> tuple[list[dict], list[dict]]:
        """(attributed site records, unattributed site records)."""
        attributed: list[dict] = []
        unattributed: list[dict] = []
        primitives = self.primitives()
        for source in self.files:
            if source.path.name in CORE_FILES:
                continue
            for name, addr_index, category, defining_file in primitives:
                if defining_file is not None and defining_file != source.path.name:
                    continue  # local noreturn wrappers are file scoped
                for call in self.calls_of(source, name):
                    open_paren = source.code.index("(", call)
                    args, _ = split_args(source.code, open_paren)
                    line = source.line_of(call)
                    reason_index = (addr_index + 1) if addr_index is not None else 0
                    record = {
                        "category": category,
                        "primitive": name,
                        "file": source.rel,
                        "line": line,
                        "reason": self.reason_of(source, args, reason_index),
                    }
                    if addr_index is None:
                        record["address"] = None
                        record["attribution"] = "file"
                        attributed.append(record)
                        continue
                    resolution = self.resolve(source, args[addr_index][0], args[addr_index][1], ())
                    for address, chain in sorted(set(resolution.addresses)):
                        entry = dict(record)
                        entry["address"] = address
                        entry["attribution"] = "exact" if chain == "" else "propagated"
                        if chain:
                            entry["via"] = chain
                        attributed.append(entry)
                    if resolution.unresolved:
                        blocked = sorted(
                            {
                                (expr, f"{rel}:{expr_line}")
                                for expr, rel, expr_line in resolution.unresolved
                            }
                        )
                        unattributed.append(
                            {
                                "category": category,
                                "primitive": name,
                                "file": source.rel,
                                "line": line,
                                "reason": record["reason"],
                                "blocked": [{"expression": expr, "at": at} for expr, at in blocked],
                                "entry_like": any(ENTRY_LIKE_RE.match(expr) for expr, _ in blocked),
                            }
                        )
        attributed.sort(key=lambda r: (r["address"] or 0, r["file"], r["line"], r["reason"]))
        unattributed.sort(key=lambda r: (r["file"], r["line"], r["primitive"]))
        return attributed, unattributed

    def registrations(self) -> tuple[dict[int, dict], list[dict]]:
        """address -> {handler, file, registry}, plus unresolved registrations."""
        registered: dict[int, dict] = {}
        unresolved: list[dict] = []
        for source in self.files:
            if source.path.name in CORE_FILES:
                continue
            for name, module in REGISTER_FUNCTIONS.items():
                for call in self.calls_of(source, name):
                    open_paren = source.code.index("(", call)
                    args, _ = split_args(source.code, open_paren)
                    if len(args) != 2:
                        continue
                    line = source.line_of(call)
                    address_expr = args[0][1].strip()
                    handler_expr = args[1][1].strip()
                    table = re.match(r"(\w+)\[\w+\]\.address$", address_expr)
                    if table is not None:
                        for address, handler in self.table_rows(source, table.group(1)):
                            registered[address] = {
                                "handler": handler,
                                "file": source.rel,
                                "registry": module,
                            }
                        continue
                    resolution = self.resolve(source, args[0][0], address_expr, ())
                    exact = [a for a, chain in resolution.addresses if chain == ""]
                    if len(exact) == 1:
                        registered[exact[0]] = {
                            "handler": handler_expr,
                            "file": source.rel,
                            "registry": module,
                        }
                    else:
                        unresolved.append(
                            {"file": source.rel, "line": line, "expression": address_expr}
                        )
        return registered, unresolved

    def table_rows(self, source: SourceFile, array_name: str) -> list[tuple[int, str]]:
        rows: list[tuple[int, str]] = []
        match = re.search(r"\b" + re.escape(array_name) + r"\[\]\s*=\s*\{", source.code)
        if match is None:
            return rows
        start = match.end() - 1
        end = matching_brace(source.code, start)
        body = source.code[start:end]
        for pair in re.finditer(r"\{\s*(\w+)\s*,\s*&?(\w+)\s*\}", body):
            token, handler = pair.group(1), pair.group(2)
            literal = LITERAL_RE.match(token)
            if literal is not None:
                rows.append((int(literal.group(1), 0), handler))
            elif token in source.constants:
                rows.append((source.constants[token], handler))
        return rows


def load_report(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        report = json.load(handle)
    if report.get("kind") != "xdk-handler-registration":
        raise SystemExit(f"{path} is not an xdk-handler-registration report")
    return report


def classify(root: Path, report: dict) -> dict:
    extractor = Extractor(root)
    sites, unattributed = extractor.refusal_sites()
    static_registrations, unresolved_registrations = extractor.registrations()

    surface: dict[int, dict] = {}
    for entry in report["entries"]:
        address = int(entry["address"], 16)
        surface[address] = {
            "section": entry["section"],
            "module": entry.get("module"),
            "registered": bool(entry["registered_handler"]),
        }

    registered_addresses = sorted(a for a, e in surface.items() if e["registered"])

    by_address: dict[int, list[dict]] = {}
    file_scoped: dict[str, list[dict]] = {}
    for site in sites:
        if site["address"] is None:
            file_scoped.setdefault(site["file"], []).append(site)
        else:
            by_address.setdefault(site["address"], []).append(site)

    registrations_by_file: dict[str, list[int]] = {}
    for address, info in static_registrations.items():
        registrations_by_file.setdefault(info["file"], []).append(address)

    # Only an entry-like blocked expression poisons attribution: it means a guard
    # cites a guest entry this extraction could not pin, so every handler in the
    # file where resolution blocked must say unknown rather than clean. Dynamic
    # value guards (memory operands, frame reads) stay reported but do not taint.
    files_with_unattributed: set[str] = set()
    for record in unattributed:
        for blocked in record["blocked"]:
            if ENTRY_LIKE_RE.match(blocked["expression"]):
                files_with_unattributed.add(blocked["at"].rsplit(":", 1)[0])
                files_with_unattributed.add(record["file"])

    handlers: list[dict] = []
    buckets = {"refusals_enumerated": 0, "no_known_refusals": 0, "unknown": 0}
    for address in registered_addresses:
        static = static_registrations.get(address)
        own_sites = list(by_address.get(address, ()))
        scoped_note = None
        if static is not None:
            scoped = file_scoped.get(static["file"], ())
            peers = registrations_by_file.get(static["file"], [])
            if scoped and len(peers) == 1:
                own_sites = own_sites + list(scoped)
                scoped_note = "includes file-scoped refusals (single handler unit)"
        refusals = [s for s in own_sites if s["category"] == "refusal"]
        unmodelled = [s for s in own_sites if s["category"] == "unmodelled"]
        if static is None:
            classification = "unknown"
            detail = "registered per the native report, no static registration found"
        elif refusals or unmodelled:
            classification = "refusals_enumerated"
            detail = scoped_note
        elif static["file"] in files_with_unattributed or (
            file_scoped.get(static["file"])
            and len(registrations_by_file.get(static["file"], [])) > 1
        ):
            classification = "unknown"
            detail = "unattributed or shared file-scoped refusal sites in its unit"
        else:
            classification = "no_known_refusals"
            detail = None
        buckets[classification] += 1
        record = {
            "address": f"0x{address:08X}",
            "section": surface[address]["section"],
            "module": surface[address]["module"],
            "classification": classification,
            "handler": static["handler"] if static else None,
            "file": static["file"] if static else None,
            "refused_branches": len(refusals),
            "unmodelled_notes": len(unmodelled),
            "refusals": [
                {k: v for k, v in s.items() if k not in ("category", "address")} for s in refusals
            ],
            "unmodelled": [
                {k: v for k, v in s.items() if k not in ("category", "address")} for s in unmodelled
            ],
        }
        if detail:
            record["detail"] = detail
        handlers.append(record)

    registered_set = set(registered_addresses)
    static_only = sorted(set(static_registrations) - registered_set)
    citing_unregistered = []
    citing_non_surface = []
    for address in sorted(by_address):
        if address in registered_set:
            continue
        rows = [{k: v for k, v in s.items() if k != "address"} for s in by_address[address]]
        target = citing_unregistered if address in surface else citing_non_surface
        target.append({"address": f"0x{address:08X}", "sites": rows})

    return {
        "schema": 1,
        "kind": "xdk-implementation-coverage",
        "surface_count": len(surface),
        "registered_handler_count": len(registered_addresses),
        "buckets": {
            "registered_refusals_enumerated": buckets["refusals_enumerated"],
            "registered_no_known_refusals": buckets["no_known_refusals"],
            "registered_unknown": buckets["unknown"],
            "unregistered": len(surface) - len(registered_addresses),
        },
        "full_implementation_count": None,
        "full_coverage": "unknown",
        "handlers": handlers,
        "refusals_citing_unregistered_surface": citing_unregistered,
        "refusals_citing_non_surface": citing_non_surface,
        "unattributed_sites": unattributed,
        "consistency": {
            "static_registrations_found": len(static_registrations),
            "static_only_addresses": [f"0x{a:08X}" for a in static_only],
            "unresolved_registration_calls": unresolved_registrations,
        },
        "caveats": CAVEATS,
    }


def render_text(result: dict) -> str:
    lines = [
        "XDK implementation coverage (three buckets, never one number)",
        f"surface addresses: {result['surface_count']}",
        f"registered handlers: {result['registered_handler_count']}",
    ]
    for key, value in result["buckets"].items():
        lines.append(f"  {key}: {value}")
    lines.append("")
    lines.append("address      class                 refused  noted  handler")
    for handler in result["handlers"]:
        lines.append(
            f"{handler['address']}  {handler['classification']:<20}"
            f"  {handler['refused_branches']:>7}  {handler['unmodelled_notes']:>5}"
            f"  {handler['handler'] or '?'}"
        )
    lines.append("")
    lines.append(
        f"refusals citing unregistered surface addresses: "
        f"{len(result['refusals_citing_unregistered_surface'])}"
    )
    lines.append(
        f"refusals citing non-surface addresses: {len(result['refusals_citing_non_surface'])}"
    )
    lines.append(f"unattributed refusal sites: {len(result['unattributed_sites'])}")
    lines.append("")
    for caveat in result["caveats"]:
        lines.append(f"caveat: {caveat}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Classify XDK handler branch refusals beyond registration."
    )
    default_root = Path(__file__).resolve().parents[1]
    parser.add_argument(
        "--root",
        type=Path,
        default=default_root,
        help="repository root holding src/gpu, src/audio, src/input",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="native xdk-handler-registration JSON "
        "(default: ROOT/generated/retail/xdk-handler-registration.json)",
    )
    parser.add_argument("--format", choices=("json", "text"), default="json", help="output format")
    parser.add_argument("--out", type=Path, default=None, help="output file (default: stdout)")
    args = parser.parse_args(argv)
    report_path = args.report or (args.root / "generated/retail/xdk-handler-registration.json")
    if not report_path.exists():
        print(
            f"xdk_coverage: {report_path} is missing. Build and run the native "
            "xdk_report binary first, then pass its output via --report.",
            file=sys.stderr,
        )
        return 2
    result = classify(args.root, load_report(report_path))
    rendered = (
        json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.format == "json"
        else render_text(result)
    )
    if args.out is not None:
        args.out.write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
