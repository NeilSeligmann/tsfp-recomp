# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the corpus of vertex programs the title can run, into a GITIGNORED directory.

Two populations, both derived from the user's own executable and therefore never to be
committed (`docs/provenance.md`, `tools/ci/check-no-disc-data.sh`):

  * static: the programs stored in the XBE `.data`, found through their header dword;
  * generated: the microcode the title's own `XGAssembleShader` emits, under emulation
    (`tools/shaderscan/assemble.py`), for every source the title's own builder can form.
    The "reachable" subset is the one `tools/shaderscan` estimates from the key literals.

Output goes under `--out` (default `generated/shaders/corpus`) as `*.bin` files named by
digest, plus `manifest.json` with counts and flags only. The address arguments belong to
this build and have its values as defaults, as `docs/shader-inputs.md` section 10 lists.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from argparse import Namespace
from pathlib import Path

from tools.shaderscan import builders, vsh
from tools.shaderscan.assemble import (
    ORDINAL_RAISE_EXCEPTION,
    AssemblerEmulator,
    AssemblerSpec,
    Outcome,
)
from tools.shaderscan.cli import _reachable
from tools.shaderscan.image import Image
from tools.xdk_abi import SectionMap, executable_sections, walk_function

VERTEX_BUILDER = ("vertex", 0x208B0, "ebx", 0xFFFD9FFF)
ASSEMBLER = 0x3EE2B3
HEAP = (0x383678, 0x383DF3)
KEY_GLOBAL = 0x4B85E4
WRAPPER_LITERALS = ("0x1cd90:esi:0x1cfe2", "0x1d0b0:esi:0x1d2d0")
REACHABLE_SITE = (0x18EA0, 3)


def static_programs(image: Image) -> list[tuple[int, bytes]]:
    """`(virtual address, header + instructions)` for every headed program in the image."""
    found: list[tuple[int, bytes]] = []
    for section in image.xbe.sections:
        body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
        for program in vsh.find_headed_programs(body):
            end = program.offset + 4 + program.instructions * vsh.INSTRUCTION_BYTES
            found.append((section.virtual_addr + program.offset, body[program.offset : end]))
    return found


#: Programs the D3D library uploads itself, kept in the image WITHOUT the header dword the title's
#: own programs carry, so `static_programs` cannot find them. `(name, virtual address, instruction
#: count)`, each read from the image bytes. The Swap copy composition's vertex program (T443,
#: `0x003D5C10` loads it, docs/d3d8-copy-composition.md): two instructions at `0x003E1CE0`, the
#: position and texture coordinate 0 passed through.
LIBRARY_PROGRAMS = (("swap-copy", 0x3E1CE0, 2),)
PROGRAM_HEADER_VERSION = 0x2078  # the header's low u16, gpu_pgraph_replay.h PROGRAM_HEADER


def library_programs(image: Image) -> list[tuple[str, int, bytes]]:
    """`(name, virtual address, header + instructions)` for each of `LIBRARY_PROGRAMS`, the header
    made here.

    The digest the replay names a module by is the SHA-256 of `0x2078, count, instructions`, so a
    program stored without a header is wrapped in the same two u16 and then equals what the title's
    header-carrying programs would hash to."""
    found: list[tuple[str, int, bytes]] = []
    for name, address, count in LIBRARY_PROGRAMS:
        body = image.read(address, count * vsh.INSTRUCTION_BYTES)
        if body is None:
            raise SystemExit(f"library program {name} at {address:#x} is not inside the image")
        header = (count << 16) | PROGRAM_HEADER_VERSION
        found.append((name, address, header.to_bytes(4, "little") + body))
    return found


def generated_programs(
    image: Image, xbe_path: Path, log: object = sys.stderr
) -> tuple[dict[str, bytes], set[str], dict[str, int], dict[int, str], int]:
    """Assemble every source the vertex builder can emit.

    Returns `(outputs by digest, digests of the reachable subset, outcome counts,
    assembled digest by effective key, effective key mask)`.
    """
    name, entry, register, mask = VERTEX_BUILDER
    sections = SectionMap(executable_sections(xbe_path))
    walk = walk_function(sections, entry)
    if not walk.clean:
        raise SystemExit("vertex builder control-flow walk is not clean, refusing")
    tested = builders.tested_bits([(i.mnemonic, i.op_str) for i in walk.insns], register)
    effective = tested & mask
    spec = builders.BuilderSpec(name, entry, register, ASSEMBLER)
    emulator = builders.BuilderEmulator(image, spec)
    everything = builders.enumerate_keys(emulator, builders.subsets(effective), keep_sources=True)
    reach_args = Namespace(
        reachable=REACHABLE_SITE,
        section=".text",
        wrapper_literals=list(WRAPPER_LITERALS),
        key_global=KEY_GLOBAL,
    )
    _, reachable = _reachable(reach_args, image, emulator, mask)
    reachable_sources = set(reachable.by_digest)
    assembler = AssemblerEmulator(image, AssemblerSpec(ASSEMBLER, *HEAP))
    outputs: dict[str, bytes] = {}
    reachable_outputs: set[str] = set()
    # `unfinished` is a budget expiry, fault or bad return: the original produced no
    # HRESULT, so it is neither assembled nor rejected (T202).
    outcomes = {"assembled": 0, "rejected": 0, "unfinished": 0, "other": 0}
    assembled_by_source: dict[str, str] = {}
    for done, (digest, (source, flags)) in enumerate(everything.sources.items(), 1):
        run = assembler.run(source, flags)
        result = run.result
        if run.outcome not in (Outcome.RETURNED, Outcome.KERNEL_CALL) or result is None:
            outcomes["unfinished"] += 1
        elif result.kernel_call is not None and result.kernel_call != ORDINAL_RAISE_EXCEPTION:
            outcomes["other"] += 1
        elif result.kernel_call == ORDINAL_RAISE_EXCEPTION:
            outcomes["rejected"] += 1
        elif result.status != 0 or not result.data:
            outcomes["other"] += 1
        else:
            outcomes["assembled"] += 1
            outputs[result.digest] = result.data
            assembled_by_source[digest] = result.digest
            if digest in reachable_sources:
                reachable_outputs.add(result.digest)
        if done % 2000 == 0:
            print(f"  assembled {done} of {len(everything.sources)}", file=log)  # type: ignore[arg-type]
    keys = {
        key: assembled_by_source[source]
        for key, source in everything.key_digest.items()
        if source in assembled_by_source
    }
    return outputs, reachable_outputs, outcomes, keys, effective


def build(xbe: Path, out: Path) -> dict[str, object]:
    image = Image.load(xbe)
    out.mkdir(parents=True, exist_ok=True)
    (out / "static").mkdir(exist_ok=True)
    (out / "generated").mkdir(exist_ok=True)
    statics = static_programs(image) + [
        (address, data) for _, address, data in library_programs(image)
    ]
    static_digests: dict[str, list[int]] = {}
    for address, data in statics:
        digest = hashlib.sha256(data).hexdigest()
        static_digests.setdefault(digest, []).append(address)
        (out / "static" / f"{digest}.bin").write_bytes(data)
    outputs, reachable, outcomes, keys, key_mask = generated_programs(image, xbe)
    for digest, data in outputs.items():
        (out / "generated" / f"{digest}.bin").write_bytes(data)
    manifest: dict[str, object] = {
        "static_found": len(statics),
        "static_distinct": len(static_digests),
        "static": {d: [hex(a) for a in addrs] for d, addrs in static_digests.items()},
        "generated_distinct": len(outputs),
        "generated_reachable": sorted(reachable),
        "assembler_outcomes": outcomes,
        "key_mask": key_mask,
        "generated_keys": {hex(key): digest for key, digest in sorted(keys.items())},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    return manifest


def load(out: Path) -> tuple[dict[str, bytes], dict[str, bytes], set[str]]:
    """`(static by digest, generated by digest, reachable generated digests)` from `build`."""
    manifest = json.loads((out / "manifest.json").read_text())
    static = {p.stem: p.read_bytes() for p in sorted((out / "static").glob("*.bin"))}
    generated = {p.stem: p.read_bytes() for p in sorted((out / "generated").glob("*.bin"))}
    return static, generated, set(manifest["generated_reachable"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path)
    parser.add_argument("--out", type=Path, default=Path("generated/shaders/corpus"))
    args = parser.parse_args(argv)
    manifest = build(args.xbe, args.out)
    print(
        f"static {manifest['static_found']} found, {manifest['static_distinct']} distinct; "
        f"generated {manifest['generated_distinct']} distinct, "
        f"{len(manifest['generated_reachable'])} reachable; "  # type: ignore[arg-type]
        f"outcomes {manifest['assembler_outcomes']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
