# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile src/game for the harness and for the manifest, with the project's own flags.

The hand-written code is compiled here with the warning set CMakeLists.txt applies to
every project target (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
-Werror`), because a replacement that only builds under a laxer flag set is not what the
product builds. The optimisation level is a parameter: the product's default is Release,
and a Debug build is `-O0`, so a replacement should be proven at both.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path

from tools.harness.provenance import tree_digest

PROJECT_WARNINGS = (
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wconversion",
    "-Wstrict-prototypes",
    "-Werror",
)
INFRASTRUCTURE_FOR_MANIFEST = ("game_registry.c", "game_manifest.c")
COMPILE_TIMEOUT_SECONDS = 120


class BuildError(RuntimeError):
    """A hand-written source did not compile or link."""


def run(command: list[str], *, cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=COMPILE_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise BuildError(
            f"timed out after {COMPILE_TIMEOUT_SECONDS} s: {' '.join(command)}"
        ) from error
    if completed.returncode != 0:
        raise BuildError(f"{' '.join(command)}\n{completed.stderr}{completed.stdout}")
    return completed.stdout


def game_sources(game_dir: Path, *, include_manifest_main: bool) -> list[Path]:
    sources = sorted(game_dir.glob("*.c"))
    if not include_manifest_main:
        sources = [path for path in sources if path.name != "game_manifest.c"]
    return sources


def compile_objects(
    game_dir: Path,
    out_dir: Path,
    *,
    opt_level: str,
    cc: str = "cc",
    include_manifest_main: bool = False,
    extra_flags: tuple[str, ...] = (),
    warnings: tuple[str, ...] = PROJECT_WARNINGS,
) -> list[Path]:
    """Compile every src/game source to an object in `out_dir`. Returns the objects.

    `extra_flags` (default none, T1620 fp-scalar-v1 passes -ffp-contract=off -fno-fast-math)
    follow the project flags."""
    out_dir.mkdir(parents=True, exist_ok=True)
    objects: list[Path] = []
    from .x87_arbiter_contract import COMPILE_FLAGS as X87_FLAGS
    from .x87_arbiter_contract import replace_enabled

    if replace_enabled():  # T1510 opt-in v2 proof only; default flags are untouched
        extra_flags = (*extra_flags, *X87_FLAGS)
    from .codewrite_arbiter_contract import COMPILE_FLAGS as CODEWRITE_FLAGS
    from .codewrite_arbiter_contract import replace_enabled as codewrite_enabled

    if codewrite_enabled():  # T1508 opt-in v2 proof only; default flags are untouched
        extra_flags = (*extra_flags, *CODEWRITE_FLAGS)
    for source in game_sources(game_dir, include_manifest_main=include_manifest_main):
        obj = out_dir / f"{source.stem}.o"
        run(
            [
                cc,
                "-std=c11",
                opt_level,
                "-fPIE",
                *warnings,
                *extra_flags,
                f"-I{game_dir}",
                "-c",
                str(source),
                "-o",
                str(obj),
            ]
        )
        objects.append(obj)
    return objects


def build_manifest_exe(game_dir: Path, work_dir: Path, *, cc: str = "cc") -> Path:
    """Build the executable that prints the registry of everything in src/game."""
    objects = compile_objects(
        game_dir, work_dir / "manifest-objs", opt_level="-O0", cc=cc, include_manifest_main=True
    )
    # A registry-only binary never executes guest adapters. Supply the exact vector
    # TLS ABI separately so a vector adapter can be linked for metadata extraction.
    support = Path(__file__).with_name("manifest_vector_state.c")
    vector_object = work_dir / "manifest-vector-state.o"
    run([cc, "-O0", "-std=c11", "-fPIC", "-c", str(support), "-o", str(vector_object)])
    exe = work_dir / "game_manifest"
    from .x87_arbiter_contract import replace_enabled

    x87_runtime: list[str] = []
    if replace_enabled():  # the raw backend needs the isolated x87 runtime and libm
        harness = Path(__file__).resolve().parents[1] / "harness"
        x87_runtime = [
            str(harness / "x87_runtime.c"),
            str(harness / "x87_native.c"),
            f"-I{harness}",
            "-lm",
        ]
    from .codewrite_arbiter_contract import replace_enabled as codewrite_enabled

    codewrite_support: list[str] = []
    if codewrite_enabled():  # the proof build references the driver's stop hook
        codewrite_support = [str(Path(__file__).with_name("manifest_codewrite_stop.c"))]
    run(
        [
            cc,
            "-pie",
            *map(str, objects),
            str(vector_object),
            *x87_runtime,
            *codewrite_support,
            "-o",
            str(exe),
        ]
    )
    return exe


HARNESS_DIR = Path(__file__).resolve().parents[1] / "harness"
BASELINE_COMPLETION = "baseline-complete.json"


def _baseline_identity(gen_dir: Path) -> dict[str, str]:
    return {
        "gen_dir": str(gen_dir.resolve()),
        "tree_sha_full": tree_digest(gen_dir),
        "shim_sha": hashlib.sha256((HARNESS_DIR / "call_stub_shim.h").read_bytes()).hexdigest(),
        "builder_sha": hashlib.sha256(
            (HARNESS_DIR / "build_stub_objects.sh").read_bytes()
        ).hexdigest(),
        "gen_headers_sha": hashlib.sha256(
            (gen_dir / "recomp_funcs.h").read_bytes() + (gen_dir / "recomp_types.h").read_bytes()
        ).hexdigest(),
        "cc": os.environ.get("CC", "cc"),
    }


def _object_digests(base: Path, gen_dir: Path) -> dict[str, str]:
    expected = {f"{source.name}.o" for source in gen_dir.glob("*.c")}
    objects = {path.name: path for path in (base / "obj").glob("*.o")}
    if not expected or expected != objects.keys():
        raise BuildError(f"incomplete baseline object set in {base}")
    if any(path.stat().st_size == 0 for path in objects.values()):
        raise BuildError(f"empty baseline object in {base}")
    return {
        name: hashlib.sha256(path.read_bytes()).hexdigest()
        for name, path in sorted(objects.items())
    }


def _baseline_metadata(base: Path, gen_dir: Path, identity: dict[str, str]) -> dict[str, str]:
    provenance: dict[str, str] = {}
    for line in (base / "provenance.txt").read_text().splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in provenance:
            raise BuildError(f"malformed baseline provenance in {base}")
        provenance[key] = value
    for key in ("gen_dir", "tree_sha_full", "shim_sha", "gen_headers_sha"):
        if provenance.get(key) != identity[key]:
            raise BuildError(f"stale baseline {key} in {base}")
    if provenance.get("weak_dir") != "NONE" or not provenance.get("built_at"):
        raise BuildError(f"incomplete or non-baseline provenance in {base}")
    return _object_digests(base, gen_dir)


def _baseline_valid(base: Path, gen_dir: Path, identity: dict[str, str]) -> bool:
    try:
        completion = json.loads((base / BASELINE_COMPLETION).read_text())
        return (
            completion["schema"] == 1
            and completion["identity"] == identity
            and completion["objects"] == _baseline_metadata(base, gen_dir, identity)
        )
    except (OSError, ValueError, KeyError, TypeError, BuildError):
        return False


def _build_baseline(gen_dir: Path, base: Path) -> None:
    # A baseline must not inherit another invocation's weak/reuse configuration.
    environment = {
        key: value for key, value in os.environ.items() if key not in {"WEAK_DIR", "REUSE_OBJ_DIR"}
    }
    command = [str(HARNESS_DIR / "build_stub_objects.sh"), str(gen_dir), str(base)]
    completed = subprocess.run(
        command, env=environment, capture_output=True, text=True, timeout=3000, check=False
    )
    if completed.returncode != 0:
        raise BuildError(
            f"baseline build exited {completed.returncode}: {completed.stderr[-2000:]}"
        )


def prepare_baseline(
    gen_dir: Path,
    base_dir: Path,
    *,
    builder: Callable[[Path, Path], None] = _build_baseline,
) -> Path:
    """Select a verified baseline or build a fresh content-keyed sibling.

    Old caches are preserved. Successful build completion, exact generated path,
    tree/shim/builder digests, baseline provenance and every object digest must
    match before reuse. A process lock serializes preparations of the same key.
    Call this once before launching parallel replacement/mutant workers.
    """
    gen_dir = gen_dir.resolve()
    base_dir = base_dir.resolve()
    identity = _baseline_identity(gen_dir)
    if _baseline_valid(base_dir, gen_dir, identity):
        return base_dir
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:20]
    prefix = f"{base_dir.name}-{key}"
    base_dir.parent.mkdir(parents=True, exist_ok=True)
    with (base_dir.parent / f"{prefix}.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for candidate in sorted(base_dir.parent.glob(f"{prefix}*")):
            if candidate.is_dir() and _baseline_valid(candidate, gen_dir, identity):
                return candidate
        fresh = base_dir.parent / prefix
        if fresh.exists():
            fresh = Path(tempfile.mkdtemp(prefix=f"{prefix}-", dir=base_dir.parent))
        builder(gen_dir, fresh)
        if _baseline_identity(gen_dir) != identity:
            raise BuildError("generated tree or harness inputs changed during baseline build")
        objects = _baseline_metadata(fresh, gen_dir, identity)
        completion = {"schema": 1, "identity": identity, "objects": objects}
        marker = fresh / BASELINE_COMPLETION
        temporary = marker.with_suffix(".tmp")
        temporary.write_text(json.dumps(completion, sort_keys=True) + "\n")
        temporary.replace(marker)
        return fresh
