# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh one reviewed stale CMP/SUB-ZF body in a private installed profile."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

from tools.graft_lifted_function import function_body

ADDRESS = 0x002561D0
END = 0x00256991
NAME = "sub_002561D0"
SPAN_SHA = "4cc488d08ed4580debd43eb25eb424b79ad8eeb8c700a2034bd83a555228f3fa"
STALE = 'RECOMP_FLAGS_UNRESOLVED(_flags, "je", 0x0025693Bu)'


def locate(directory: Path) -> tuple[Path, str] | None:
    """Require a unique existing definition, with a complete generated comment/body."""
    pattern = re.compile(rf"/\*\*\n \* {NAME}\n.*?\nvoid {NAME}\(void\)\n\{{\n.*?\n\}}\n", re.S)
    found = []
    for path in sorted(directory.glob("recomp_[0-9][0-9][0-9][0-9].c")):
        for match in pattern.finditer(path.read_text()):
            found.append((path, match[0]))
    if len(found) > 1:
        raise RuntimeError("private ZF refresh found duplicate definitions")
    return found[0] if found else None


def replace_body(baseline: Path, output: Path, replacement: str) -> str:
    """Copy a plain baseline and replace exactly one existing body; no dispatcher edits."""
    from tools.play.seed_profile import ProfileError, manifest

    before = manifest(baseline)
    selected = locate(baseline)
    if selected is None:
        raise ProfileError("private ZF refresh has no existing body")
    path, old = selected
    if STALE not in old or STALE in replacement or "_cmp_sub_zf" not in replacement:
        raise ProfileError("private ZF refresh lacks the reviewed computed-ZF transition")
    if f"void {NAME}(void)" not in replacement:
        raise ProfileError("private ZF refresh changed the function ABI")
    if output.exists():
        raise ProfileError("private ZF output already exists")
    shutil.copytree(baseline, output)
    relative = path.relative_to(baseline).as_posix()
    expected = path.read_text().replace(old, replacement, 1)
    (output / relative).write_text(expected)
    after = manifest(output)
    if set(before) != set(after):
        raise ProfileError("private ZF refresh changed the file set")
    for name, value in before.items():
        if name == relative:
            if after[name]["mode"] != value["mode"]:
                raise ProfileError("private ZF refresh changed the chunk mode")
        elif after[name] != value:
            raise ProfileError(f"private ZF refresh changed unrelated file {name}")
    if (output / relative).read_text() != expected or manifest(baseline) != before:
        raise ProfileError("private ZF refresh changed its baseline or exact replacement")
    return relative


def prepare(root: Path, xbe: Path, baseline: Path) -> Path:
    from tools.dsoundscan.image import Image
    from tools.play.seed_profile import (
        XBE_SHA,
        ProfileError,
        identity,
        manifest,
        provenance,
        run_lift,
        sha,
    )

    root, xbe, baseline = root.resolve(), xbe.resolve(), baseline.resolve()
    selected = locate(baseline)
    if selected is None or STALE not in selected[1]:
        return baseline
    if (
        sha(xbe) != XBE_SHA
        or hashlib.sha256(Image(xbe).read(ADDRESS, END - ADDRESS)).hexdigest() != SPAN_SHA
    ):
        raise ProfileError("ZF refresh requires the exact reviewed retail function span")
    before = manifest(baseline)
    sources = provenance(root)
    sources["tools/play/zf_profile.py"] = sha(root / "tools/play/zf_profile.py")
    inputs = {
        "version": 1,
        "baseline_path": str(baseline),
        "baseline": before,
        "xbe_sha256": XBE_SHA,
        "span": [ADDRESS, END],
        "span_sha256": SPAN_SHA,
        "lifter": sources,
    }
    key = identity(inputs)
    cache = root / "tmp/play/zf-profiles"
    cache.mkdir(parents=True, exist_ok=True)
    final = cache / key
    if final.exists():
        try:
            receipt = json.loads((final / "receipt.json").read_text())
            if receipt["inputs"] != inputs or manifest(final / "gen") != receipt["output"]:
                raise ProfileError("private ZF cache identity mismatch")
        except (OSError, KeyError, ValueError) as error:
            raise ProfileError(f"invalid private ZF receipt: {error}") from error
        print(f"play: verified private computed-ZF profile {key}")
        return final / "gen"
    staging = Path(tempfile.mkdtemp(prefix=".prepare-", dir=cache))
    try:
        fresh = staging / "fresh"
        command = [
            sys.executable,
            "-m",
            "tools.lift",
            "--vendor-root",
            str(root / "third_party/xboxrecomp"),
            "run",
            str(xbe),
            "--out-dir",
            str(fresh),
            "--manual-functions",
            str(root / "generated/lifted/manual.json"),
            "--flag-bridge",
            str(root / "tools/config/flag_bridge.json"),
        ]
        print("play: refreshing one genuine computed-ZF body; canonical untouched")
        # A newly prepared menu profile can share its fully hash-checked fresh lift.
        # Legacy receipts without the full fresh manifest never qualify.
        shared = baseline.parent / "fresh/gen"
        reused = False
        try:
            menu_receipt = json.loads((baseline.parent / "receipt.json").read_text())
            common_sources = {k: v for k, v in sources.items() if k != "tools/play/zf_profile.py"}
            if (
                menu_receipt["inputs"]["lifter"] == common_sources
                and menu_receipt["inputs"]["xbe_sha256"] == XBE_SHA
                and menu_receipt["fresh_output"] == manifest(shared)
            ):
                fresh_gen = shared
                reused = True
        except (OSError, KeyError, ValueError, ProfileError):
            pass
        if not reused:
            run_lift(command, root)
            fresh_gen = fresh / "gen"
        body = function_body(fresh_gen, NAME)
        prototypes = (baseline / "recomp_funcs.h").read_text()
        dependencies = set(re.findall(r"\bsub_[0-9A-F]{8}\b", body)) - {NAME}
        if any(f"void {name}(void);" not in prototypes for name in dependencies):
            raise ProfileError("private ZF body has unavailable baseline dependencies")
        changed = replace_body(baseline, staging / "gen", body)
        if manifest(baseline) != before or provenance(root) != {
            k: v for k, v in sources.items() if k != "tools/play/zf_profile.py"
        }:
            raise ProfileError("ZF baseline/lifter changed during preparation")
        if sha(root / "tools/play/zf_profile.py") != sources["tools/play/zf_profile.py"]:
            raise ProfileError("ZF preparer changed during preparation")
        receipt = {
            "inputs": inputs,
            "key": key,
            "changed_chunk": changed,
            "body_sha256": hashlib.sha256(body.encode()).hexdigest(),
            "output": manifest(staging / "gen"),
            "lift_command": command,
            "reused_verified_menu_fresh": reused,
            "fresh_source": str(fresh_gen),
            "fresh_output": manifest(fresh_gen),
        }
        (staging / "receipt.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        os.rename(staging, final)
        print(f"play: verified private computed-ZF profile {key}")
        return final / "gen"
    finally:
        if staging.exists():
            shutil.rmtree(staging)
