# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Reconcile an instrumented boot's capture with the static shader census.

Two layers, kept apart so the logic is testable without the user's executable:

  * `build_census` reads the XBE with `tools.shaderscan` (call sites and their key
    arguments, the headed vertex programs, who names them, the key-modification
    variants, the pixel definitions) and turns it into a `Census` of plain data.
  * `reconcile` takes a parsed `Capture`, a `Census` and a function that says what the
    title's own builders emit for a key, and returns a `Report`. It is pure.

Every statement in the report is one of three kinds and the renderer prints the kind:
MEASURED (the capture or a cross-check of it says so), INFERRED (a reading of measured
facts) and NOT REACHED (something the static census names that the boot never exercised,
which is absence of observation and never evidence of absence).

The report carries counts, addresses and dwords. It never carries a shader.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from tools.shaderprobe.log import Capture, fnv1a64
from tools.shaderprobe.profile import BuilderProfile, Profile

#: Length in bytes of `call rel32`. A direct call site's return address is `site + 5`.
CALL_LENGTH = 5


class EmptyCapture(Exception):
    """The capture has none of a record kind the question needs, so it proves nothing."""


@dataclass
class SiteFact:
    """One static call site and what it passes as the argument that matters."""

    address: int
    #: "imm" when the argument is a literal, else the classifier's kind
    kind: str
    value: int | None


@dataclass
class Census:
    """Static facts, plain data. Built from the XBE, or by hand in a test."""

    #: batcher function -> its direct call sites (key argument classified)
    batcher_sites: dict[int, list[SiteFact]] = field(default_factory=dict)
    #: direct call sites of the program loader, argument 1 classified
    loader_sites: list[SiteFact] = field(default_factory=list)
    #: address -> instruction count, every headed vertex program in the image
    programs: dict[int, int] = field(default_factory=dict)
    #: programs a loader site names by a literal pointer
    named_programs: set[int] = field(default_factory=set)
    #: literal pixel-definition pointers the SetPixelShader wrapper is called with
    pixel_definitions: set[int] = field(default_factory=set)
    #: key values the title's literals form directly, and after each OR/AND variation
    base_keys: set[int] = field(default_factory=set)
    variant_keys: set[int] = field(default_factory=set)
    #: image address range, to tell a static pointer from a heap one
    image_low: int = 0
    image_high: int = 0

    def in_image(self, pointer: int) -> bool:
        return self.image_low <= pointer < self.image_high


#: What a builder emits for a key: (FNV-1a 64 of the source, its length, assembler flags),
#: or None when the builder never reached the assembler. Injected so tests need no emulator.
EmissionFunction = Callable[[BuilderProfile, int], tuple[int, int, int] | None]


@dataclass
class AssemblerCall:
    builder: str
    key: int
    length: int
    flags: int
    #: True when the title's own builder, run on `key & mask`, emits exactly this source
    reconciled: bool


@dataclass
class Report:
    records: dict[str, int] = field(default_factory=dict)
    #: every value stored to the key global, in order, with its writer
    stores: list[tuple[int, int, int]] = field(default_factory=list)
    distinct_stored: list[int] = field(default_factory=list)
    #: keys read at each draw dispatch, in order
    draw_keys: list[int] = field(default_factory=list)
    #: keys at every probed dispatch, distinct, in first-seen order
    keys_at_probes: list[int] = field(default_factory=list)
    observed_keys: list[int] = field(default_factory=list)
    keys_not_literal_base: list[int] = field(default_factory=list)
    keys_not_in_variants: list[int] = field(default_factory=list)
    base_keys_total: int = 0
    base_keys_formed: int = 0
    batcher_calls: dict[int, int] = field(default_factory=dict)
    batcher_sites_total: dict[int, int] = field(default_factory=dict)
    batcher_sites_reached: dict[int, list[int]] = field(default_factory=dict)
    batcher_sites_unknown: list[int] = field(default_factory=list)
    #: per reached literal site, whether the logged key argument equals the static literal
    literal_agreements: int = 0
    literal_disagreements: list[int] = field(default_factory=list)
    nonliteral_sites_total: dict[int, int] = field(default_factory=dict)
    nonliteral_sites_reached: dict[int, int] = field(default_factory=dict)
    key_modifier_calls: int = 0
    key_modifier_stores: int = 0
    assembler_calls: list[AssemblerCall] = field(default_factory=list)
    loader_calls: int = 0
    loader_sites_reached: list[int] = field(default_factory=list)
    loader_pointer_classes: Counter[str] = field(default_factory=Counter)
    programs_loaded: list[int] = field(default_factory=list)
    programs_total: int = 0
    unnamed_programs: list[int] = field(default_factory=list)
    unnamed_programs_loaded: list[int] = field(default_factory=list)
    pixel_installs: Counter[str] = field(default_factory=Counter)
    pixel_definitions_loaded: list[int] = field(default_factory=list)
    pixel_definitions_total: int = 0
    gaps: list[str] = field(default_factory=list)
    #: where each host thread stopped, from the host's own report
    stops: list[tuple[str, str, int | None, str]] = field(default_factory=list)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise EmptyCapture(message)


def reconcile(
    capture: Capture, census: Census, profile: Profile, emission: EmissionFunction
) -> Report:
    """Build the report. Raises `EmptyCapture` rather than reporting from nothing."""
    _require(bool(capture.probes), "no probe records: the host probe did not log (TSFP_XDK_PROBE)")
    _require(
        bool(capture.enters), "no enter records: the host was not built from the instrumented lift"
    )
    _require(bool(capture.stores), "no store records: no key store was logged")
    _require(bool(census.batcher_sites) and bool(census.programs), "the census is empty")

    report = Report()
    report.records = {
        "probe": len(capture.probes),
        "enter": len(capture.enters),
        "store": len(capture.stores),
    }
    key_address = profile.key_global

    report.stores = [(s.function, s.label, s.value) for s in capture.stores]
    seen: dict[int, None] = {}
    for store in capture.stores:
        seen.setdefault(store.value)
    report.distinct_stored = list(seen)

    at_probes: dict[int, None] = {}
    for probe in capture.probes:
        _require(key_address in probe.words, f"probe n={probe.number} sampled no key word")
        at_probes.setdefault(probe.words[key_address])
        if probe.address in profile.xdk_draws:
            report.draw_keys.append(probe.words[key_address])
    report.keys_at_probes = list(at_probes)

    observed = dict.fromkeys([*report.distinct_stored, *report.keys_at_probes])
    report.observed_keys = list(observed)
    report.base_keys_total = len(census.base_keys)
    report.keys_not_literal_base = sorted(k for k in observed if k not in census.base_keys)
    report.keys_not_in_variants = sorted(k for k in observed if k not in census.variant_keys)
    report.base_keys_formed = len(census.base_keys & set(observed))

    _reconcile_batchers(report, capture, census, profile)

    report.key_modifier_calls = sum(1 for e in capture.enters if e.function == profile.key_modifier)
    report.key_modifier_stores = sum(
        1 for s in capture.stores if s.function == profile.key_modifier
    )

    _reconcile_assembler(report, capture, profile, emission)
    _reconcile_programs(report, capture, census, profile)
    report.stops = [(s.thread, s.reason, s.address, s.detail) for s in capture.stops]
    return report


def _reconcile_batchers(report: Report, capture: Capture, census: Census, profile: Profile) -> None:
    for batcher in profile.batchers:
        sites = census.batcher_sites.get(batcher, [])
        report.batcher_sites_total[batcher] = len(sites)
        by_address = {site.address: site for site in sites}
        nonliteral = [site for site in sites if site.kind != "imm"]
        report.nonliteral_sites_total[batcher] = len(nonliteral)
        calls = [e for e in capture.enters if e.function == batcher]
        report.batcher_calls[batcher] = len(calls)
        reached: dict[int, None] = {}
        for call in calls:
            site = by_address.get(call.caller - CALL_LENGTH)
            if site is None:
                report.batcher_sites_unknown.append(call.caller)
                continue
            reached.setdefault(site.address)
            if site.kind == "imm":
                argument = call.args[profile.key_argument(batcher) - 1]
                if argument == site.value:
                    report.literal_agreements += 1
                else:
                    report.literal_disagreements.append(site.address)
        report.batcher_sites_reached[batcher] = list(reached)
        report.nonliteral_sites_reached[batcher] = sum(
            1 for site in nonliteral if site.address in reached
        )


def _reconcile_assembler(
    report: Report, capture: Capture, profile: Profile, emission: EmissionFunction
) -> None:
    builders = (profile.vertex_builder, profile.pixel_builder)
    for probe in capture.probes:
        if probe.address != profile.assembler:
            continue
        _require(
            probe.digest is not None and not probe.unreadable,
            "an assembler probe has no readable hash",
        )
        owner = next((b for b in builders if b.entry <= probe.caller < b.entry + b.size), None)
        _require(owner is not None, f"assembler caller {probe.caller:#x} is in neither builder")
        assert owner is not None
        key = probe.words[profile.key_global]
        emitted = emission(owner, key & owner.mask)
        reconciled = (
            emitted is not None
            and emitted[0] == probe.digest
            and emitted[1] == probe.args[profile.assembler_length]
            and emitted[2] == probe.args[profile.assembler_flags]
        )
        report.assembler_calls.append(
            AssemblerCall(
                owner.name,
                key,
                probe.args[profile.assembler_length],
                probe.args[profile.assembler_flags],
                reconciled,
            )
        )


def _reconcile_programs(report: Report, capture: Capture, census: Census, profile: Profile) -> None:
    report.programs_total = len(census.programs)
    report.unnamed_programs = sorted(set(census.programs) - census.named_programs)
    loaded: dict[int, None] = {}
    for probe in capture.probes:
        if probe.address == profile.xdk_loader:
            pointer = probe.args[0]
            report.loader_calls += 1
            if pointer in census.programs:
                cls = (
                    "static, named by a loader site"
                    if pointer in census.named_programs
                    else "static, NEVER NAMED"
                )
                loaded.setdefault(pointer)
            elif census.in_image(pointer):
                cls = "in the image, not a headed program"
            else:
                cls = "outside the image (run-time buffer)"
            report.loader_pointer_classes[cls] += 1
        elif probe.address == profile.xdk_pixel_shader:
            pointer = probe.args[0]
            if pointer in census.pixel_definitions:
                report.pixel_installs["static definition"] += 1
                if pointer not in report.pixel_definitions_loaded:
                    report.pixel_definitions_loaded.append(pointer)
            elif pointer == 0:
                report.pixel_installs["null (fixed function)"] += 1
            elif census.in_image(pointer):
                report.pixel_installs["in the image, not a known definition"] += 1
            else:
                report.pixel_installs["outside the image (run-time buffer)"] += 1
    report.programs_loaded = list(loaded)
    report.unnamed_programs_loaded = [a for a in loaded if a not in census.named_programs]
    report.pixel_definitions_total = len(census.pixel_definitions)
    loader_sites = {s.address: s for s in census.loader_sites}
    reached: dict[int, None] = {}
    for enter in capture.enters:
        if enter.function == profile.loader:
            site = loader_sites.get(enter.caller - CALL_LENGTH)
            if site is not None:
                reached.setdefault(site.address)
    report.loader_sites_reached = list(reached)
    if report.draw_keys and len(report.draw_keys) < 100:
        report.gaps.append(
            f"only {len(report.draw_keys)} draw dispatches were reached; gameplay keys are not exercised"
        )


# ---------------------------------------------------------------------------
# census from the image
# ---------------------------------------------------------------------------


def build_census(xbe: Path, profile: Profile) -> Census:
    """Read the static facts out of the XBE with `tools.shaderscan`."""
    from tools.gen_d3d8_surface import decode_section
    from tools.shaderscan import callargs, vsh, xrefs
    from tools.shaderscan.image import Image

    image = Image.load(xbe)
    text = image.xbe.section_by_name(".text")
    assert text is not None, "no .text section"
    body = image.raw[text.raw_addr : text.raw_addr + text.raw_size]
    insns, _ = decode_section(body, text.virtual_addr)

    census = Census()
    census.image_low = image.xbe.base_address
    census.image_high = image.xbe.base_address + image.xbe.size_of_image

    def fact(site: callargs.Site, argument: int) -> SiteFact:
        value = site.args[argument - 1]
        return SiteFact(site.va, value.kind, value.value)

    for batcher in profile.batchers:
        sites = callargs.find_sites(insns, {batcher}, profile.key_argument(batcher))
        census.batcher_sites[batcher] = [fact(s, profile.key_argument(batcher)) for s in sites]
    loader_sites = callargs.find_sites(insns, {profile.loader}, 1)
    census.loader_sites = [fact(s, 1) for s in loader_sites]

    for section in image.xbe.sections:
        data = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
        for program in vsh.find_headed_programs(data):
            census.programs[section.virtual_addr + program.offset] = program.instructions
    census.named_programs = {
        site.value
        for site in census.loader_sites
        if site.kind == "imm" and site.value is not None and site.value in census.programs
    }
    # The wrapper's calls share a tail, so this needs the path-aware form the combiner
    # census uses (docs/shader-inputs.md 5.1), or the 35 definitions read as 33.
    from tools.shaderscan import combiners

    leaders = callargs.branch_leaders(insns)
    position = {insn.address: index for index, insn in enumerate(insns)}
    install_sites = callargs.find_sites(insns, {profile.pixel_install}, 1, leaders)
    arguments = [
        combiners.tail_call_argument(insns, position[s.va]) if s.kind == "jmp" else s.args[0]
        for s in install_sites
    ]
    census.pixel_definitions = {
        a.value
        for a in arguments
        if a.kind == callargs.KIND_IMM and a.value is not None and image.looks_like_pointer(a.value)
    }

    literals = {
        site.value
        for batcher in profile.batchers
        for site in census.batcher_sites[batcher]
        if site.kind == "imm" and site.value is not None
    }
    wrapper_literals: set[int] = set()
    for entry, register, end in profile.wrapper_literals:
        prefix = f"{register}, 0x"
        wrapper_literals |= {
            int(i.op_str.split(", ")[1], 16)
            for i in insns
            if entry <= i.address < end
            and i.mnemonic == "mov"
            and i.op_str.startswith(prefix)
            and "[" not in i.op_str
        }
    census.base_keys = literals | wrapper_literals
    modifications = xrefs.global_modifications(insns, profile.key_global)
    ors = sorted({imm for (op, imm) in modifications if op == "or"})
    ands = sorted({imm for (op, imm) in modifications if op == "and"})
    for value in census.base_keys:
        variants = {value}
        for bit in ors:
            variants |= {v | bit for v in variants}
        for clear in ands:
            variants |= {v & clear for v in variants}
        census.variant_keys |= variants
    return census


def emission_function(xbe: Path, profile: Profile) -> EmissionFunction:
    """What the title's own builders emit for a key, run in Unicorn from the image."""
    from tools.shaderscan.builders import BuilderEmulator, BuilderSpec
    from tools.shaderscan.image import Image

    image = Image.load(xbe)
    emulators: dict[str, BuilderEmulator] = {}

    def emit(builder: BuilderProfile, key: int) -> tuple[int, int, int] | None:
        if builder.name not in emulators:
            spec = BuilderSpec(builder.name, builder.entry, builder.key_register, profile.assembler)
            emulators[builder.name] = BuilderEmulator(image, spec)
        result = emulators[builder.name].run(key)
        if result is None:
            return None
        return fnv1a64(result.content), result.length, result.flags

    return emit


def analyze(xbe: Path, capture: Capture, profile: Profile) -> Report:
    _require(bool(capture.probes), "no probe records: the host probe did not log")
    census = build_census(xbe, profile)
    return reconcile(capture, census, profile, emission_function(xbe, profile))


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------


def _hex_list(values: list[int]) -> str:
    return ", ".join(f"{v:#x}" for v in values) if values else "none"


def render(report: Report) -> str:
    lines: list[str] = []
    add = lines.append
    add("shader key probe report (counts, addresses and dwords only)")
    add(f"records: {report.records}")
    add("")
    add("MEASURED keys formed in this boot")
    add(
        f"  stores to the key global: {len(report.stores)}, distinct values {_hex_list(report.distinct_stored)}"
    )
    add(f"  keys seen at probed dispatches: {_hex_list(report.keys_at_probes)}")
    add(
        f"  keys at the {len(report.draw_keys)} draw dispatches, in order: {_hex_list(report.draw_keys)}"
    )
    add(
        f"  observed keys that are NOT a literal base key: {_hex_list(report.keys_not_literal_base)}"
    )
    add(
        f"  observed keys outside the static OR/AND variant set: {_hex_list(report.keys_not_in_variants)}"
    )
    add(f"  base keys formed: {report.base_keys_formed} of {report.base_keys_total}")
    add(
        f"  key modifier: {report.key_modifier_calls} calls, {report.key_modifier_stores} stores executed"
    )
    add("")
    add("MEASURED batcher call sites")
    for batcher, total in report.batcher_sites_total.items():
        reached = report.batcher_sites_reached.get(batcher, [])
        add(
            f"  {batcher:#x}: {report.batcher_calls.get(batcher, 0)} calls from {len(reached)} of {total} static sites "
            f"({_hex_list(reached)}); non-literal sites {report.nonliteral_sites_reached.get(batcher, 0)} "
            f"of {report.nonliteral_sites_total.get(batcher, 0)} reached"
        )
    add(
        f"  logged key argument equals the static literal at the site: {report.literal_agreements} agree, {len(report.literal_disagreements)} disagree"
    )
    add(
        f"  calls from a return address that is not a static direct site: {_hex_list(report.batcher_sites_unknown)}"
    )
    add("")
    add("MEASURED assembler calls, reconciled against the title's own builder run on the key")
    for call in report.assembler_calls:
        add(
            f"  {call.builder} key {call.key:#x}: {call.length} bytes, flags {call.flags:#x}, builder emits the same source: {call.reconciled}"
        )
    add("")
    add("MEASURED program loader")
    add(f"  {report.loader_calls} calls, pointer classes {dict(report.loader_pointer_classes)}")
    add(f"  static programs loaded: {_hex_list(report.programs_loaded)} of {report.programs_total}")
    add(
        f"  never-named programs: {len(report.unnamed_programs)}, loaded in this boot: {_hex_list(report.unnamed_programs_loaded)}"
    )
    add(f"  loader call sites reached: {_hex_list(report.loader_sites_reached)}")
    add(
        f"  pixel installs: {dict(report.pixel_installs)}; static definitions installed "
        f"{len(report.pixel_definitions_loaded)} of {report.pixel_definitions_total}"
    )
    add("")
    add("NOT REACHED (absence of observation, not evidence of absence)")
    for batcher, total in report.batcher_sites_total.items():
        reached_count = len(report.batcher_sites_reached.get(batcher, []))
        add(f"  {batcher:#x}: {total - reached_count} of {total} static call sites never ran")
    add(
        f"  programs never loaded: {report.programs_total - len(report.programs_loaded)} of {report.programs_total}"
    )
    for gap in report.gaps:
        add(f"  {gap}")
    if not report.stops:
        add("  the capture has no end-of-run stop report, so what ended the boot is unknown")
    for thread, reason, address, detail in report.stops:
        where = f" at {address:#x}" if address is not None else ""
        add(f"  stop: {thread}: {reason}{where}{(' (' + detail + ')') if detail else ''}")
    return "\n".join(lines)


def to_json(report: Report) -> dict[str, object]:
    return {
        "records": report.records,
        "distinct_stored": report.distinct_stored,
        "keys_at_probes": report.keys_at_probes,
        "draw_keys": report.draw_keys,
        "keys_not_literal_base": report.keys_not_literal_base,
        "keys_not_in_variants": report.keys_not_in_variants,
        "base_keys_formed": report.base_keys_formed,
        "base_keys_total": report.base_keys_total,
        "batcher_calls": {hex(k): v for k, v in report.batcher_calls.items()},
        "batcher_sites_total": {hex(k): v for k, v in report.batcher_sites_total.items()},
        "batcher_sites_reached": {
            hex(k): [hex(a) for a in v] for k, v in report.batcher_sites_reached.items()
        },
        "nonliteral_sites_total": {hex(k): v for k, v in report.nonliteral_sites_total.items()},
        "nonliteral_sites_reached": {hex(k): v for k, v in report.nonliteral_sites_reached.items()},
        "literal_agreements": report.literal_agreements,
        "literal_disagreements": [hex(a) for a in report.literal_disagreements],
        "assembler_calls": [
            {
                "builder": c.builder,
                "key": hex(c.key),
                "length": c.length,
                "flags": hex(c.flags),
                "reconciled": c.reconciled,
            }
            for c in report.assembler_calls
        ],
        "loader_calls": report.loader_calls,
        "loader_pointer_classes": dict(report.loader_pointer_classes),
        "programs_loaded": [hex(a) for a in report.programs_loaded],
        "programs_total": report.programs_total,
        "unnamed_programs": [hex(a) for a in report.unnamed_programs],
        "unnamed_programs_loaded": [hex(a) for a in report.unnamed_programs_loaded],
        "pixel_installs": dict(report.pixel_installs),
        "pixel_definitions_total": report.pixel_definitions_total,
        "gaps": report.gaps,
        "stops": [
            {"thread": t, "reason": r, "address": a, "detail": d} for t, r, a, d in report.stops
        ],
    }
