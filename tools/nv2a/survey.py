# SPDX-License-Identifier: GPL-3.0-or-later
"""Counts over a set of vertex programs: which operations, registers and patterns occur.

Counts only, never program bytes. Reads a corpus directory written by
`tools.nv2a.corpus` (gitignored). The statistics answer what a translator and the pixel
side need: instruction coverage, the output registers written, which inputs feed which
outputs, relative-addressing windows, and the shape of the position computation.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from collections.abc import Iterable
from pathlib import Path

from tools.nv2a import isa

#: Scalar-unit opcodes whose C operand is read as ONE component (LIT reads three).
SCALAR_ONE_COMPONENT = ("RCP", "RCC", "RSQ", "EXP", "LOG")


def operands_read(decoded: isa.Decoded) -> list[tuple[str, isa.Source]]:
    letters = isa.MAC_READS.get(decoded.mac_name, "") if decoded.mac < len(isa.MAC_NAMES) else ""
    if decoded.ilu:
        letters += "C"
    return [(letter, decoded.source(letter)) for letter in sorted(set(letters))]


MIRROR_TEMP = 12


def _mask_positions(mask: int) -> set[int]:
    return {position for position in range(4) if mask >> (3 - position) & 1}


def components_needed(decoded: isa.Decoded, letter: str) -> set[int]:
    """Source components of operand `letter` that the instruction's result depends on.

    Exact for the dot products, DST, the scalar unit and LIT, and the destination-mask
    positions for the component-wise operations, so a read of a register that was only
    partly written is not flagged for components nobody uses.
    """
    swizzle = decoded.source(letter).swizzle
    if letter == "C" and decoded.ilu:
        if decoded.ilu_name == "LIT":
            return {swizzle[0], swizzle[1], swizzle[3]}
        if decoded.ilu_name != "MOV":
            return {swizzle[0]}
        positions = _mask_positions(
            decoded.ilu_mask | (decoded.out_mask if decoded.out_is_ilu else 0)
        )
        return {swizzle[i] for i in positions}
    name = decoded.mac_name
    if name in ("DP3", "DPH"):
        return set(swizzle[:3])
    if name == "DP4":
        return set(swizzle)
    if name == "DST":
        return {swizzle[1], swizzle[2]} if letter == "A" else {swizzle[1], swizzle[3]}
    if name == "ARL":
        return {swizzle[0]}
    positions = _mask_positions(decoded.mac_mask | (0 if decoded.out_is_ilu else decoded.out_mask))
    return {swizzle[i] for i in positions}


def temps_written(decoded: isa.Decoded) -> set[int]:
    written: set[int] = set()
    if decoded.mac and decoded.mac != isa.MAC_ARL and decoded.mac_mask:
        written.add(decoded.temp_out)
    if decoded.ilu and decoded.ilu_mask:
        written.add(decoded.temp_out)
    return written


def survey(programs: Iterable[bytes]) -> dict[str, object]:
    """Statistics over `programs` (headed or bare byte strings)."""
    count = instructions = 0
    ops: Counter[str] = Counter()
    nop_only = raw_distance_one = 0
    dual_issue = 0
    out_masks: Counter[tuple[str, int]] = Counter()
    reserved_outputs = constant_writes = 0
    out_written_by_program: Counter[str] = Counter()
    inputs_to_outputs: Counter[tuple[int, str]] = Counter()
    relative_reads = programs_with_relative = 0
    relative_bases: set[int] = set()
    non_replicated_scalar = 0
    highest_temp = highest_const = -1
    position_rcc_programs = 0
    viewport_programs = 0
    max_length = 0
    mirror_readers = mirror_writers = mirror_read_after_position = 0
    for data in programs:
        program = isa.decode_program(data)
        count += 1
        instructions += len(program)
        max_length = max(max_length, len(program))
        seen_outputs: set[str] = set()
        has_relative = False
        reads_viewport = rcc_seen = False
        position_written = 0
        reads_mirror = writes_mirror = False
        mirror_consistent = True
        for index, decoded in enumerate(program):
            if decoded.mac == 0 and decoded.ilu == 0:
                nop_only += 1
            if decoded.mac and decoded.ilu:
                dual_issue += 1
            if decoded.mac:
                ops[f"MAC:{decoded.mac_name}"] += 1
            if decoded.ilu:
                ops[f"ILU:{decoded.ilu_name}"] += 1
                rcc_seen = rcc_seen or decoded.ilu_name == "RCC"
                if decoded.ilu_name in SCALAR_ONE_COMPONENT and len(set(decoded.c.swizzle)) != 1:
                    non_replicated_scalar += 1
            reads = operands_read(decoded)
            if any(source.mux == isa.MUX_CONST for _, source in reads):
                highest_const = max(highest_const, decoded.const)
                if decoded.relative:
                    relative_reads += 1
                    has_relative = True
                    relative_bases.add(decoded.const)
                if decoded.out_to_output and decoded.out_address == 0 and decoded.const in (58, 59):
                    reads_viewport = True
            for letter, source in reads:
                if source.mux == isa.MUX_TEMP:
                    highest_temp = max(highest_temp, source.temp)
                    if source.temp == MIRROR_TEMP:
                        reads_mirror = True
                        needed = components_needed(decoded, letter)
                        written = _mask_positions(position_written)
                        mirror_consistent = mirror_consistent and needed <= written
            if decoded.mac_mask or decoded.ilu_mask:
                highest_temp = max(highest_temp, decoded.temp_out)
                writes_mirror = writes_mirror or decoded.temp_out == MIRROR_TEMP
            if index + 1 < len(program):
                following = program[index + 1]
                if temps_written(decoded) & {
                    s.temp for _, s in operands_read(following) if s.mux == isa.MUX_TEMP
                }:
                    raw_distance_one += 1
            if decoded.out_mask and decoded.out_to_output and decoded.out_address == 0:
                position_written |= decoded.out_mask
            if decoded.out_mask:
                if not decoded.out_to_output:
                    constant_writes += 1
                    continue
                name = isa.OUTPUT_NAMES.get(decoded.out_address)
                if name is None:
                    reserved_outputs += 1
                    name = f"reserved{decoded.out_address}"
                out_masks[(name, decoded.out_mask)] += 1
                seen_outputs.add(name)
                for _, source in reads:
                    if source.mux == isa.MUX_INPUT:
                        inputs_to_outputs[(decoded.input, name)] += 1
        mirror_readers += reads_mirror
        mirror_writers += writes_mirror
        mirror_read_after_position += reads_mirror and mirror_consistent
        programs_with_relative += has_relative
        viewport_programs += reads_viewport
        position_rcc_programs += reads_viewport and rcc_seen
        for name in seen_outputs:
            out_written_by_program[name] += 1
    return {
        "programs": count,
        "instructions": instructions,
        "longest": max_length,
        "operations": dict(sorted(ops.items())),
        "nop_only_instructions": nop_only,
        "dual_issue_instructions": dual_issue,
        "raw_distance_one": raw_distance_one,
        "output_masks": {f"{name}:{mask:#x}": n for (name, mask), n in sorted(out_masks.items())},
        "programs_writing": dict(sorted(out_written_by_program.items())),
        "reserved_output_writes": reserved_outputs,
        "constant_file_writes": constant_writes,
        "input_to_output_reads": {
            f"v{inp}->{name}": n for (inp, name), n in sorted(inputs_to_outputs.items())
        },
        "relative_constant_reads": relative_reads,
        "programs_with_relative": programs_with_relative,
        "relative_base_min": min(relative_bases) if relative_bases else None,
        "relative_base_max": max(relative_bases) if relative_bases else None,
        "scalar_sources_not_replicated": non_replicated_scalar,
        "highest_temp": highest_temp,
        "highest_constant_named": highest_const,
        "programs_reading_viewport_into_position": viewport_programs,
        "of_which_with_rcc": position_rcc_programs,
        "programs_reading_r12": mirror_readers,
        "programs_writing_r12": mirror_writers,
        "programs_whose_r12_reads_follow_position_writes": mirror_read_after_position,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path("generated/shaders/corpus"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    result: dict[str, object] = {}
    for label, pattern in (("static", "static/*.bin"), ("generated", "generated/*.bin")):
        paths = sorted(args.corpus.glob(pattern))
        result[label] = survey(path.read_bytes() for path in paths)
    text = json.dumps(result, indent=1)
    print(text)
    if args.json:
        args.json.write_text(text + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
