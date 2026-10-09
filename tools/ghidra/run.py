# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive Ghidra headless: import an XBE, analyse it, export decompiled C.

The pipeline is deliberately reproducible and re-runnable. Its output under
`generated/` is regenerable and must never be hand-edited; hand-written
replacements live in `patched/` and shadow it at link time via weak-symbol
aliasing.

Setup this expects, established by `tools/ghidra/setup.sh`:

* Ghidra **12.0.3** specifically. The XBE loader extension is version-pinned and
  its newest prebuilt release targets 12.0.3, so a newer Ghidra silently fails
  with "No load spec found".
* The loader installed into the **user** extension directory
  (`~/.config/ghidra/ghidra_12.0.3_PUBLIC/Extensions/`). Dropping it into
  `<install>/Extensions/Ghidra/` alone is not picked up by headless.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

GHIDRA_VERSION = "12.0.3"
GHIDRA_ENV = "GHIDRA_INSTALL_DIR"
SCRIPT_DIR = Path(__file__).resolve().parent


def ghidra_dir(explicit: Path | None) -> Path:
    """Locate the Ghidra installation, preferring an explicit path."""
    if explicit is not None:
        return explicit
    from_env = os.environ.get(GHIDRA_ENV)
    if from_env:
        return Path(from_env)
    raise SystemExit(
        f"Ghidra not found. Pass --ghidra or set {GHIDRA_ENV}. "
        f"Version {GHIDRA_VERSION} is required: the XBE loader extension is "
        f"version-pinned and newer Ghidra fails with 'No load spec found'."
    )


def headless(install: Path) -> Path:
    binary = install / "support" / "analyzeHeadless"
    if not binary.is_file():
        raise SystemExit(f"not a Ghidra installation: {install} (no {binary})")
    return binary


def run(command: list[str], log: Path) -> int:
    """Run a command, tee-ing combined output to `log`. Returns the exit code."""
    log.parent.mkdir(parents=True, exist_ok=True)
    print("+ " + " ".join(command), flush=True)
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        assert process.stdout is not None
        for line in process.stdout:
            stream.write(line)
            # Ghidra is extremely chatty; surface only what matters.
            if any(
                tag in line for tag in ("ERROR", "Using Loader", "REPORT", "exporting", "done:")
            ):
                print("  " + line.rstrip(), flush=True)
        return process.wait()


def analyse_command(
    binary: Path,
    project: Path,
    name: str,
    xbe_name: str,
    timeout: int,
    stock: bool = False,
) -> list[str]:
    """The headless command line for the analyse stage.

    `EnableDeepAnalysis.java` must run as a **pre**-script: it flips analyzer
    options, which only has an effect before analysis begins. Passing it as a
    post-script would run it after the work it was meant to configure.

    This stage carried no `-preScript` at all until it was measured, which is
    precisely why the project we had been treating as our baseline was a stock one:
    the better configuration was reachable only by bypassing this driver, so nobody
    was getting it. MEASURED on retail TSFP, 69.91% -> 82.79% decompiler-clean for
    about 74 extra seconds, with the recovered function table bit-identical either
    way. It is therefore the default, and `stock=True` exists only to reproduce the
    old baseline.
    """
    command = [
        str(binary),
        str(project),
        name,
        "-process",
        xbe_name,
        "-analysisTimeoutPerFile",
        str(timeout),
    ]
    if not stock:
        command += ["-scriptPath", str(SCRIPT_DIR), "-preScript", "EnableDeepAnalysis.java"]
    return command


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Import, analyse and export an XBE with Ghidra headless."
    )
    parser.add_argument("xbe", type=Path, help="path to the XBE to process")
    parser.add_argument(
        "--ghidra", type=Path, default=None, help=f"Ghidra {GHIDRA_VERSION} install dir"
    )
    parser.add_argument(
        "--project", type=Path, default=Path("build/ghidra"), help="Ghidra project dir"
    )
    parser.add_argument("--name", default="tsfp", help="Ghidra project name")
    parser.add_argument("--out", type=Path, default=Path("generated"), help="export destination")
    parser.add_argument(
        "--stage",
        choices=("import", "analyse", "export", "all"),
        default="all",
        help="run only one stage; 'all' runs them in order",
    )
    parser.add_argument(
        "--timeout", type=int, default=6000, help="Ghidra analysis timeout, seconds"
    )
    parser.add_argument(
        "--stock",
        action="store_true",
        help=(
            "analyse with Ghidra's default options instead of enabling Decompiler "
            "Parameter ID. Measured 12.9 percentage points WORSE on decompiler-clean "
            "output, so this exists for reproducing the old baseline, not for use"
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    install = ghidra_dir(args.ghidra)
    binary = headless(install)
    project = args.project
    logs = project / "logs"

    if not args.xbe.is_file():
        raise SystemExit(f"no such file: {args.xbe}")

    stages = ("import", "analyse", "export") if args.stage == "all" else (args.stage,)

    if "import" in stages:
        project.mkdir(parents=True, exist_ok=True)
        code = run(
            [str(binary), str(project), args.name, "-import", str(args.xbe), "-noanalysis"],
            logs / "import.log",
        )
        if code != 0:
            return code

    if "analyse" in stages:
        code = run(
            analyse_command(binary, project, args.name, args.xbe.name, args.timeout, args.stock),
            logs / "analyse.log",
        )
        if code != 0:
            return code

    if "export" in stages:
        args.out.mkdir(parents=True, exist_ok=True)
        code = run(
            [
                str(binary),
                str(project),
                args.name,
                "-process",
                args.xbe.name,
                "-noanalysis",
                "-scriptPath",
                str(SCRIPT_DIR),
                "-postScript",
                "ExportDecompiledC.java",
                str(args.out.resolve()),
            ],
            logs / "export.log",
        )
        if code != 0:
            return code
        manifest = args.out / "manifest.json"
        if manifest.is_file():
            print(f"manifest: {manifest} ({manifest.stat().st_size} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
