# SPDX-License-Identifier: GPL-3.0-or-later
"""Private baseline plus one genuinely discovered XGRPH entry; never install a lift."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

from tools.gen_xdk_original_bodies import strict_json
from tools.play.seed_profile import ProfileError, manifest, provenance, sha
from tools.shaderscan.image import Image

ADDRESS = 0x003F36F1
END = 0x003F3703
XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
SPAN_SHA = "0a56e10d2f485041ac953b48ab9ac635e42b0d5dc14e6803993a463fd4dfe4cd"
RECEIPT = "membership_seed_receipt.json"


def selected_record(path: Path) -> dict:
    rows = strict_json(path)
    selected = [row for row in rows if int(str(row["start"]), 0) == ADDRESS]
    if len(selected) != 1:
        raise ValueError("discovery must contain exactly one membership entry")
    row = selected[0]
    expected = {
        "start": "0x003F36F1",
        "end": "0x003F3703",
        "size": 18,
        "section": "XGRPH",
        "detection_method": "seed_vtable_thunk",
        "calls_to": ["0x003F3497"],
        "num_instructions": 5,
        "name": "sub_003F36F1",
        "confidence": 0.95,
        "has_prologue": False,
        "called_by": [],
        "noreturn": False,
        "noreturn_reason": "",
    }
    if row != expected:
        raise ValueError("membership discovery is not the reviewed original function")
    return row


def verify(view: Path) -> dict:
    """Check exact baseline delta, source identities and modes, including helper inputs."""
    receipt = strict_json(view / RECEIPT)
    if (
        type(receipt.get("schema")) is not int
        or receipt.get("schema") != 1
        or receipt.get("address") != ADDRESS
    ):
        raise ValueError("unsupported membership seed receipt")
    required = {
        "schema",
        "address",
        "baseline",
        "baseline_identity",
        "discovery_functions",
        "source_hashes",
        "root",
        "lifter_identity",
        "record",
        "observation",
        "xbe",
        "seed",
    }
    if set(receipt) != required:
        raise ValueError("membership seed receipt fields differ from schema")
    if sha(Path(receipt["xbe"])) != XBE_SHA or sha_span(Path(receipt["xbe"])) != SPAN_SHA:
        raise ValueError("membership receipt XBE changed")
    baseline = Path(receipt["baseline"])
    if manifest(baseline) != receipt["baseline_identity"]:
        raise ValueError("membership baseline changed after preparation")
    discovered = Path(receipt["discovery_functions"]).parents[1]
    expected_sources = {
        str(Path(receipt["seed"])),
        receipt["discovery_functions"],
        str(discovered / "disasm/summary.json"),
        str(discovered / "logs/disasm.log"),
        receipt["xbe"],
        str(Path(receipt["root"]) / "tools/prepare_xgrph_membership.py"),
    }
    if set(receipt["source_hashes"]) != expected_sources:
        raise ValueError("membership receipt is missing provenance inputs")
    for path, expected in receipt["source_hashes"].items():
        if sha(Path(path)) != expected:
            raise ValueError(f"membership discovery/provenance changed: {path}")
    if provenance(Path(receipt["root"])) != receipt["lifter_identity"]:
        raise ValueError("membership lifter provenance changed")
    baseline_rows = strict_json(baseline / "disasm/functions.json")
    record = selected_record(Path(receipt["discovery_functions"]))
    if record != receipt["record"]:
        raise ValueError("membership discovery record changed")
    rows = strict_json(view / "disasm/functions.json")
    expected_rows = sorted(baseline_rows + [record], key=lambda row: int(str(row["start"]), 0))
    if rows != expected_rows:
        raise ValueError("private membership metadata is not the exact one-record addition")
    before, after = receipt["baseline_identity"], manifest(view)
    aliases = {"gen/recomp_xdk_original.c", "gen/original_manifest.json"}
    if set(after) - aliases != (set(before) - aliases) | {RECEIPT}:
        raise ValueError("private membership view changed its file set")
    for key, value in before.items():
        if key in aliases:
            continue  # installer owns only this independently verified output pair
        if key == "disasm/functions.json":
            if value["mode"] != after[key]["mode"]:
                raise ValueError("private membership metadata changed baseline mode")
        elif value != after[key]:
            raise ValueError(f"private membership view changed baseline bytes/mode: {key}")
    return receipt


def prepare(
    *, root: Path, xbe: Path, baseline: Path, discovery: Path, seed: Path, out: Path
) -> Path:
    if any(path.is_symlink() for path in (baseline, discovery, seed, out)):
        raise ValueError("membership preparation refuses linked input/output roots")
    root, xbe, baseline, discovery, seed, out = (
        path.resolve() for path in (root, xbe, baseline, discovery, seed, out)
    )
    if out.exists() or any(out == path or path in out.parents for path in (baseline, discovery)):
        raise ValueError("membership output must be new and outside its source trees")
    if sha(xbe) != XBE_SHA or sha_span(xbe) != SPAN_SHA:
        raise ValueError("membership seed requires the reviewed retail XBE and 18-byte span")
    seeds = strict_json(seed)
    if (
        len(seeds) != 1
        or seeds[0].get("start") != "0x003F36F1"
        or seeds[0].get("observed") is not True
    ):
        raise ValueError("membership seed must be the single observed original entry")
    if seeds[0].get("xbe_sha256") != XBE_SHA or seeds[0].get("original_sha256") != SPAN_SHA:
        raise ValueError("membership seed provenance differs from original bytes")
    before = manifest(baseline)
    manifest(discovery)  # refuse linked/special discovery inputs
    functions = discovery / "disasm/functions.json"
    record = selected_record(functions)
    baseline_rows = strict_json(baseline / "disasm/functions.json")
    if any(int(str(row["start"]), 0) == ADDRESS for row in baseline_rows):
        raise ValueError("baseline already contains membership entry; no seeded replacement")
    sources = [
        seed,
        functions,
        discovery / "disasm/summary.json",
        discovery / "logs/disasm.log",
        xbe,
        root / "tools/prepare_xgrph_membership.py",
    ]
    hashes = {str(path): sha(path) for path in sources}
    lifter = provenance(root)
    shutil.copytree(baseline, out, copy_function=shutil.copy2)
    try:
        rows = sorted(baseline_rows + [record], key=lambda row: int(str(row["start"]), 0))
        (out / "disasm/functions.json").write_text(json.dumps(rows, indent=2) + "\n")
        receipt = {
            "xbe": str(xbe),
            "seed": str(seed),
            "schema": 1,
            "address": ADDRESS,
            "baseline": str(baseline),
            "baseline_identity": before,
            "discovery_functions": str(functions),
            "source_hashes": hashes,
            "root": str(root),
            "lifter_identity": lifter,
            "record": record,
            "observation": (
                "controlled original x86 execution; not retail reachability or attestation"
            ),
        }
        (out / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n")
        verify(out)
    except (ValueError, OSError, ProfileError):
        # Preserve failed output for review; never reuse it as verified preparation.
        raise
    return out


def sha_span(xbe: Path) -> str:
    import hashlib

    return hashlib.sha256(Image.load(xbe).read(ADDRESS, END - ADDRESS)).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("xbe", "baseline", "discovery", "seed", "out"):
        parser.add_argument("--" + argument, type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        prepare(**vars(args))
    except (ValueError, OSError, KeyError, TypeError, ProfileError) as error:
        parser.exit(1, f"membership preparation: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
