# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""The pinned, patched DMTCP the snapshots are taken with, built into the gitignored `tmp/dmtcp`.

DMTCP 3.2.0 (checkpoint-restore in user space, no kernel privilege) is the only route that works in this
container: CRIU needs sysctls and mounts that sysboxfs refuses (docs/state-snapshot.md). Stock 3.2.0 cannot
restore the host because Mesa's lavapipe sub-allocates ONE anonymous memfd into hundreds of mappings and
DMTCP neither tracks memfd_create nor restores such a mapping, so the patch `dmtcp-3.2.0-tsfp.patch` adds both.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = "https://github.com/dmtcp/dmtcp.git"
TAG = "3.2.0"
COMMIT = "bc38d1a3bdfca87905f1a3adfada1e63d64042e5"
PATCH = Path(__file__).with_name("dmtcp-3.2.0-tsfp.patch")
PLUGIN_SOURCE = Path(__file__).with_name("tsfp_snapshot_plugin.c")
WORK = Path("tmp/dmtcp")
STEP_TIMEOUT = 3600
BINARIES = ("dmtcp_launch", "dmtcp_restart", "dmtcp_coordinator", "dmtcp_command")
PLUGIN_LIB = Path("lib/libtsfp_snapshot.so")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main_root() -> Path:
    """The main checkout (a gitignored build lives there and a worktree reuses it)."""
    env = os.environ.get("TSFP_MAIN_ROOT")
    if env:
        return Path(env).resolve()
    probe = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    return Path(probe.stdout.strip()).parent if probe.returncode == 0 else ROOT


def install_dir(base: Path) -> Path:
    """Named by the patch and plugin hashes, so a changed patch builds a new tree."""
    key = hashlib.sha256(PATCH.read_bytes() + PLUGIN_SOURCE.read_bytes()).hexdigest()[:12]
    return base / WORK / f"install-{TAG}-{key}"


def locate() -> dict[str, Path]:
    """Paths of the four binaries and the plugin, or SystemExit with the exact build command."""
    env = os.environ.get("TSFP_DMTCP_ROOT")
    candidates = [Path(env)] if env else [install_dir(ROOT), install_dir(main_root())]
    for prefix in candidates:
        found = {name: prefix / "bin" / name for name in BINARIES}
        found["plugin"] = prefix / PLUGIN_LIB
        found["prefix"] = prefix
        if all(path.exists() for path in found.values()):
            return found
    raise SystemExit(
        "snapshot: the patched DMTCP is not built. Run: python -m tools.snapshot build-dmtcp"
    )


def build_info(prefix: Path) -> dict[str, str]:
    """What a snapshot manifest pins: tag, commit, patch and plugin hashes, launcher binary hash."""
    return {
        "tag": TAG,
        "commit": COMMIT,
        "patch_sha256": sha256_file(PATCH),
        "plugin_sha256": sha256_file(prefix / PLUGIN_LIB),
        "launch_sha256": sha256_file(prefix / "bin/dmtcp_launch"),
    }


def run(argv: list[str], cwd: Path, log: Path, label: str) -> None:
    print(f"[{label}] {' '.join(argv)}", flush=True)
    with log.open("w") as handle:
        done = subprocess.run(
            argv,
            cwd=cwd,
            stdout=handle,
            stderr=subprocess.STDOUT,
            timeout=STEP_TIMEOUT,
            check=False,
        )
    if done.returncode != 0:
        tail = "\n".join(log.read_text(errors="replace").splitlines()[-15:])
        raise SystemExit(f"{label}: exit {done.returncode}\n{tail}")


def build() -> Path:
    base = main_root()
    prefix = install_dir(base)
    if (
        all((prefix / "bin" / name).exists() for name in BINARIES)
        and (prefix / PLUGIN_LIB).exists()
    ):
        print(f"[dmtcp] cache hit {prefix}")
        return prefix
    work = base / WORK
    source = work / f"src-{TAG}"
    work.mkdir(parents=True, exist_ok=True)
    if not source.is_dir():
        run(
            ["git", "clone", "--depth", "1", "--branch", TAG, REPOSITORY, str(source)],
            work,
            work / "clone.log",
            "clone",
        )
    head = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    ).stdout.strip()
    if head != COMMIT:
        raise SystemExit(f"dmtcp source {source} is at {head}, expected the pinned {COMMIT}")
    subprocess.run(["git", "-C", str(source), "checkout", "--", "."], timeout=60, check=True)
    run(["git", "-C", str(source), "apply", str(PATCH)], work, work / "apply.log", "apply-patch")
    run(["./configure", f"--prefix={prefix}"], source, work / "configure.log", "configure")
    run(["make", "-j", str(os.cpu_count() or 4)], source, work / "make.log", "make")
    run(["make", "install"], source, work / "install.log", "install")
    plugin_command = [
        "gcc",
        "-shared",
        "-fPIC",
        "-Wall",
        "-Wextra",
        f"-I{source / 'include'}",
        "-o",
        str(prefix / PLUGIN_LIB),
        str(PLUGIN_SOURCE),
    ]
    run(plugin_command, work, work / "plugin.log", "plugin")
    (prefix / "BUILD.json").write_text(json.dumps(build_info(prefix), indent=2) + "\n")
    print(f"[dmtcp] built {prefix}")
    return prefix


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "locate"))
    args = parser.parse_args(argv)
    if args.action == "build":
        build()
    else:
        print(json.dumps({key: str(value) for key, value in locate().items()}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
