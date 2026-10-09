# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh reviewed scalar-float join bodies in private installed profiles."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.graft_lifted_function import function_body

ADDRESS = 0x002FB4C0
END = 0x002FB580
NAME = "sub_002FB4C0"
SPAN_SHA = "66053b6a7c6d97fcfd298ecd156cd782dea289dd49464457c64bcd0de53cb695"
STALE = 'RECOMP_FLAGS_UNRESOLVED(_flags, "jbe", 0x002FB575u)'


@dataclass(frozen=True)
class FloatRefresh:
    address: int
    end: int
    name: str
    span_sha: str
    stale: str
    cache: str = "float-profiles"


DEFAULT_REFRESH = FloatRefresh(ADDRESS, END, NAME, SPAN_SHA, STALE)
WIDGET_REFRESH = FloatRefresh(
    0x0007A7B0,
    0x0007B33F,
    "sub_0007A7B0",
    "5c6e64dce424c9817a25f40d9b0a7fc4ff5e76a38b7dc11ea9f05d92dd69f38e",
    'RECOMP_FLAGS_UNRESOLVED(_flags, "jbe", 0x0007A917u)',
    "widget-float-profiles",
)


def locate(directory: Path, refresh: FloatRefresh = DEFAULT_REFRESH) -> tuple[Path, str] | None:
    """Require a unique existing definition, with a complete generated comment/body."""
    pattern = re.compile(
        rf"/\*\*\n \* {refresh.name}\n.*?\nvoid {refresh.name}\(void\)\n\{{\n.*?\n\}}\n", re.S
    )
    found = []
    for path in sorted(directory.glob("recomp_[0-9][0-9][0-9][0-9].c")):
        for match in pattern.finditer(path.read_text()):
            found.append((path, match[0]))
    if len(found) > 1:
        raise RuntimeError("private float refresh found duplicate definitions")
    return found[0] if found else None


def replace_body(
    baseline: Path, output: Path, replacement: str, refresh: FloatRefresh = DEFAULT_REFRESH
) -> str:
    """Copy a plain baseline and replace exactly one existing body; no dispatcher edits."""
    from tools.play.seed_profile import ProfileError, manifest

    before = manifest(baseline)
    selected = locate(baseline, refresh)
    if selected is None:
        raise ProfileError("private float refresh has no existing body")
    path, old = selected
    if (
        refresh.stale not in old
        or refresh.stale in replacement
        or "_fca <= _fcb" not in replacement
    ):
        raise ProfileError("private float refresh lacks the reviewed scalar-float transition")
    if f"void {refresh.name}(void)" not in replacement:
        raise ProfileError("private float refresh changed the function ABI")
    if output.exists():
        raise ProfileError("private float output already exists")
    shutil.copytree(baseline, output)
    relative = path.relative_to(baseline).as_posix()
    expected = path.read_text().replace(old, replacement, 1)
    (output / relative).write_text(expected)
    after = manifest(output)
    if set(before) != set(after):
        raise ProfileError("private float refresh changed the file set")
    for name, value in before.items():
        if name == relative:
            if after[name]["mode"] != value["mode"]:
                raise ProfileError("private float refresh changed the chunk mode")
        elif after[name] != value:
            raise ProfileError(f"private float refresh changed unrelated file {name}")
    if (output / relative).read_text() != expected or manifest(baseline) != before:
        raise ProfileError("private float refresh changed its baseline or exact replacement")
    return relative


def verified_fresh(baseline: Path, sources: dict[str, object]) -> Path | None:
    """Reuse a direct lift or the verified lift referenced by a ZF receipt."""
    from tools.play.seed_profile import XBE_SHA, ProfileError, manifest

    try:
        receipt = json.loads((baseline.parent / "receipt.json").read_text())
        lifter = dict(receipt["inputs"]["lifter"])
        lifter.pop("tools/play/zf_profile.py", None)
        lifter.pop("tools/play/float_profile.py", None)
        if lifter != sources or receipt["inputs"]["xbe_sha256"] != XBE_SHA:
            return None
        if receipt["output"] != manifest(baseline):
            return None
        candidates = [baseline.parent / "fresh/gen"]
        if "fresh_source" in receipt:
            candidates.append(Path(receipt["fresh_source"]))
        for candidate in candidates:
            if candidate.is_dir() and manifest(candidate) == receipt["fresh_output"]:
                return candidate
    except (OSError, KeyError, ValueError, TypeError, ProfileError):
        pass
    return None


def prepare(root: Path, xbe: Path, baseline: Path, refresh: FloatRefresh = DEFAULT_REFRESH) -> Path:
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
    selected = locate(baseline, refresh)
    if selected is None or refresh.stale not in selected[1]:
        return baseline
    if (
        sha(xbe) != XBE_SHA
        or hashlib.sha256(
            Image(xbe).read(refresh.address, refresh.end - refresh.address)
        ).hexdigest()
        != refresh.span_sha
    ):
        raise ProfileError("float refresh requires the exact reviewed retail function span")
    before = manifest(baseline)
    sources = provenance(root)
    sources["tools/play/float_profile.py"] = sha(root / "tools/play/float_profile.py")
    inputs = {
        "version": 1,
        "baseline_path": str(baseline),
        "baseline": before,
        "xbe_sha256": XBE_SHA,
        "span": [refresh.address, refresh.end],
        "span_sha256": refresh.span_sha,
        "lifter": sources,
    }
    key = identity(inputs)
    cache = root / "tmp/play" / refresh.cache
    cache.mkdir(parents=True, exist_ok=True)
    final = cache / key
    if final.exists():
        try:
            receipt = json.loads((final / "receipt.json").read_text())
            if receipt["inputs"] != inputs or manifest(final / "gen") != receipt["output"]:
                raise ProfileError("private float cache identity mismatch")
        except (OSError, KeyError, ValueError) as error:
            raise ProfileError(f"invalid private float receipt: {error}") from error
        print(f"play: verified private scalar-float profile {key}")
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
            "--only-function",
            hex(refresh.address),
            "--manual-functions",
            str(root / "generated/lifted/manual.json"),
            "--flag-bridge",
            str(root / "tools/config/flag_bridge.json"),
        ]
        print("play: refreshing one genuine scalar-float body; canonical untouched")
        common_sources = {k: v for k, v in sources.items() if k != "tools/play/float_profile.py"}
        fresh_gen = verified_fresh(baseline, common_sources)
        reused = fresh_gen is not None
        if reused:
            print("play: reusing verified fresh lift for scalar-float refresh")
        if not reused:
            run_lift(command, root)
            fresh_gen = fresh / "gen"
        body = function_body(fresh_gen, refresh.name)
        prototypes = (baseline / "recomp_funcs.h").read_text()
        dependencies = set(re.findall(r"\bsub_[0-9A-F]{8}\b", body)) - {refresh.name}
        if any(f"void {name}(void);" not in prototypes for name in dependencies):
            raise ProfileError("private float body has unavailable baseline dependencies")
        changed = replace_body(baseline, staging / "gen", body, refresh)
        if manifest(baseline) != before or provenance(root) != {
            k: v for k, v in sources.items() if k != "tools/play/float_profile.py"
        }:
            raise ProfileError("float baseline/lifter changed during preparation")
        if sha(root / "tools/play/float_profile.py") != sources["tools/play/float_profile.py"]:
            raise ProfileError("float preparer changed during preparation")
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
        print(f"play: verified private scalar-float profile {key}")
        return final / "gen"
    finally:
        if staging.exists():
            shutil.rmtree(staging)
