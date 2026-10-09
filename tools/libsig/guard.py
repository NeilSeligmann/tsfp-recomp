# SPDX-License-Identifier: GPL-3.0-or-later
"""Provenance guard: signature databases and match outputs stay in ignored trees."""

from __future__ import annotations

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ALLOWED_TOPS = ("tmp", "generated")


class ProvenanceError(RuntimeError):
    """Raised when an output path would land outside the gitignored trees."""


def check_output_path(path: Path, allow_outside: bool = False, root: Path = REPO_ROOT) -> Path:
    """Return `path` if it resolves under `<root>/tmp` or `<root>/generated`.

    Symlinks and `..` are resolved first, so a path cannot dodge the check.
    """
    resolved = path.resolve()
    for top in ALLOWED_TOPS:
        if resolved.is_relative_to((root / top).resolve()):
            return path
    if allow_outside:
        return path
    raise ProvenanceError(
        f"refusing to write {path}: library-derived output must live under "
        f"{' or '.join(ALLOWED_TOPS)}/ (pass --allow-outside-ignored to override)"
    )
