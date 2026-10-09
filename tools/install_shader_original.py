#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate, verify and install the retained original shader-compiler aliases.

One command replaces the manual-map snippet and file copies from
docs/shader-original-routes.md. The manual map is derived from the compiled
manual chunk of the target lift, the aliases are produced by
tools.gen_xdk_original_bodies, and the result is checked against its own manifest
before anything is copied. Only recomp_xdk_original.c and original_manifest.json
are written into the lift's gen directory. Reconfigure CMake afterwards. The host
still needs --native-shader-assembler to run the aliases.

T367 additions. A fresh worktree also lacks the gitignored src/xbox/xdk_surface.{c,h}
and xdk_abi.inc, which the generator needs. They are produced from the same XBE when
--xtlid names the XboxDev/xtlid database, and refused with the exact commands when it
does not (without the database the surface carries fewer names, so the result would
silently differ). --check re-hashes every recorded input against the installed pair and
reports a pair gone stale after a relift, an entries edit or a surface change.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.gen_xdk_original_bodies import (
    ROOT,
    digest,
    generate,
    input_paths,
    vendor_source_hashes,
)

CHUNK = "recomp_xdk_original.c"
MANIFEST = "original_manifest.json"
MANUAL_DEFINITION = re.compile(r"\bvoid sub_([0-9A-F]{8})\(void\)\s*\{")
DEFAULT_SURFACE_DIR = ROOT / "src/xbox"
DEFAULT_ENTRIES = ROOT / "tools/data/shader_original_entries.json"
DEFAULT_VENDOR = ROOT / "third_party/xboxrecomp"
SURFACE_PAIR = ("xdk_surface.c", "xdk_surface.h")
ABI_TABLE = "xdk_abi.inc"
#: Process exit codes of --check. Absent is not an error: the chunk is optional.
EXIT_CURRENT = 0
EXIT_PROBLEMS = 1
EXIT_ABSENT = 2

CURRENT = "current"
ABSENT = "absent"
STALE = "stale"

LIFT_HELP = (
    "generate it with:\n"
    "    uv run python -m tools.gen_xdk_manual_list --surface src/xbox/xdk_surface.c "
    "--json tmp/xdk-manual.json --trampolines generated/lifted/gen/recomp_xdk_manual.c\n"
    "    uv run python -m tools.lift run <default.xbe> --manual-functions tmp/xdk-manual.json"
)


class MissingInputError(ValueError):
    """A gitignored private input is absent. The message names it and the command."""


def derive_manual_map(chunk_text: str) -> dict[str, str]:
    """Return the pinned address to symbol map for every compiled manual body."""
    addresses = MANUAL_DEFINITION.findall(chunk_text)
    if not addresses:
        raise ValueError("compiled manual chunk defines no manual functions")
    if len(addresses) != len(set(addresses)):
        raise ValueError("compiled manual chunk defines a function twice")
    return {f"0x{address}": f"sub_{address}" for address in sorted(addresses)}


def verify_output(out_dir: Path, xbe_hash: str | None) -> dict[str, object]:
    """Check that the generated chunk is the one its manifest describes."""
    chunk = (out_dir / CHUNK).read_bytes()
    manifest = json.loads((out_dir / MANIFEST).read_text())
    if not chunk:
        raise ValueError("generated original chunk is empty")
    if manifest.get("generated_chunk_sha256") != digest(chunk):
        raise ValueError("generated chunk does not match its manifest")
    if xbe_hash is not None and manifest.get("xbe_sha256") != xbe_hash:
        raise ValueError("manifest was generated from a different XBE")
    bodies = manifest.get("bodies")
    if not isinstance(bodies, list) or not bodies:
        raise ValueError("manifest records no original bodies")
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


def missing_surface_inputs(surface_dir: Path) -> list[Path]:
    """The gitignored generated surface files absent from `surface_dir`."""
    return [
        surface_dir / name
        for name in (*SURFACE_PAIR, ABI_TABLE)
        if not (surface_dir / name).is_file()
    ]


def surface_commands(xbe: Path) -> str:
    return (
        f"    uv run python -m tools.gen_d3d8_surface {xbe} --xtlid <xtlid.xml>\n"
        f"    uv run python -m tools.xdk_abi --xbe {xbe} --emit-c src/xbox/xdk_abi.inc"
    )


def prepare_surface(xbe: Path, xtlid: Path | None, surface_dir: Path) -> list[Path]:
    """Create the missing generated surface files from the XBE, returning what was written.

    The surface carries names from the XboxDev/xtlid database, so generating it without
    the database would give a different table than the one a seeded tree holds. That is
    refused with the exact commands instead of guessed. Existing files are never replaced
    unless the surface pair itself is incomplete, because the ABI table is keyed by it.
    """
    missing = missing_surface_inputs(surface_dir)
    if not missing:
        return []
    names = ", ".join(path.name for path in missing)
    if xtlid is None or not xtlid.is_file():
        raise MissingInputError(
            f"missing generated surface input(s) {names} in {surface_dir}. Pass --xtlid "
            "<xtlid.xml> (the XboxDev/xtlid database, see tools/xtlid.py) so they are "
            f"generated from {xbe}, or generate them by hand:\n{surface_commands(xbe)}"
        )
    from tools import gen_d3d8_surface, xdk_abi

    surface_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    with tempfile.TemporaryDirectory(dir=surface_dir, prefix=".surface-") as temp:
        stage = Path(temp)
        regenerate = any(path.name in SURFACE_PAIR for path in missing)
        if regenerate:
            argv = [str(xbe), "--xtlid", str(xtlid), "--out-dir", str(stage)]
            with contextlib.redirect_stdout(io.StringIO()):
                code = gen_d3d8_surface.main(argv)
            if code not in (0, None):
                raise ValueError(f"gen_d3d8_surface failed with exit {code}")
            for name in SURFACE_PAIR:
                if not (stage / name).is_file():
                    raise ValueError(f"gen_d3d8_surface wrote no {name}")
        surface_file = (stage if regenerate else surface_dir) / SURFACE_PAIR[0]
        # The table is keyed by the surface, so a regenerated surface regenerates it too.
        if regenerate or ABI_TABLE in {path.name for path in missing}:
            report = xdk_abi.measure(xbe, surface_file)
            if xdk_abi.emit_c(report, stage / ABI_TABLE) <= 0:
                raise ValueError("xdk_abi emitted no rows")
        for path in sorted(stage.iterdir()):
            os.replace(path, surface_dir / path.name)
            written.append(surface_dir / path.name)
    return written


def require_lift(gen_dir: Path) -> None:
    """Refuse a directory that holds no compiled manual chunk, naming how to make one."""
    if not (gen_dir / "recomp_xdk_manual.c").is_file():
        raise MissingInputError(
            f"no lift at {gen_dir} (recomp_xdk_manual.c absent). Create it: {LIFT_HELP}"
        )


@dataclass(frozen=True)
class InstallCheck:
    status: str
    problems: tuple[str, ...] = ()
    profile: str | None = None


def check_installed(
    gen_dir: Path,
    *,
    lifted: Path | None = None,
    entries: Path = DEFAULT_ENTRIES,
    surface: Path = DEFAULT_SURFACE_DIR / SURFACE_PAIR[0],
    vendor: Path = DEFAULT_VENDOR,
    xbe: Path | None = None,
) -> InstallCheck:
    """Compare an installed pair against the inputs it was generated from.

    ABSENT when neither file is installed (the chunk is optional). STALE carries one
    problem per mismatch: a tampered or partial pair, another XBE, or any recorded input
    (compiled manual chunk, runtime template, lift analysis, entries, surface, vendored
    translator) whose hash no longer matches. CURRENT only when every one agrees.
    """
    chunk, manifest_path = gen_dir / CHUNK, gen_dir / MANIFEST
    if not chunk.exists() and not manifest_path.exists():
        return InstallCheck(ABSENT)
    if not (chunk.is_file() and manifest_path.is_file()):
        return InstallCheck(STALE, (f"partial install, need both {CHUNK} and {MANIFEST}",))
    problems: list[str] = []
    try:
        xbe_hash = digest(xbe.read_bytes()) if xbe is not None else None
        manifest = verify_output(gen_dir, xbe_hash)
    except (ValueError, OSError, KeyError, TypeError) as error:
        return InstallCheck(STALE, (str(error),))
    recorded = manifest.get("inputs")
    if not isinstance(recorded, dict) or not recorded:
        return InstallCheck(STALE, ("manifest records no inputs",), str(manifest.get("profile")))
    lifted = gen_dir.parent if lifted is None else lifted
    try:
        paths = input_paths(
            entries=entries, manual=Path(), surface=surface, lifted=lifted, vendor=vendor
        )
    except (OSError, ValueError) as error:
        reason = (
            "missing input entries/compiled closure"
            if isinstance(error, FileNotFoundError)
            else "input entries/compiled closure cannot be verified"
        )
        return InstallCheck(STALE, (f"{reason}: {error}",), str(manifest.get("profile")))
    for key in sorted(paths):
        if key == "manual":
            continue
        if key not in recorded:
            problems.append(f"manifest lacks input {key}")
        elif not paths[key].is_file():
            problems.append(f"input {key} is missing: {paths[key]}")
        elif digest(paths[key].read_bytes()) != recorded[key]:
            problems.append(f"input {key} changed since generation: {paths[key]}")
    try:
        current_sources = vendor_source_hashes(vendor)
    except OSError as error:
        problems.append(f"vendored translator unreadable: {error}")
    else:
        if current_sources != manifest.get("vendor_sources"):
            problems.append("vendored translator sources changed since generation")
    profile = str(manifest.get("profile"))
    if problems:
        return InstallCheck(STALE, tuple(problems), profile)
    return InstallCheck(CURRENT, (), profile)


def run(
    *,
    xbe: Path,
    lifted: Path,
    entries: Path,
    work: Path,
    install: bool,
    surface_dir: Path = DEFAULT_SURFACE_DIR,
    xtlid: Path | None = None,
    vendor: Path = DEFAULT_VENDOR,
) -> dict[str, object]:
    gen_dir = lifted / "gen"
    surface = surface_dir / SURFACE_PAIR[0]
    require_lift(gen_dir)
    prepare_surface(xbe, xtlid, surface_dir)
    mapping = derive_manual_map((gen_dir / "recomp_xdk_manual.c").read_text())
    work.mkdir(parents=True, exist_ok=True)
    manual = work / "xdk-manual-canonical.json"
    manual.write_text(json.dumps(mapping, indent=2) + "\n")
    out_dir = work / "shader-original"
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
    check = {"entries": entries, "surface": surface, "vendor": vendor, "xbe": xbe}
    fresh = check_installed(out_dir, lifted=lifted, **check)
    if fresh.status != CURRENT:
        raise ValueError("generated pair disagrees with its inputs: " + "; ".join(fresh.problems))
    if install:
        publish(out_dir, gen_dir)
        landed = check_installed(gen_dir, **check)
        if landed.status != CURRENT:
            raise ValueError("installed pair fails its own check: " + "; ".join(landed.problems))
    return manifest


def after_lift(
    *,
    xbe: Path,
    lifted: Path,
    install: bool,
    work: Path,
    entries: Path = DEFAULT_ENTRIES,
    surface_dir: Path = DEFAULT_SURFACE_DIR,
    vendor: Path = DEFAULT_VENDOR,
) -> list[str]:
    """What `tools.lift run` reports about the retained chunk once a lift finished.

    A relift leaves any earlier chunk in place, bound to the old lift, so a stale one is
    named loudly. With `install` the pair is regenerated and installed for the new lift.
    The hint when nothing is installed keeps the optional step discoverable.
    """
    command = f"uv run python -m tools.install_shader_original --xbe {xbe} --install"
    if install:
        manifest = run(
            xbe=xbe,
            lifted=lifted,
            entries=entries,
            work=work,
            install=True,
            surface_dir=surface_dir,
            vendor=vendor,
        )
        return [f"shader original   installed {manifest['profile']} into {lifted / 'gen'}"]
    result = check_installed(
        lifted / "gen",
        entries=entries,
        surface=surface_dir / SURFACE_PAIR[0],
        vendor=vendor,
        xbe=xbe,
    )
    if result.status == STALE:
        return [
            "shader original   STALE, bound to a previous lift or input: "
            + "; ".join(result.problems),
            f"                  reinstall with: {command}",
        ]
    if result.status == ABSENT:
        return [f"shader original   not installed (optional): {command}"]
    return [f"shader original   {result.profile} installed and current"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--xbe",
        type=Path,
        help="the extracted retail default.xbe (required unless --check, which skips its hash)",
    )
    parser.add_argument("--lifted-dir", type=Path, default=Path("generated/lifted"))
    parser.add_argument("--original-entries", type=Path, default=DEFAULT_ENTRIES)
    parser.add_argument("--work-dir", type=Path, default=Path("tmp/shader-original-work"))
    parser.add_argument("--vendor-root", type=Path, default=DEFAULT_VENDOR)
    parser.add_argument(
        "--surface-dir",
        type=Path,
        default=DEFAULT_SURFACE_DIR,
        help="holds the generated xdk_surface.{c,h} and xdk_abi.inc (default: src/xbox)",
    )
    parser.add_argument(
        "--xtlid",
        type=Path,
        help="xtlid.xml, used to generate any missing surface files (a missing file with "
        "no database is refused with the exact commands)",
    )
    parser.add_argument(
        "--install",
        action="store_true",
        help="copy the verified pair into <lifted-dir>/gen (default: generate and verify only)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="only compare the installed pair with its recorded inputs. Exit 0 current, "
        "1 stale or broken, 2 not installed",
    )
    args = parser.parse_args()
    if args.check and args.install:
        parser.error("--check and --install are exclusive")
    if not args.check and args.xbe is None:
        parser.error("--xbe is required unless --check")
    if args.check:
        result = check_installed(
            args.lifted_dir / "gen",
            entries=args.original_entries,
            surface=args.surface_dir / SURFACE_PAIR[0],
            vendor=args.vendor_root,
            xbe=args.xbe,
        )
        if result.status == ABSENT:
            print("shader original: not installed (optional)")
            return EXIT_ABSENT
        if result.status == STALE:
            print("shader original: STALE or broken", file=sys.stderr)
            for problem in result.problems:
                print(f"  {problem}", file=sys.stderr)
            return EXIT_PROBLEMS
        print(f"shader original: {result.profile} installed and current")
        return EXIT_CURRENT
    try:
        manifest = run(
            xbe=args.xbe,
            lifted=args.lifted_dir,
            entries=args.original_entries,
            work=args.work_dir,
            install=args.install,
            surface_dir=args.surface_dir,
            xtlid=args.xtlid,
        )
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"install shader original: {exc}", file=sys.stderr)
        return 1
    bodies = manifest["bodies"]
    print(f"profile {manifest['profile']}: {len(bodies)} original bodies verified")
    print(("installed into " + str(args.lifted_dir / "gen")) if args.install else "not installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
