# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""The snapshot manifest and the loud refusal of every snapshot that is not valid for the current inputs.

A snapshot is a DMTCP image of the whole process tree. It maps the host binary and every shared library
(libc, libvulkan_lvp, SDL3, ...) from the file system at the same paths on restore, so it is valid only for
the exact bytes it was taken on. The manifest pins them with sha256 and `verify` returns one problem line
per mismatch; the caller refuses to resume when the list is not empty. Nothing here is forced or guessed.
"""

import hashlib
import json
import os
import platform
import re
import time
from pathlib import Path

from tools.snapshot import dmtcp

MANIFEST_NAME = "manifest.json"
VERSION = 1
LIFT_KEY_PATTERN = re.compile(r"build-([0-9a-f]{16})")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def lift_key_of(host: Path) -> str:
    """The private-host lift key of `.../build-<key>/tsfp_host` (tools.private_host), else 'unknown'."""
    for part in reversed(host.resolve().parts):
        found = LIFT_KEY_PATTERN.fullmatch(part)
        if found:
            return found.group(1)
    return "unknown"


def lift_inputs_sha256(host: Path) -> str:
    """sha256 of the lift's KEY.txt (the list of every lift input hash) next to the host build, or ''."""
    key = lift_key_of(host)
    if key == "unknown":
        return ""
    for parent in host.resolve().parents:
        candidate = parent / f"lift-{key}" / "KEY.txt"
        if candidate.is_file():
            return sha256_file(candidate)
    return ""


def is_pinned(path: Path, host: Path | None = None) -> bool:
    """Executables and shared libraries are pinned. Mutable mappings (Mesa's shader cache index under ~/.cache,
    which every Vulkan run of any agent rewrites) are not: a changed cache must not refuse a snapshot."""
    if host is not None and path == host:
        return True
    return ".so" in path.name


def hash_files(paths: list[Path]) -> dict[str, str]:
    """sha256 of every existing file, keyed by absolute path. A vanished file is simply absent."""
    return {str(path): sha256_file(path) for path in sorted(set(paths)) if path.is_file()}


def read_mapped_files(snapshot_dir: Path) -> list[Path]:
    """The file-backed mappings the host listed just before the checkpoint (host_snapshot.c)."""
    listing = snapshot_dir / "mapped-files.txt"
    if not listing.is_file():
        return []
    return [Path(line) for line in listing.read_text().splitlines() if line.startswith("/")]


def image_files(ckpt_dir: Path) -> list[Path]:
    return sorted(ckpt_dir.glob("ckpt_*.dmtcp"))


def describe_images(ckpt_dir: Path, with_hash: bool) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for path in image_files(ckpt_dir):
        entry: dict[str, object] = {"bytes": path.stat().st_size}
        if with_hash:
            entry["sha256"] = sha256_file(path)
        result[path.name] = entry
    return result


def build_manifest(
    *,
    poll: int,
    play_argv: list[str],
    host: Path,
    xbe: Path,
    record: Path | None,
    run_dir: Path,
    display: int,
    snapshot_dir: Path,
    gzip: bool,
    seconds: dict[str, float],
    repository_head: str,
) -> dict[str, object]:
    ckpt = snapshot_dir / "ckpt"
    tools = dmtcp.locate()
    return {
        "version": VERSION,
        "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "poll": poll,
        "host": {
            "path": str(host.resolve()),
            "sha256": sha256_file(host),
            "lift_key": lift_key_of(host),
            "lift_inputs_sha256": lift_inputs_sha256(host),
        },
        "xbe": {"path": str(xbe.resolve()), "sha256": sha256_file(xbe)},
        "record": (
            {"path": str(record.resolve()), "sha256": sha256_file(record)} if record else None
        ),
        "play_argv": play_argv,
        "run_dir": str(run_dir),
        "display": display,
        "dmtcp": dmtcp.build_info(tools["prefix"]),
        "gzip": gzip,
        "mapped_files": hash_files(
            [path for path in read_mapped_files(snapshot_dir) if is_pinned(path, host.resolve())]
        ),
        "unpinned_mappings": sorted(
            str(path)
            for path in read_mapped_files(snapshot_dir)
            if not is_pinned(path, host.resolve())
        ),
        "images": describe_images(ckpt, with_hash=True),
        "machine": {
            "kernel": os.uname().release,
            "arch": os.uname().machine,
            "libc": " ".join(platform.libc_ver()),
        },
        "repository_head": repository_head,
        "seconds": {key: round(value, 2) for key, value in seconds.items()},
    }


def load(snapshot_dir: Path) -> dict[str, object]:
    path = snapshot_dir / MANIFEST_NAME
    if not path.is_file():
        raise SystemExit(f"snapshot: {snapshot_dir} has no {MANIFEST_NAME}, it is not a snapshot")
    return json.loads(path.read_text())


def x_display_in_use(display: int) -> str:
    """Why X display :n cannot be reused (a live server owns it), else ''. A stale lock file is fine."""
    lock = Path(f"/tmp/.X{display}-lock")
    if not lock.is_file():
        return ""
    try:
        pid = int(lock.read_text().strip())
    except ValueError:
        return ""
    return (
        f"X display :{display} is held by live process {pid}"
        if Path(f"/proc/{pid}").exists()
        else ""
    )


def _check_file(label: str, path_text: str, expected: str, problems: list[str]) -> None:
    path = Path(path_text)
    if not path.is_file():
        problems.append(f"{label} {path} is gone, the snapshot maps it at that path")
    elif sha256_file(path) != expected:
        problems.append(
            f"{label} {path} changed since the snapshot (sha256 differs from {expected[:16]}...)"
        )


def verify(
    manifest: dict[str, object],
    snapshot_dir: Path,
    *,
    host_override: Path | None = None,
    fast: bool = False,
    check_display: bool = True,
) -> list[str]:
    """Every reason the snapshot is not valid now, one line each. Empty means it may be resumed."""
    problems: list[str] = []
    if manifest.get("version") != VERSION:
        problems.append(f"manifest version {manifest.get('version')} is not {VERSION}")
        return problems
    host = manifest["host"]
    assert isinstance(host, dict)
    _check_file("host binary", str(host["path"]), str(host["sha256"]), problems)
    if host_override is not None:
        if not host_override.is_file():
            problems.append(f"--host {host_override} does not exist")
        elif sha256_file(host_override) != host["sha256"]:
            problems.append(
                f"--host {host_override} is a different build than the snapshot's host "
                f"(lift key {host['lift_key']}, sha256 {str(host['sha256'])[:16]}...): a snapshot "
                "only continues the exact binary it was taken on, take a new one"
            )
    xbe = manifest["xbe"]
    assert isinstance(xbe, dict)
    _check_file("XBE", str(xbe["path"]), str(xbe["sha256"]), problems)
    record = manifest.get("record")
    if isinstance(record, dict):
        _check_file("recorded input", str(record["path"]), str(record["sha256"]), problems)
    mapped = manifest["mapped_files"]
    assert isinstance(mapped, dict)
    for path_text, digest in mapped.items():
        if path_text == host["path"]:
            continue
        _check_file("mapped library", path_text, str(digest), problems)
    recorded = manifest["dmtcp"]
    assert isinstance(recorded, dict)
    try:
        prefix = dmtcp.locate()["prefix"]
        current = dmtcp.build_info(prefix)
    except SystemExit as error:
        problems.append(str(error))
    else:
        for key, value in current.items():
            if recorded.get(key) != value:
                problems.append(f"DMTCP build differs from the snapshot's ({key})")
    images = manifest["images"]
    assert isinstance(images, dict)
    if not images:
        problems.append("the snapshot has no checkpoint images")
    for name, entry in images.items():
        image = snapshot_dir / "ckpt" / name
        if not image.is_file():
            problems.append(f"checkpoint image {name} is missing")
        elif image.stat().st_size != entry["bytes"]:
            problems.append(f"checkpoint image {name} has a different size than recorded")
        elif not fast and sha256_file(image) != entry["sha256"]:
            problems.append(f"checkpoint image {name} is corrupt (sha256 differs)")
    if not (snapshot_dir / "run-dir").is_dir():
        problems.append("the run directory copy (the HDD files at the snapshot instant) is missing")
    machine = manifest["machine"]
    assert isinstance(machine, dict)
    if machine.get("arch") != os.uname().machine:
        problems.append(f"snapshot is for {machine.get('arch')}, this is {os.uname().machine}")
    if check_display:
        busy = x_display_in_use(int(str(manifest["display"])))
        if busy:
            problems.append(busy + " (the restored Xvfb needs it)")
    return problems
