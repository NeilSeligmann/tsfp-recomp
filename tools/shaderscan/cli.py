# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for `tools.shaderscan`: what feeds the title's shader pipeline?

    python -m tools.shaderscan.cli sites     XBE --target 0x3ee2b3 --nargs 11
    python -m tools.shaderscan.cli programs  XBE --loader 0x226e0
    python -m tools.shaderscan.cli builders  XBE --builder vertex:0x208b0:ebx:0xfffd9fff
    python -m tools.shaderscan.cli disc      --iso discs/tsfp-xbox.iso

Every mode is read-only and prints counts, structure and addresses. It never prints the
bytes of a shader, a program or a source string: those are derived from the user's own
executable and disc and must not enter a committed file (`docs/provenance.md`).

The addresses the modes need are arguments, not constants, so nothing about the retail
image is baked into the tool. `docs/shader-inputs.md` records the command lines that
reproduce each figure it quotes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pickle
import sys
from collections import Counter
from collections.abc import Sequence
from pathlib import Path

import capstone

from tools.gen_d3d8_surface import decode_section
from tools.shaderscan import callargs, vsh
from tools.shaderscan.assemble import (
    ORDINAL_RAISE_EXCEPTION,
    Assembled,
    AssemblerEmulator,
    AssemblerSpec,
    assemble_many,
    default_jobs,
)
from tools.shaderscan.builders import BuilderEmulator, Enumeration, enumerate_keys
from tools.shaderscan.image import Image


def _hex(text: str) -> int:
    return int(text, 0)


def _hex_list(text: str) -> list[int]:
    return [_hex(part) for part in text.split(",") if part]


def _decode(image: Image, name: str) -> list[capstone.CsInsn]:
    section = image.xbe.section_by_name(name)
    if section is None:
        raise SystemExit(f"no section named {name!r}")
    body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
    insns, _ = decode_section(body, section.virtual_addr)
    return insns


def _executable_names(image: Image) -> list[str]:
    return [section.name for section in image.xbe.sections if section.executable]


def _describe(arg: callargs.ArgValue) -> str:
    if arg.kind == callargs.KIND_IMM and arg.value is not None:
        return f"imm {arg.value:#x}"
    if arg.kind == callargs.KIND_GLOBAL_LOAD and arg.value is not None:
        return f"global_load [{arg.value:#x}]"
    return arg.kind


# ---------------------------------------------------------------------------
# sites
# ---------------------------------------------------------------------------


def cmd_sites(args: argparse.Namespace) -> int:
    image = Image.load(args.xbe)
    targets = set(args.target)
    insns = _decode(image, args.section)
    sites = callargs.find_sites(insns, targets, args.nargs)
    print(f"{len(sites)} direct sites to {sorted(hex(t) for t in targets)} in {args.section}")
    print(
        f"indirect call/jmp instructions in {args.section}: {callargs.count_indirect_calls(insns)}"
    )
    per_argument: dict[str, object] = {}
    report: dict[str, object] = {"sites": len(sites), "arguments": per_argument}
    for number in range(1, args.nargs + 1):
        kinds = callargs.census(sites, number)
        print(f"  arg {number}: {dict(sorted(kinds.items()))}")
        pointer_classes = Counter(
            callargs.pointer_class(image, site.args[number - 1]) for site in sites
        )
        immediates = Counter(
            site.args[number - 1].value for site in sites if site.args[number - 1].kind == "imm"
        )
        per_argument[str(number)] = {
            "kinds": dict(kinds),
            "pointer_classes": dict(pointer_classes),
            "distinct_immediates": len(immediates),
        }
        if number in args.detail:
            print(f"    pointer classes {dict(sorted(pointer_classes.items()))}")
            print(f"    distinct immediates {len(immediates)}")
            for value, count in sorted(
                immediates.items(), key=lambda item: (-item[1], item[0] or 0)
            ):
                print(f"      {value:#x} x{count}")
    if args.list_sites:
        for site in sites:
            print(f"  {site.va:#010x} {site.kind}: " + ", ".join(_describe(a) for a in site.args))
    _write_json(args.json, report)
    return 0


# ---------------------------------------------------------------------------
# assembler
# ---------------------------------------------------------------------------


def cmd_assembler(args: argparse.Namespace) -> int:
    """Classify what each assembler call site passes as its source buffer."""
    image = Image.load(args.xbe)
    target = {args.target}
    total = 0
    classes: Counter[str] = Counter()
    print(f"assembler {args.target:#x}")
    for name in _executable_names(image):
        insns = _decode(image, name)
        sites = callargs.find_sites(insns, target, args.nargs)
        total += len(sites)
        for site in sites:
            source = site.args[args.source_arg - 1]
            label = callargs.pointer_class(image, source)
            classes[label] += 1
            print(
                f"  {site.va:#010x} in {name}: source {_describe(source)} -> {label}, "
                f"length {_describe(site.args[args.length_arg - 1])}, "
                f"flags {_describe(site.args[args.flags_arg - 1])}"
            )
    print(f"{total} sites. source classes: {dict(sorted(classes.items()))}")
    _write_json(args.json, {"sites": total, "source_classes": dict(classes)})
    return 0


# ---------------------------------------------------------------------------
# programs
# ---------------------------------------------------------------------------


def cmd_programs(args: argparse.Namespace) -> int:
    image = Image.load(args.xbe)
    found: list[tuple[str, int, vsh.HeadedProgram]] = []
    for section in image.xbe.sections:
        body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
        for program in vsh.find_headed_programs(body):
            found.append((section.name, section.virtual_addr + program.offset, program))
    by_section = Counter(name for name, _, _ in found)
    print(f"{len(found)} headed programs, {sum(p.instructions for _, _, p in found)} instructions")
    print(f"  by section: {dict(by_section)}")
    print(f"  by header version: {dict(Counter(f'{p.version:#x}' for _, _, p in found))}")

    programs = []
    for _, address, program in found:
        start = image.xbe.va_to_offset(address)
        assert start is not None
        data = image.raw[start + 4 : start + 4 + program.instructions * vsh.INSTRUCTION_BYTES]
        programs.append(vsh.parse_instructions(data))
    result = vsh.characterise(programs)
    _print_characterisation(result)
    coincidence = vsh.arl_coincidence(programs)
    print(
        f"  relative constant reads inside programs that contain an ARL: "
        f"{coincidence.relative_reads_in_arl_programs} of {coincidence.relative_reads}, "
        f"those programs hold {coincidence.instructions_in_arl_programs} of "
        f"{coincidence.instructions} instructions, so independent placement would give "
        f"about {coincidence.null_probability:.1e}"
    )
    if args.headerless:
        _headerless(image, args.seed)
    print("  null model for the structural rules (uniformly random 128-bit words):")
    print(f"    full predicate           {vsh.null_pass_rate(args.null_trials, args.seed):.3e}")
    print(
        f"    dword 0 forced to zero   "
        f"{vsh.null_pass_rate(args.null_trials, args.seed, zero_first=True):.4f}"
    )

    report: dict[str, object] = {
        "programs": len(found),
        "instructions": result.instructions,
        "arl_instructions": result.arl_instructions,
    }
    if args.loader:
        report["loader"] = _cross_reference(image, args, {address for _, address, _ in found})
    _write_json(args.json, report)
    return 0


def _headerless(image: Image, seed: int) -> None:
    """Programs found by structure alone, outside every headed program, per section.

    The check that no program hides without a header. The control is the same scan over
    the section's dwords shuffled, which keeps the value distribution and destroys the
    structure, so a count that does not exceed the control is coincidence.
    """
    print("  structural scan with no header (programs outside every headed one):")
    for section in image.xbe.sections:
        body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
        if len(body) < vsh.INSTRUCTION_BYTES:
            continue
        covered = {
            offset
            for found in vsh.find_headed_programs(body)
            for offset in range(found.offset, found.offset + 4 + found.instructions * 16, 4)
        }
        cells = []
        for minimum in (2, 4, 8):
            outside = [
                span
                for span in vsh.find_programs(body, minimum=minimum)
                if span.offset not in covered
            ]
            control = len(vsh.shuffled_dword_scan(body, seed, minimum))
            cells.append(f"min {minimum}: {len(outside)} (shuffled control {control})")
        print(f"    {section.name:<14} " + ", ".join(cells))


def _print_characterisation(result: vsh.Characterisation) -> None:
    print("characterisation (counts only):")
    print(f"  program length histogram: {dict(sorted(result.lengths.items()))}")
    print(f"  MAC operations: {dict(result.mac_ops.most_common())}")
    print(f"  ILU operations: {dict(result.ilu_ops.most_common())}")
    print(f"  instructions issuing both a MAC and an ILU operation: {result.dual_issue}")
    print(f"  ARL: {result.arl_instructions} instructions in {result.programs_with_arl} programs")
    print(
        f"  relative constant reads: {result.relative_reads} instructions in "
        f"{result.programs_with_relative} programs"
    )
    print(
        f"  constant reads: {result.constant_reads} in {result.programs_reading_constants} "
        f"programs, highest register {result.highest_constant} of {vsh.CONSTANT_FILE_SIZE}"
    )
    print(
        f"  programs by distinct constants named: {dict(sorted(result.distinct_constants.items()))}"
    )
    print(f"  input registers read: {dict(sorted(result.input_registers.items()))}")
    print(
        f"  output register writes: {dict(sorted(result.output_addresses.items()))}, "
        f"reserved addresses {result.reserved_output_writes}, "
        f"constant-file destinations {result.constant_file_writes}"
    )
    print(
        f"  structural defects: {dict(result.defects)}, FINAL bit misplaced "
        f"{result.final_bit_misplaced}"
    )


def _cross_reference(image: Image, args: argparse.Namespace, addresses: set[int]) -> dict[str, int]:
    """Which found programs are named by a static pointer at the loader's call sites?"""
    from tools.shaderscan import xrefs

    insns = _decode(image, args.section)
    sites = callargs.find_sites(insns, {args.loader}, args.loader_args)
    # A literal that is not an image address (0, a count) is not a static program.
    pointers = Counter(
        site.args[0].value
        for site in sites
        if site.args[0].kind == callargs.KIND_IMM
        and site.args[0].value is not None
        and image.looks_like_pointer(site.args[0].value)
    )
    runtime = len(sites) - sum(pointers.values())
    referenced = {p for p in pointers if p in addresses}
    stray = {p for p in pointers if p not in addresses}
    unreferenced = addresses - referenced
    code = xrefs.code_references(insns, unreferenced)
    data = xrefs.data_references(image, unreferenced)
    print(f"loader {args.loader:#x}: {len(sites)} sites")
    print(f"  static pointer argument {sum(pointers.values())}, other {runtime}")
    print(
        f"  distinct static pointers {len(pointers)}, "
        f"of which are a found program {len(referenced)}"
    )
    print(f"  static pointers that are NOT a found program {len(stray)}")
    print(f"  found programs never named at a loader site {len(unreferenced)}")
    print(
        f"    of those, with any other code reference {sum(1 for a in unreferenced if code[a])}, "
        f"with any data-dword reference {sum(1 for a in unreferenced if data[a])}, "
        f"with neither {sum(1 for a in unreferenced if not code[a] and not data[a])}"
    )
    return {
        "sites": len(sites),
        "static": sum(pointers.values()),
        "runtime": runtime,
        "distinct_static": len(pointers),
        "referenced_programs": len(referenced),
        "stray_pointers": len(stray),
        "unreferenced_programs": len(unreferenced),
    }


# ---------------------------------------------------------------------------
# builders
# ---------------------------------------------------------------------------


def cmd_builders(args: argparse.Namespace) -> int:
    from tools.shaderscan import builders
    from tools.xdk_abi import SectionMap, executable_sections, walk_function

    image = Image.load(args.xbe)
    sections = SectionMap(executable_sections(args.xbe))
    assembler = _assembler(image, args) if args.heap else None
    report: dict[str, object] = {}
    for spec_text in args.builder:
        name, entry_text, register, mask_text = spec_text.split(":")
        entry, mask = _hex(entry_text), _hex(mask_text)
        spec = builders.BuilderSpec(name, entry, register, args.assembler)
        walk = walk_function(sections, entry)
        if not walk.clean:
            print(f"{name}: control-flow walk is not clean, refusing", file=sys.stderr)
            return 1
        tested = builders.tested_bits([(i.mnemonic, i.op_str) for i in walk.insns], register)
        effective = tested & mask
        emulator = builders.BuilderEmulator(image, spec)
        disagreements = builders.probe_dead_bits(emulator, mask, effective, args.probe, args.seed)
        keep = assembler is not None
        everything = builders.enumerate_keys(
            emulator, builders.subsets(effective), keep_sources=keep
        )
        print(f"builder {name} at {entry:#x}: {len(walk.insns)} instructions walked")
        print(f"  key bits tested {tested:#010x}, masked by {mask:#010x} -> {effective:#010x}")
        print(f"  {bin(effective).count('1')} effective bits, {everything.keys_run} keys")
        print(
            f"  dead-bit probe: {disagreements} of {args.probe} random keys changed output "
            f"when bits outside the effective set were cleared"
        )
        print(
            f"  distinct emissions {everything.distinct}, run failures {everything.failures}, "
            f"never reached the assembler {everything.no_call}"
        )
        sizes = sorted(set(everything.lengths.values()))
        print(f"  emission sizes in bytes: {sizes[:3]}.. {sizes[-3:]}, {len(sizes)} distinct")
        print(f"  assembler flags argument values seen: {dict(everything.flag_values)}")
        data_low, data_high = _static_data_span(image)
        sample = builders.sample_keys(effective, args.fragment_sample, args.seed)
        touched = emulator.static_reads(sample, data_low, data_high)
        doubled = emulator.static_reads(
            builders.sample_keys(effective, 2 * args.fragment_sample, args.seed + 1),
            data_low,
            data_high,
        )
        regions = builders.contiguous_regions(touched | doubled)
        print(
            f"  static bytes read: {len(touched)} over {args.fragment_sample} sampled keys, "
            f"{len(doubled)} over {2 * args.fragment_sample} fresh ones, "
            f"{len(touched | doubled)} in {len(regions)} contiguous regions in the union "
            f"(adjacent fragments merge, so the region count is a lower bound on fragments)"
        )
        touched = touched | doubled
        entry_report: dict[str, object] = {
            "effective_bits": bin(effective).count("1"),
            "keys": everything.keys_run,
            "distinct": everything.distinct,
            "dead_bit_disagreements": disagreements,
            "static_bytes_read": len(touched),
            "static_regions": len(regions),
        }
        reachable_digests: set[str] | None = None
        if args.reachable:
            summary, reachable = _reachable(args, image, emulator, mask)
            entry_report["reachable"] = summary
            reachable_digests = set(reachable.by_digest)
        if assembler is not None:
            entry_report["assembled"] = _assembled(
                name, assembler, everything, reachable_digests, image, args
            )
        report[name] = entry_report
    _write_json(args.json, report)
    return 0


def _static_data_span(image: Image) -> tuple[int, int]:
    """Lowest and highest initialised address of the non-executable-library data sections."""
    spans = [
        (section.virtual_addr, section.virtual_addr + section.raw_size - 1)
        for section in image.xbe.sections
        if section.name in (".rdata", ".data")
    ]
    return min(low for low, _ in spans), max(high for _, high in spans)


def _assembler(image: Image, args: argparse.Namespace) -> AssemblerEmulator:

    alloc, free = (_hex(part) for part in args.heap.split(":"))
    return AssemblerEmulator(image, AssemblerSpec(args.assembler, alloc, free))


def _assembled(
    name: str,
    assembler: AssemblerEmulator,
    enumeration: Enumeration,
    reachable_digests: set[str] | None,
    image: Image,
    args: argparse.Namespace,
) -> dict[str, object]:
    """Run the real assembler over every distinct source and characterise the outputs."""
    outputs: dict[str, Assembled] = {}
    outcomes: Counter[str] = Counter()
    rejected: set[str] = set()
    digests = list(enumeration.sources)
    results = assemble_many(
        image,
        assembler.spec,
        [enumeration.sources[digest] for digest in digests],
        args.jobs,
        serial=assembler,
    )
    for digest, result in zip(digests, results, strict=True):
        if result is None:
            outcomes["emulation fault"] += 1
        elif result.kernel_call == ORDINAL_RAISE_EXCEPTION:
            outcomes["rejected (RtlRaiseException)"] += 1
            rejected.add(digest)
        elif result.kernel_call is not None:
            outcomes[f"other kernel call {result.kernel_call}"] += 1
        elif result.status != 0 or not result.data:
            outcomes[f"status {result.status:#x}"] += 1
        else:
            outcomes["assembled"] += 1
            outputs[digest] = result
    distinct = {result.digest for result in outputs.values()}
    print(f"  assembled {len(enumeration.sources)} distinct sources: {dict(outcomes)}")
    sizes = Counter(len(result.data) for result in outputs.values())
    print(
        f"    distinct outputs {len(distinct)}, output sizes in bytes {dict(sorted(sizes.items()))}"
    )
    summary: dict[str, object] = {
        "sources": len(enumeration.sources),
        "assembled": len(outputs),
        "outcomes": dict(outcomes),
        "distinct_outputs": len(distinct),
        # Over the ordered (digest, result) pairs, so a serial and a sharded run compare equal.
        "verdict_sha256": hashlib.sha256(
            pickle.dumps(list(zip(digests, results, strict=True)))
        ).hexdigest(),
    }
    groups = {"all": set(outputs)}
    if reachable_digests is not None:
        groups["reachable"] = {d for d in outputs if d in reachable_digests}
        refused = reachable_digests & rejected
        print(
            f"    of the {len(reachable_digests)} reachable sources: {len(groups['reachable'])} "
            f"assembled ({len({outputs[d].digest for d in groups['reachable']})} distinct "
            f"outputs), {len(refused)} rejected"
        )
        summary["reachable_assembled"] = len(groups["reachable"])
        summary["reachable_rejected"] = len(refused)
    if outputs and all(vsh_header(result.data) is not None for result in outputs.values()):
        for label, members in groups.items():
            summary[label] = _characterise_outputs(
                [outputs[d].data for d in sorted(members)], label
            )
    return summary


def vsh_header(data: bytes) -> int | None:
    """Instruction count if `data` is exactly one headed vertex program, else None."""
    if len(data) < 4:
        return None
    word = int.from_bytes(data[:4], "little")
    count = word >> 16
    if word & 0xFFFF not in vsh.HEADER_VERSIONS or len(data) != 4 + count * vsh.INSTRUCTION_BYTES:
        return None
    return count


def _characterise_outputs(blobs: list[bytes], label: str) -> dict[str, int]:
    programs = []
    malformed = 0
    for data in blobs:
        count = vsh_header(data)
        if count is None:
            malformed += 1
            continue
        programs.append(vsh.parse_instructions(data[4:]))
    plausible = sum(all(vsh.is_plausible(i) for i in program) for program in programs)
    result = vsh.characterise(programs)
    print(
        f"    [{label}] {len(programs)} headed programs ({malformed} not headed), "
        f"{plausible} fully plausible, {result.instructions} instructions"
    )
    if programs:
        _print_characterisation(result)
    return {
        "programs": len(programs),
        "malformed": malformed,
        "plausible": plausible,
        "instructions": result.instructions,
        "arl_instructions": result.arl_instructions,
        "programs_with_arl": result.programs_with_arl,
    }


def _reachable(
    args: argparse.Namespace,
    image: Image,
    emulator: BuilderEmulator,
    mask: int,
) -> tuple[dict[str, int], Enumeration]:
    """Distinct emissions over keys the title's own literals can form.

    Base keys are the literals passed as the key argument at the key sites, plus the
    immediates a translation wrapper loads into its key register. Each is then varied
    by every modification the code applies to the key global: an `or` may or may not
    run, so each OR-ed bit is toggled, and an `and` may or may not run. That is a
    deliberate over-approximation of control flow (it ignores the conditions), so the
    count is an upper bound on what those literals can reach.
    """
    from tools.shaderscan import xrefs

    insns = _decode(image, args.section)
    target, argument = args.reachable
    sites = callargs.find_sites(insns, {target}, argument)
    literals = {s.args[argument - 1].value for s in sites if s.args[argument - 1].kind == "imm"}
    runtime = sum(1 for s in sites if s.args[argument - 1].kind != "imm")
    wrapper_literals: set[int] = set()
    for spec in args.wrapper_literals:
        entry_text, register, end_text = spec.split(":")
        entry, end = _hex(entry_text), _hex(end_text)
        prefix = f"{register}, 0x"
        wrapper_literals |= {
            int(i.op_str.split(", ")[1], 16)
            for i in insns
            if entry <= i.address < end
            and i.mnemonic == "mov"
            and i.op_str.startswith(prefix)
            and "[" not in i.op_str
        }
    base = {value for value in literals if value is not None} | wrapper_literals
    modifications = xrefs.global_modifications(insns, args.key_global) if args.key_global else {}
    ors = sorted({imm for (op, imm) in modifications if op == "or"})
    ands = sorted({imm for (op, imm) in modifications if op == "and"})
    keys: set[int] = set()
    for value in base:
        variants = {value}
        for bit in ors:
            variants |= {v | bit for v in variants}
        for clear in ands:
            variants |= {v & clear for v in variants}
        keys |= {v & mask for v in variants}
    result = enumerate_keys(emulator, sorted(keys))
    print(
        f"  reachable: {len(literals)} literal key values at {len(sites)} sites "
        f"({runtime} sites pass a non-literal), {len(wrapper_literals)} wrapper literals, "
        f"{len(base)} distinct base keys"
    )
    print(f"    key global modifications: or {[hex(x) for x in ors]}, and {[hex(x) for x in ands]}")
    print(f"    {len(keys)} masked keys -> {result.distinct} distinct emissions")
    summary = {
        "literal_values": len(literals),
        "runtime_sites": runtime,
        "wrapper_literals": len(wrapper_literals),
        "base_keys": len(base),
        "keys": len(keys),
        "distinct": result.distinct,
    }
    return summary, result


# ---------------------------------------------------------------------------
# combiners
# ---------------------------------------------------------------------------


def cmd_combiners(args: argparse.Namespace) -> int:
    """Count the distinct register-combiner configurations the title can install."""
    from tools.shaderscan import combiners as cb

    image = Image.load(args.xbe)
    text = _decode(image, args.section)
    leaders = callargs.branch_leaders(text)
    report: dict[str, object] = {}

    # Route 2: table-driven SetRenderState loops, classified by dispatch class.
    walks = [walk for walk in cb.find_array_walks(text) if walk.key_displacement is not None]
    print(f"table walks with a recognised key field: {len(walks)}")
    for walk in walks:
        try:
            indices = cb.read_walk_indices(image, walk)
        except ValueError:
            continue
        histogram = cb.index_histogram(indices)
        if histogram.get(cb.CLASS_OUT_OF_RANGE, 0) == len(indices):
            continue
        print(
            f"  walk at {walk.loop_va:#x}: {len(indices)} records of {walk.stride:#x} bytes "
            f"at {walk.start:#x}, indices by class {dict(histogram)}"
        )
        report[f"walk_{walk.loop_va:#x}"] = dict(histogram)

    # Route 1: the SetPixelShader wrapper, whole-program installs.
    by_address = {insn.address: position for position, insn in enumerate(text)}
    transfers = callargs.find_sites(text, {args.setter}, 1, leaders)
    arguments = [
        cb.tail_call_argument(text, by_address[site.va]) if site.kind == "jmp" else site.args[0]
        for site in transfers
    ]
    kinds = Counter(
        (site.kind, argument.kind) for site, argument in zip(transfers, arguments, strict=True)
    )
    instructions = len({site.va for site in transfers})
    print(
        f"SetPixelShader wrapper {args.setter:#x}: {len(transfers)} argument paths over "
        f"{instructions} transfer instructions {dict(kinds)}"
    )
    static = [a.value for a in arguments if a.kind == callargs.KIND_IMM and a.value is not None]
    distinct, readable = cb.distinct_contents(image, static, args.definition_size)
    print(
        f"  static definitions: {len(set(static))} distinct addresses, {distinct} distinct by "
        f"content ({readable} readable at {args.definition_size:#x} bytes)"
    )
    print(f"  spacing between definitions {dict(cb.definition_gaps(static).most_common(4))}")
    stages = cb.field_histogram(image, static, args.control_offset, args.control_mask)
    print(f"  combiner-control stage-count field histogram {dict(sorted(stages.items()))}")
    report["static_definitions"] = distinct

    # Route 3: the consumer of a definition, and direct one-header writes.
    if args.consumer:
        start_text, end_text, register = args.consumer.split(":")
        start, end = _hex(start_text), _hex(end_text)
        section = image.section_at(start)
        body = _decode(image, section.name) if section else []
        function = [i for i in body if start <= i.address < end]
        extent = cb.field_extent(function, register)
        print(
            f"consumer {start:#x}..{end:#x}: reads {len(extent.spans)} spans, block at least "
            f"{extent.size:#x} bytes"
        )
        for block in cb.copy_blocks(function):
            destination = f"{block.destination:#x}" if block.destination else "register"
            print(f"  copy of {block.dwords} dwords to {destination}")
    if args.header_primitive:
        sites = cb.header_sites(text, {args.header_primitive}, leaders)
        print(f"one-header writer {args.header_primitive:#x}: {len(sites)} sites")
        for site in sites:
            method = f"{site.method:#x}" if site.method else "not a literal"
            print(
                f"  {site.va:#x} {site.kind}: header {_describe(site.header)}, method {method}, "
                f"value {site.value.kind}"
            )

    if args.generator_reachable is not None:
        estimate = cb.estimate_configurations(
            len(set(static)), args.generator_reachable, args.cache_slots
        )
        print(
            f"estimate: lower {estimate.lower}, upper {estimate.upper} programs "
            f"(static {estimate.static_definitions}, generator ceiling "
            f"min({args.generator_reachable}, {args.cache_slots}))"
        )
        report["estimate"] = {"lower": estimate.lower, "upper": estimate.upper}
    _write_json(args.json, report)
    return 0


# ---------------------------------------------------------------------------
# disc
# ---------------------------------------------------------------------------


def cmd_disc(args: argparse.Namespace) -> int:
    from tools.shaderscan.disc import scan_disc

    scan = scan_disc(args.iso)
    print(f"{scan.files} files, {scan.bytes_scanned} bytes scanned")
    print(
        f"  {scan.pak_files} paks, {scan.pak_entries} entries, "
        f"{scan.pak_parse_failures} paks that did not parse, {scan.entry_failures} entry failures"
    )
    print(f"  vertex programs found: {scan.programs} ({scan.instructions} instructions)")
    for path, count in sorted(scan.hits_by_path.items()):
        print(f"    {path}: {count}")
    _write_json(args.json, {"files": scan.files, "programs": scan.programs})
    return 0


# ---------------------------------------------------------------------------


def _write_json(path: Path | None, payload: object) -> None:
    if path is not None:
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.shaderscan.cli",
        description="Measure what feeds the title's shader pipeline. Counts only.",
    )
    sub = parser.add_subparsers(dest="mode", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--json", type=Path, help="also write the figures as JSON here")

    sites = sub.add_parser("sites", help="classify the arguments at call sites of a target")
    sites.add_argument("xbe", type=Path)
    sites.add_argument("--target", type=_hex_list, required=True, help="hex, comma separated")
    sites.add_argument("--nargs", type=int, required=True, help="stack arguments to trace")
    sites.add_argument("--section", default=".text", help="section holding the callers")
    sites.add_argument("--detail", type=int, nargs="*", default=[], help="arguments to expand")
    sites.add_argument("--list-sites", action="store_true")
    common(sites)
    sites.set_defaults(func=cmd_sites)

    assembler = sub.add_parser("assembler", help="what do assembler call sites pass?")
    assembler.add_argument("xbe", type=Path)
    assembler.add_argument("--target", type=_hex, required=True)
    assembler.add_argument("--nargs", type=int, default=11)
    assembler.add_argument("--source-arg", type=int, default=2)
    assembler.add_argument("--length-arg", type=int, default=3)
    assembler.add_argument("--flags-arg", type=int, default=4)
    common(assembler)
    assembler.set_defaults(func=cmd_assembler)

    programs = sub.add_parser("programs", help="find and characterise vertex programs")
    programs.add_argument("xbe", type=Path)
    programs.add_argument("--loader", type=_hex, help="loader wrapper address, to cross-reference")
    programs.add_argument("--loader-args", type=int, default=3)
    programs.add_argument("--section", default=".text")
    programs.add_argument("--headerless", action="store_true", help="also scan by structure alone")
    programs.add_argument("--null-trials", type=int, default=1_000_000)
    programs.add_argument("--seed", type=int, default=1)
    common(programs)
    programs.set_defaults(func=cmd_programs)

    builders = sub.add_parser("builders", help="enumerate what the source builders can emit")
    builders.add_argument("xbe", type=Path)
    builders.add_argument(
        "--builder",
        action="append",
        required=True,
        metavar="NAME:ENTRY:KEYREG:MASK",
        help="e.g. vertex:0x208b0:ebx:0xfffd9fff, MASK being what the caller ANDs the key with",
    )
    builders.add_argument("--assembler", type=_hex, required=True)
    builders.add_argument("--probe", type=int, default=400, help="random dead-bit probe samples")
    builders.add_argument(
        "--fragment-sample",
        type=int,
        default=2000,
        help="keys sampled to find the static fragments",
    )
    builders.add_argument("--seed", type=int, default=1)
    builders.add_argument(
        "--reachable",
        type=lambda text: (_hex(text.split(":")[0]), int(text.split(":")[1])),
        metavar="TARGET:ARG",
        help="also count emissions for keys formed from the literals passed as ARG at TARGET",
    )
    builders.add_argument(
        "--key-global",
        type=_hex,
        help="address of the global the key is kept in, to find the bits the title ORs/ANDs in",
    )
    builders.add_argument(
        "--wrapper-literals",
        action="append",
        default=[],
        metavar="ENTRY:REG:END",
        help="also take the immediates loaded into REG between ENTRY and END as base keys. "
        "A linear range, because the wrappers dispatch through jump tables a flow walk "
        "does not follow",
    )
    builders.add_argument(
        "--heap",
        metavar="ALLOC:FREE",
        help="also run the assembler under emulation, with the title's heap allocate and free "
        "routines (hex addresses) replaced by a bump allocator and a no-op",
    )
    builders.add_argument(
        "--jobs",
        type=int,
        default=default_jobs(),
        help="independent emulator processes for the assembler corpus audit "
        "(default min(cpu count, 8)); verdicts merge in corpus order, identical to --jobs 1",
    )
    builders.add_argument("--section", default=".text")
    common(builders)
    builders.set_defaults(func=cmd_builders)

    combiners = sub.add_parser("combiners", help="count register-combiner configurations")
    combiners.add_argument("xbe", type=Path)
    combiners.add_argument("--setter", type=_hex, required=True, help="game SetPixelShader wrapper")
    combiners.add_argument("--definition-size", type=_hex, default=0xF0)
    combiners.add_argument("--control-offset", type=_hex, default=0xD4)
    combiners.add_argument("--control-mask", type=_hex, default=0xF)
    combiners.add_argument(
        "--consumer", metavar="START:END:REG", help="function reading a definition"
    )
    combiners.add_argument("--header-primitive", type=_hex, help="one-header write primitive")
    combiners.add_argument(
        "--generator-reachable", type=int, help="distinct generated pixel sources"
    )
    combiners.add_argument("--cache-slots", type=int, default=110)
    combiners.add_argument("--section", default=".text")
    common(combiners)
    combiners.set_defaults(func=cmd_combiners)

    disc = sub.add_parser("disc", help="scan a disc image for vertex programs")
    disc.add_argument("--iso", type=Path, required=True)
    common(disc)
    disc.set_defaults(func=cmd_disc)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
