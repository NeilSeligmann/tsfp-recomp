#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lift the seven title-called XMV exports into separate private original aliases.

The XMV library is statically linked and its internals are already translated, so only
the seven surface entries (which the normal lift replaces with measured trampolines)
need an executable original body. Output is a gitignored chunk next to the lift,
recomp_xmv_original.c, plus a manifest binding it to the XBE and the translation inputs.
The host runs it only under --native-xmv. Provenance, not runtime attestation.
"""

from __future__ import annotations

import argparse
import importlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

from tools import retained_original
from tools.gen_xdk_manual_list import parse_surface, symbol_for
from tools.gen_xdk_original_bodies import ROOT, address, digest, strict_json, vendor_api
from tools.install_shader_original import derive_manual_map
from tools.shaderscan.image import Image

PROFILE = "xmv-original-v1"
REQUIRED = frozenset(
    (0x00444A2D, 0x00444F71, 0x00445055, 0x004450C2, 0x00445241, 0x00445252, 0x0044525D)
)
CHUNK = "recomp_xmv_original.c"
MANIFEST = "xmv_original_manifest.json"
SYMBOL = "recomp_xmv_original_{:08X}"


def load_inputs(entries: Path, manual: Path, surface: Path) -> tuple[list[int], dict[int, str]]:
    profile = strict_json(entries)
    if not isinstance(profile, dict) or set(profile) != {"schema", "profile", "addresses"}:
        raise ValueError("strict profile schema required")
    if (
        type(profile["schema"]) is not int
        or profile["schema"] != 1
        or profile["profile"] != PROFILE
    ):
        raise ValueError("unsupported original profile")
    if not isinstance(profile["addresses"], list):
        raise ValueError("addresses must be an array")
    selected = [address(v) for v in profile["addresses"]]
    if len(selected) != len(set(selected)) or set(selected) != REQUIRED:
        raise ValueError("the complete seven-export XMV profile is required")
    raw = strict_json(manual)
    if not isinstance(raw, dict) or not raw:
        raise ValueError("full pinned manual map required")
    mapping: dict[int, str] = {}
    for key, name in raw.items():
        va = address(key)
        if va in mapping or name != symbol_for(va):
            raise ValueError("manual map requires unique canonical pinned symbols")
        mapping[va] = name
    rows = parse_surface(surface)
    if len({row[0] for row in rows}) != len(rows):
        raise ValueError("duplicate surface address")
    sections = {row[0]: row[1] for row in rows}
    if any(sections.get(va) != "XMV" or va not in mapping for va in selected):
        raise ValueError("original entries must be normal XMV manual members")
    return sorted(selected), mapping


def alias_body(code: str, va: int, name: str) -> str:
    definition = rf"\bvoid {re.escape(name)}\(void\)"
    if (
        len(re.findall(definition, code)) != 1
        or "UNIMPLEMENTED" in code
        or "/* unimplemented" in code.lower()
    ):
        raise ValueError(f"unsupported or ambiguous translated body {va:#x}")
    return re.sub(definition, f"void {SYMBOL.format(va)}(void)", code, count=1)


def generate(
    *, xbe: Path, lifted: Path, manual: Path, entries: Path, surface: Path, vendor: Path, out: Path
) -> dict[str, Any]:
    def profile_for(_selected: list[int]) -> retained_original.Profile:
        return retained_original.Profile(
            "XMV",
            SYMBOL,
            "recomp_xmv_lookup_original",
            PROFILE,
            "recomp_xmv_profile_identity",
        )

    text, manifest = retained_original.translate(
        xbe=xbe,
        lifted=lifted,
        manual=manual,
        entries=entries,
        surface=surface,
        vendor=vendor,
        input_loader=load_inputs,
        input_builder=retained_original.input_paths,
        vendor_hasher=retained_original.vendor_source_hashes,
        profile_for=profile_for,
        image_loader=Image.load,
        vendor_loader=vendor_api,
        module_importer=importlib.import_module,
        alias_body=alias_body,
    )
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as temp:
        stage = Path(temp)
        (stage / CHUNK).write_text(text)
        (stage / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n")
        for name in (CHUNK, MANIFEST):
            os.replace(stage / name, out / name)
    return manifest


def verify_output(out_dir: Path, xbe_hash: str) -> dict[str, Any]:
    """Check that the generated chunk is the one its manifest describes."""
    chunk = (out_dir / CHUNK).read_bytes()
    manifest = json.loads((out_dir / MANIFEST).read_text())
    if not chunk:
        raise ValueError("generated original chunk is empty")
    if manifest.get("generated_chunk_sha256") != digest(chunk):
        raise ValueError("generated chunk does not match its manifest")
    if manifest.get("xbe_sha256") != xbe_hash or manifest.get("profile") != PROFILE:
        raise ValueError("manifest was generated from a different XBE or profile")
    bodies = manifest.get("bodies")
    if not isinstance(bodies, list) or len(bodies) != len(REQUIRED):
        raise ValueError("manifest does not record the seven XMV bodies")
    text = chunk.decode()
    for record in bodies:
        if record["symbol"] not in text:
            raise ValueError(f"chunk lacks manifest body {record['symbol']}")
    return manifest


def publish(out_dir: Path, gen_dir: Path) -> None:
    """Copy the verified pair into a lift gen directory, one atomic replace each."""
    if not (gen_dir / "recomp_xdk_manual.c").is_file():
        raise ValueError(f"{gen_dir} is not a lifted gen directory")
    for name in (CHUNK, MANIFEST):
        with tempfile.NamedTemporaryFile(dir=gen_dir, prefix=name + ".", delete=False) as handle:
            handle.write((out_dir / name).read_bytes())
            temp = Path(handle.name)
        os.replace(temp, gen_dir / name)


def install(
    *, xbe: Path, lifted: Path, entries: Path, work: Path, surface: Path, vendor: Path
) -> dict[str, Any]:
    """Derive the pinned manual map from the compiled chunk, generate, verify and publish."""
    gen_dir = lifted / "gen"
    mapping = derive_manual_map((gen_dir / "recomp_xdk_manual.c").read_text())
    work.mkdir(parents=True, exist_ok=True)
    manual = work / "xdk-manual-canonical.json"
    manual.write_text(json.dumps(mapping, indent=2) + "\n")
    out_dir = work / "xmv-original"
    generate(
        xbe=xbe,
        lifted=lifted,
        manual=manual,
        entries=entries,
        surface=surface,
        vendor=vendor,
        out=out_dir,
    )
    manifest = verify_output(out_dir, digest(xbe.read_bytes()))
    publish(out_dir, gen_dir)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True, help="the extracted retail default.xbe")
    parser.add_argument("--lifted-dir", type=Path, default=Path("generated/lifted"))
    parser.add_argument(
        "--manual-functions",
        type=Path,
        help="pinned manual map (default with --install: derived from the compiled chunk)",
    )
    parser.add_argument(
        "--original-entries", type=Path, default=ROOT / "tools/data/xmv_original_entries.json"
    )
    parser.add_argument(
        "--out-dir", type=Path, help="generate here only (default with --install: the work dir)"
    )
    parser.add_argument("--work-dir", type=Path, default=Path("tmp/xmv-original-work"))
    parser.add_argument(
        "--install",
        action="store_true",
        help="generate, verify and copy the pair into <lifted-dir>/gen, then reconfigure CMake",
    )
    parser.add_argument("--surface", type=Path, default=ROOT / "src/xbox/xdk_surface.c")
    parser.add_argument("--vendor-root", type=Path, default=ROOT / "third_party/xboxrecomp")
    args = parser.parse_args()
    try:
        if args.install:
            manifest = install(
                xbe=args.xbe,
                lifted=args.lifted_dir,
                entries=args.original_entries,
                work=args.work_dir,
                surface=args.surface,
                vendor=args.vendor_root,
            )
            print(
                f"profile {manifest['profile']}: {len(manifest['bodies'])} original bodies "
                f"installed into {args.lifted_dir / 'gen'}"
            )
            return 0
        if args.manual_functions is None or args.out_dir is None:
            raise ValueError("--manual-functions and --out-dir are required without --install")
        generate(
            xbe=args.xbe,
            lifted=args.lifted_dir,
            manual=args.manual_functions,
            entries=args.original_entries,
            surface=args.surface,
            vendor=args.vendor_root,
            out=args.out_dir,
        )
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"xmv original bodies: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
