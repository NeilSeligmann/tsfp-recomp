# SPDX-License-Identifier: GPL-3.0-or-later
"""Compose two genuine cleanup records over an immutable verified v7 parent."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

from tools.gen_xdk_original_bodies import strict_json
from tools.play.seed_profile import ProfileError, manifest, provenance, sha
from tools.prepare_xgrph_recursive import verify as verify_parent
from tools.shaderscan.image import Image

SPANS = {
    0x003F1786: "018d476ef59bfd2f10bd32149919291a2d7d35020036f47d83760d8103c49ab7",
    0x003F17D4: "5554fc880310cd35244e379267fc10ecf60c1ce6321bd2de7745c5fe231e3ac6",
}
XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
RECEIPT = "cleanup_variants_receipt.json"
FIELDS = {
    "schema",
    "parent",
    "parent_identity",
    "discovery",
    "records",
    "xbe",
    "seed",
    "root",
    "source_hashes",
    "lifter_identity",
    "observation",
}


def records(discovery: Path) -> list[dict]:
    rows = strict_json(discovery / "disasm/functions.json")
    result = []
    for address in SPANS:
        expected = {
            "start": f"0x{address:08X}",
            "end": f"0x{address + 11:08X}",
            "size": 11,
            "name": f"sub_{address:08X}",
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
        found = [row for row in rows if int(str(row["start"]), 0) == address]
        if found != [expected]:
            raise ValueError("cleanup variant is not the exact genuine discovered function record")
        result.append(found[0])
    return result


def sources(root: Path, xbe: Path, seed: Path, discovery: Path) -> list[Path]:
    return [
        root / "tools/prepare_xgrph_variants.py",
        xbe,
        seed,
        discovery / "disasm/functions.json",
        discovery / "disasm/summary.json",
        discovery / "logs/disasm.log",
    ]


def verify_xbe(xbe: Path) -> None:
    import hashlib

    if sha(xbe) != XBE_SHA:
        raise ValueError("cleanup variants require the reviewed retail XBE")
    image = Image.load(xbe)
    for address, digest in SPANS.items():
        if hashlib.sha256(image.read(address, 11)).hexdigest() != digest:
            raise ValueError("cleanup variants require both exact original 11-byte spans")


def verify(view: Path) -> dict:
    receipt = strict_json(view / RECEIPT)
    if set(receipt) != FIELDS or type(receipt["schema"]) is not int or receipt["schema"] != 1:
        raise ValueError("unsupported cleanup variants seed receipt")
    parent, discovery, root, xbe, seed = (
        Path(receipt[key]) for key in ("parent", "discovery", "root", "xbe", "seed")
    )
    verify_xbe(xbe)
    verify_parent(parent)  # its strict one-record child gate is never widened
    if manifest(parent) != receipt["parent_identity"]:
        raise ValueError("cleanup variants parent byte/mode identity changed")
    expected_sources = {str(path) for path in sources(root, xbe, seed, discovery)}
    if set(receipt["source_hashes"]) != expected_sources:
        raise ValueError("cleanup variants receipt lacks exact provenance inputs")
    for path, digest in receipt["source_hashes"].items():
        if sha(Path(path)) != digest:
            raise ValueError(f"cleanup variants provenance input changed: {path}")
    if provenance(root) != receipt["lifter_identity"]:
        raise ValueError("cleanup variants lifter provenance changed")
    new_records = records(discovery)
    if new_records != receipt["records"]:
        raise ValueError("cleanup variants discovery record changed")
    previous = strict_json(parent / "disasm/functions.json")
    expected = sorted(previous + new_records, key=lambda row: int(str(row["start"]), 0))
    if strict_json(view / "disasm/functions.json") != expected:
        raise ValueError("cleanup variants view is not the exact two-record addition")
    before, after = receipt["parent_identity"], manifest(view)
    outputs = {"gen/recomp_xdk_original.c", "gen/original_manifest.json"}
    if set(after) - outputs != (set(before) - outputs) | {RECEIPT}:
        raise ValueError("cleanup variants view changed its file set")
    for key, value in before.items():
        if key in outputs:
            continue  # independent original-pair installer owns these two outputs
        if key == "disasm/functions.json":
            if value["mode"] != after[key]["mode"]:
                raise ValueError("cleanup variants metadata changed parent mode")
        elif value != after[key]:
            raise ValueError(f"cleanup variants view changed parent bytes/mode: {key}")
    return receipt


def prepare(*, root: Path, xbe: Path, parent: Path, discovery: Path, seed: Path, out: Path) -> Path:
    if any(path.is_symlink() for path in (parent, discovery, seed, out)):
        raise ValueError("cleanup variants preparation refuses linked roots")
    root, xbe, parent, discovery, seed, out = (
        path.resolve() for path in (root, xbe, parent, discovery, seed, out)
    )
    if out.exists() or any(path == out or path in out.parents for path in (parent, discovery)):
        raise ValueError("cleanup variants output must be new and outside its inputs")
    verify_xbe(xbe)
    verify_parent(parent)
    before = manifest(parent)
    manifest(discovery)
    new_records = records(discovery)
    previous = strict_json(parent / "disasm/functions.json")
    if any(int(str(row["start"]), 0) in SPANS for row in previous):
        raise ValueError("parent already contains cleanup variants entry; no replacement admitted")
    seeds = strict_json(seed)
    if (
        not isinstance(seeds, list)
        or len(seeds) != 2
        or any(not isinstance(row, dict) for row in seeds)
    ):
        raise ValueError("cleanup seed requires exactly two observed entries")
    addresses = [int(str(row.get("start")), 0) for row in seeds]
    if set(addresses) != set(SPANS) or len(set(addresses)) != 2:
        raise ValueError("cleanup seed entries differ from the two reviewed originals")
    for row in seeds:
        address = int(str(row["start"]), 0)
        if (
            row.get("end") != f"0x{address + 11:08X}"
            or row.get("observed") is not True
            or row.get("xbe_sha256") != XBE_SHA
            or row.get("original_sha256") != SPANS[address]
        ):
            raise ValueError("cleanup seed provenance differs from observed original bytes")
    hashes = {str(path): sha(path) for path in sources(root, xbe, seed, discovery)}
    lifter = provenance(root)
    shutil.copytree(parent, out, copy_function=shutil.copy2)
    rows = sorted(previous + new_records, key=lambda row: int(str(row["start"]), 0))
    (out / "disasm/functions.json").write_text(json.dumps(rows, indent=2) + "\n")
    receipt = {
        "schema": 1,
        "parent": str(parent),
        "parent_identity": before,
        "discovery": str(discovery),
        "records": new_records,
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
        parser.exit(1, f"cleanup variants preparation: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
