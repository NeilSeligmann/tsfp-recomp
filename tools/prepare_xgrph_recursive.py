# SPDX-License-Identifier: GPL-3.0-or-later
"""Compose one genuine recursive-cleanup record over a verified immutable parent view."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

from tools.gen_xdk_original_bodies import strict_json
from tools.play.seed_profile import ProfileError, manifest, provenance, sha
from tools.prepare_xgrph_membership import verify as verify_parent
from tools.shaderscan.image import Image

ADDRESS, END = 0x003F17AE, 0x003F17B9
XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
SPAN_SHA = "467f827bbed172b2602f71f8d39e7f331bd7f6984b64acf2afd079f5b874b38a"
RECEIPT = "recursive_seed_receipt.json"
FIELDS = {
    "schema",
    "parent",
    "parent_identity",
    "discovery",
    "record",
    "xbe",
    "seed",
    "root",
    "source_hashes",
    "lifter_identity",
    "observation",
}


def record(discovery: Path) -> dict:
    rows = strict_json(discovery / "disasm/functions.json")
    found = [row for row in rows if int(str(row["start"]), 0) == ADDRESS]
    expected = {
        "start": "0x003F17AE",
        "end": "0x003F17B9",
        "size": 11,
        "name": "sub_003F17AE",
        "section": "XGRPH",
        "confidence": 0.95,
        "detection_method": "seed_vtable_thunk",
        "num_instructions": 2,
        "has_prologue": False,
        "calls_to": [],
        "called_by": [],
        "noreturn": False,
        "noreturn_reason": "",
    }
    if found != [expected]:
        raise ValueError("recursive seed is not the exact genuine discovered function record")
    return found[0]


def sources(root: Path, xbe: Path, seed: Path, discovery: Path) -> list[Path]:
    return [
        root / "tools/prepare_xgrph_recursive.py",
        xbe,
        seed,
        discovery / "disasm/functions.json",
        discovery / "disasm/summary.json",
        discovery / "logs/disasm.log",
    ]


def verify_xbe(xbe: Path) -> None:
    import hashlib

    if (
        sha(xbe) != XBE_SHA
        or hashlib.sha256(Image.load(xbe).read(ADDRESS, END - ADDRESS)).hexdigest() != SPAN_SHA
    ):
        raise ValueError("recursive seed requires the reviewed retail XBE and exact11-byte span")


def verify(view: Path) -> dict:
    receipt = strict_json(view / RECEIPT)
    if set(receipt) != FIELDS or type(receipt["schema"]) is not int or receipt["schema"] != 1:
        raise ValueError("unsupported recursive seed receipt")
    parent, discovery, root, xbe, seed = (
        Path(receipt[key]) for key in ("parent", "discovery", "root", "xbe", "seed")
    )
    verify_xbe(xbe)
    verify_parent(parent)  # its strict one-record gate is never widened
    if manifest(parent) != receipt["parent_identity"]:
        raise ValueError("recursive parent byte/mode identity changed")
    expected_sources = {str(path) for path in sources(root, xbe, seed, discovery)}
    if set(receipt["source_hashes"]) != expected_sources:
        raise ValueError("recursive receipt lacks exact provenance inputs")
    for path, digest in receipt["source_hashes"].items():
        if sha(Path(path)) != digest:
            raise ValueError(f"recursive provenance input changed: {path}")
    if provenance(root) != receipt["lifter_identity"]:
        raise ValueError("recursive lifter provenance changed")
    new_record = record(discovery)
    if new_record != receipt["record"]:
        raise ValueError("recursive discovery record changed")
    previous = strict_json(parent / "disasm/functions.json")
    expected = sorted(previous + [new_record], key=lambda row: int(str(row["start"]), 0))
    if strict_json(view / "disasm/functions.json") != expected:
        raise ValueError("recursive view is not the exact one-record addition")
    before, after = receipt["parent_identity"], manifest(view)
    outputs = {"gen/recomp_xdk_original.c", "gen/original_manifest.json"}
    if set(after) - outputs != (set(before) - outputs) | {RECEIPT}:
        raise ValueError("recursive view changed its file set")
    for key, value in before.items():
        if key in outputs:
            continue  # independent original-pair installer owns these two outputs
        if key == "disasm/functions.json":
            if value["mode"] != after[key]["mode"]:
                raise ValueError("recursive metadata changed parent mode")
        elif value != after[key]:
            raise ValueError(f"recursive view changed parent bytes/mode: {key}")
    return receipt


def prepare(*, root: Path, xbe: Path, parent: Path, discovery: Path, seed: Path, out: Path) -> Path:
    if any(path.is_symlink() for path in (parent, discovery, seed, out)):
        raise ValueError("recursive preparation refuses linked roots")
    root, xbe, parent, discovery, seed, out = (
        path.resolve() for path in (root, xbe, parent, discovery, seed, out)
    )
    if out.exists() or any(path == out or path in out.parents for path in (parent, discovery)):
        raise ValueError("recursive output must be new and outside its inputs")
    verify_xbe(xbe)
    verify_parent(parent)
    before = manifest(parent)
    manifest(discovery)
    new_record = record(discovery)
    previous = strict_json(parent / "disasm/functions.json")
    if any(int(str(row["start"]), 0) == ADDRESS for row in previous):
        raise ValueError("parent already contains recursive entry; no replacement admitted")
    seeds = strict_json(seed)
    if (
        len(seeds) != 1
        or seeds[0].get("start") != "0x003F17AE"
        or seeds[0].get("observed") is not True
    ):
        raise ValueError("recursive seed must be the single observed original entry")
    if seeds[0].get("xbe_sha256") != XBE_SHA or seeds[0].get("original_sha256") != SPAN_SHA:
        raise ValueError("recursive seed provenance differs from original bytes")
    hashes = {str(path): sha(path) for path in sources(root, xbe, seed, discovery)}
    lifter = provenance(root)
    shutil.copytree(parent, out, copy_function=shutil.copy2)
    rows = sorted(previous + [new_record], key=lambda row: int(str(row["start"]), 0))
    (out / "disasm/functions.json").write_text(json.dumps(rows, indent=2) + "\n")
    receipt = {
        "schema": 1,
        "parent": str(parent),
        "parent_identity": before,
        "discovery": str(discovery),
        "record": new_record,
        "xbe": str(xbe),
        "seed": str(seed),
        "root": str(root),
        "source_hashes": hashes,
        "lifter_identity": lifter,
        "observation": (
            "controlled original recursion, not current retail reachability or attestation"
        ),
    }
    (out / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n")
    verify(out)  # preserve failed output for review; never silently reuse it
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("xbe", "parent", "discovery", "seed", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    try:
        prepare(**vars(parser.parse_args()))
    except (ValueError, OSError, KeyError, TypeError, ProfileError) as error:
        parser.exit(1, f"recursive preparation: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
