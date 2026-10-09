# SPDX-License-Identifier: GPL-3.0-or-later
"""Import the xboxrecomp lifter into ``third_party/xboxrecomp/``.

The integration model and the reasoning behind it are in this package's
docstring. This module is the mechanism: it copies a *named* subset of files out
of an upstream checkout, records exactly what it took and the hash of every file,
and refuses to produce a tree that is missing the MIT licence text.

The import is selective on purpose. Upstream's ``src/`` holds nine
LGPL-2.1-or-later files extracted from xemu and we do not want them in this
repository, so ``src/`` is never walked at all; ``FORBIDDEN_PREFIXES`` is a
second, independent check that fails loudly if a future edit to
``VENDORED_PATHS`` would reach them anyway. Two separate mistakes are needed to
vendor an LGPL file, rather than one.

Only files git tracks upstream are copied. That skips ``__pycache__`` and, more
importantly, the ~160 MB of ``output/`` run artifacts that the upstream stages
write back into their own source tree.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

#: Upstream repository, recorded for provenance. Nothing fetches it at build time.
UPSTREAM_URL = "https://github.com/sp00nznet/xboxrecomp"

#: The commit `docs/lifter-evaluation.md` was measured against.
UPSTREAM_COMMIT = "1409a7d"

#: Everything we take, and nothing else. Each entry is a path relative to the
#: upstream repository root.
#:
#:   tools/xbe_parser   stage 1: header, sections, kernel imports
#:   tools/disasm       stage 2: linear sweep + 11-pass function detection
#:   tools/func_id      stage 3: CRT/library/game classification
#:   tools/abi_analysis stage 4a: calling convention and frame shape
#:   tools/recomp       stage 4b: the lifter proper
#:   templates/runtime  recomp_types.h, which the generated C includes
VENDORED_PATHS: tuple[str, ...] = (
    "tools/xbe_parser",
    "tools/disasm",
    "tools/func_id",
    "tools/abi_analysis",
    "tools/recomp",
    "templates/runtime",
)

#: Copied verbatim. MIT obliges us to carry ``LICENSE``; ``NOTICE`` is upstream's
#: own enumeration of its non-MIT files and is what makes the exclusion auditable.
#: ``tools/__init__.py`` is comment-only but is taken anyway, so the vendored
#: ``tools`` is a regular package exactly as upstream has it rather than a PEP 420
#: namespace package that happens to behave the same today.
VENDORED_FILES: tuple[str, ...] = ("LICENSE", "NOTICE", "tools/__init__.py")

#: Paths that must never be imported, checked independently of VENDORED_PATHS.
#: Every LGPL file upstream carries lives under ``src/``.
FORBIDDEN_PREFIXES: tuple[str, ...] = ("src/", "LICENSES/", "tools/conformance/")

#: Written into the vendored root so a reader can tell where the tree came from
#: and ``vendor_status`` can tell whether it is intact.
MANIFEST_NAME = "VENDOR.json"


class VendorStatus(Enum):
    """Whether ``third_party/xboxrecomp/`` is usable."""

    #: No vendored tree at all.
    ABSENT = "absent"
    #: Present, and every file matches the manifest.
    OK = "ok"
    #: Present, every file either pristine or matching a RECORDED local patch.
    #: This is the normal state under the ADAPT verdict and is not an error.
    PATCHED = "patched"
    #: Present but a file matches NEITHER its pristine hash NOR its recorded local
    #: hash. This is the state worth catching: an unrecorded change, which is what an
    #: upstream update landing on top of one of our patches looks like.
    MODIFIED = "modified"


@dataclass(frozen=True)
class VendorManifest:
    """What ``vendor_import`` took, and from where."""

    upstream_url: str
    upstream_commit: str
    paths: tuple[str, ...]
    #: Relative path -> sha256 of the PRISTINE upstream file, for every file imported.
    files: dict[str, str]
    #: Relative path -> sha256 of our LOCALLY PATCHED file, for the subset we patch.
    #:
    #: Without this the manifest cannot do its job. Recording only pristine hashes
    #: means a tree carrying 14 local patches reports MODIFIED permanently, so the
    #: one thing worth detecting -- an upstream change landing on top of a patch --
    #: is indistinguishable from the patch itself. With both, a file matching
    #: neither hash is unambiguously an unrecorded change.
    local: dict[str, str] = field(default_factory=dict)

    def to_json(self) -> str:
        return json.dumps(
            {
                "upstream_url": self.upstream_url,
                "upstream_commit": self.upstream_commit,
                "paths": list(self.paths),
                "files": dict(sorted(self.files.items())),
                "local": dict(sorted(self.local.items())),
            },
            indent=2,
        )

    @staticmethod
    def from_json(text: str) -> VendorManifest:
        raw = json.loads(text)
        return VendorManifest(
            upstream_url=str(raw["upstream_url"]),
            upstream_commit=str(raw["upstream_commit"]),
            paths=tuple(str(p) for p in raw["paths"]),
            files={str(k): str(v) for k, v in raw["files"].items()},
            local={str(k): str(v) for k, v in raw.get("local", {}).items()},
        )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _tracked_files(source: Path, prefixes: tuple[str, ...]) -> list[str]:
    """Git-tracked files under `prefixes`, relative to `source`.

    Using git's index rather than a directory walk is what keeps the 160 MB of
    ``tools/disasm/output`` and every ``__pycache__`` out of the import.
    """
    completed = subprocess.run(
        ["git", "-C", str(source), "ls-files", "-z", *prefixes],
        capture_output=True,
        check=True,
        text=True,
        timeout=120,
    )
    return sorted(name for name in completed.stdout.split("\0") if name)


def _reject_forbidden(names: list[str]) -> None:
    offenders = [n for n in names if n.startswith(FORBIDDEN_PREFIXES)]
    if offenders:
        raise ValueError(
            "refusing to vendor excluded paths (the LGPL files live under src/): "
            + ", ".join(offenders[:10])
        )


def vendor_import(source: Path, dest: Path) -> VendorManifest:
    """Copy the vendored subset from an upstream checkout into `dest`.

    Replaces `dest` wholesale, so the result depends only on `source` and on
    ``VENDORED_PATHS`` -- re-running it on an unchanged source is a no-op in
    content, which is what makes the lift reproducible.

    Raises ``FileNotFoundError`` if `source` is not an upstream checkout, and
    ``ValueError`` if the selection would reach an excluded path or if the MIT
    licence text is absent.
    """
    source = source.resolve()
    if not (source / ".git").exists():
        raise FileNotFoundError(f"{source} is not a git checkout of xboxrecomp")

    names = _tracked_files(source, VENDORED_PATHS)
    if not names:
        raise FileNotFoundError(f"{source} tracks none of {VENDORED_PATHS}")
    for required in VENDORED_FILES:
        if not (source / required).is_file():
            raise ValueError(
                f"{source}/{required} is missing; MIT requires the licence text to "
                "travel with the code, so this import would be non-compliant"
            )
        names.append(required)
    _reject_forbidden(names)

    if dest.exists():
        shutil.rmtree(dest)
    digests: dict[str, str] = {}
    for name in names:
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
        digests[name] = _sha256(target)

    commit = _describe_commit(source)
    manifest = VendorManifest(
        upstream_url=UPSTREAM_URL,
        upstream_commit=commit,
        paths=VENDORED_PATHS,
        files=digests,
    )
    (dest / MANIFEST_NAME).write_text(manifest.to_json() + "\n", encoding="utf-8")
    return manifest


def _describe_commit(source: Path) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "HEAD"],
            capture_output=True,
            check=True,
            text=True,
            timeout=30,
        )
    except (subprocess.CalledProcessError, OSError):
        return UPSTREAM_COMMIT
    return completed.stdout.strip() or UPSTREAM_COMMIT


def vendor_status(root: Path) -> VendorStatus:
    """Classify the vendored tree at `root` without modifying it."""
    manifest_path = root / MANIFEST_NAME
    if not manifest_path.is_file():
        return VendorStatus.ABSENT
    try:
        manifest = VendorManifest.from_json(manifest_path.read_text(encoding="utf-8"))
    except (ValueError, KeyError):
        return VendorStatus.MODIFIED
    patched = False
    for name, digest in manifest.files.items():
        candidate = root / name
        if not candidate.is_file():
            return VendorStatus.MODIFIED
        actual = _sha256(candidate)
        if actual == digest:
            continue
        if actual == manifest.local.get(name):
            patched = True
            continue
        # Matches neither pristine nor a recorded local patch. This is the case the
        # manifest exists to catch.
        return VendorStatus.MODIFIED
    return VendorStatus.PATCHED if patched else VendorStatus.OK
