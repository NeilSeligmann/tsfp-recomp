# SPDX-License-Identifier: GPL-3.0-or-later
"""Loud, countable skips for tests that need gitignored inputs (T715).

A skip reads as a pass in most runners, so an input-dependent test that skips hides
regressions (T709: the T698 harness break went unseen).  Such skips carry a reason that
starts with `INPUT_SKIP_PREFIX` and names the missing input; `tests/conftest.py` prints
`input_skip_summary` at the end of every run.  Skips stay skips: absent inputs never fail.
"""

import os
import subprocess
from collections.abc import Iterable
from pathlib import Path

INPUT_SKIP_PREFIX = "missing input: "


def missing_inputs(paths: Iterable[Path], root: Path) -> list[str]:
    """Names (relative to root when possible) of the paths that do not exist."""
    names = []
    for path in paths:
        if not path.exists():
            try:
                names.append(str(path.relative_to(root)))
            except ValueError:
                names.append(str(path))
    return names


def input_skip_reason(missing: list[str], purpose: str) -> str:
    """Reason string naming every missing input and what is therefore not exercised."""
    return f"{INPUT_SKIP_PREFIX}{', '.join(missing)} ({purpose})"


def unrunnable_binary(path: Path) -> str | None:
    """Why an existing binary cannot start here (`ldd` reports a missing library or glibc
    symbol version, i.e. a stale build from a newer machine), else None."""
    if not path.exists():
        return None
    loader = subprocess.run(  # noqa: S603
        ["ldd", str(path)], capture_output=True, text=True, timeout=60, check=False  # noqa: S607
    )  # fmt: skip
    output = loader.stdout + loader.stderr
    if "not found" in output:
        return next(line.strip() for line in output.splitlines() if "not found" in line)
    return None


HOST_NAME = "tsfp_host"


def _missing_item(loader_output: str) -> str:
    """The named library or glibc version of the first `not found` line of `ldd` output."""
    line = next(line.strip() for line in loader_output.splitlines() if "not found" in line)
    if "version `" in line:
        return line.split("version `", 1)[1].split("'", 1)[0] + " not found"
    return line.split("=>")[0].strip() + " not found"


def host_runnable(path: Path) -> str | None:
    """None when the host binary can start here, else the reason it cannot (T813).

    The prebuilt gitignored hosts are overwritten by builds from other machines, so a file
    that exists may still need a library or glibc version this container lacks. The reason
    names the exact missing library or version and is ready for `pytest.skip`.
    """
    rebuild = "rebuild with `ninja -C build tsfp_host` into a private TSFP_BUILD_DIR or install it"
    if not path.is_file():
        return f"host cannot run: {path} does not exist ({rebuild})"
    if not os.access(path, os.X_OK):
        return f"host cannot run: {path} is not executable (chmod +x or {rebuild})"
    try:
        loader = subprocess.run(  # noqa: S603
            ["ldd", str(path)], capture_output=True, text=True, timeout=60, check=False  # noqa: S607
        )  # fmt: skip
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"host cannot run: ldd failed on {path} ({error})"
    output = loader.stdout + loader.stderr
    if "not found" in output:
        return f"host cannot run: {_missing_item(output)} ({rebuild})"
    return None


def host_skip_reason(path: Path) -> str | None:
    """`host_runnable` as a loud input-skip reason (counted by `input_skip_summary`)."""
    reason = host_runnable(path)
    return None if reason is None else f"{INPUT_SKIP_PREFIX}{reason}"


_HOST_CACHE: dict[tuple[str, int, int], str | None] = {}


def launch_skip_reason(args: object) -> str | None:
    """Skip reason when `args` (a Popen command) would launch an existing but unrunnable
    `tsfp_host`, else None. Cached per file and mtime so a module launching the host many
    times probes it once. A missing path is left to the caller's own guard."""
    first = args if isinstance(args, str | os.PathLike) else (args[0] if args else None)  # type: ignore[index]
    if not isinstance(first, str | os.PathLike) or Path(first).name != HOST_NAME:
        return None
    path = Path(first)
    if not path.is_file():
        return None
    info = path.stat()
    key = (str(path.resolve()), info.st_mtime_ns, info.st_size)
    if key not in _HOST_CACHE:
        _HOST_CACHE[key] = host_skip_reason(path)
    return _HOST_CACHE[key]


def unsupported_flags(path: Path, flags: Iterable[str]) -> list[str]:
    """The `flags` a built host's `--help` does not list (a build older than the test)."""
    if not path.exists():
        return []
    helped = subprocess.run(  # noqa: S603
        [str(path), "--help"], capture_output=True, text=True, timeout=60, check=False
    )
    text = helped.stdout + helped.stderr
    return [flag for flag in flags if flag not in text]


def skip_reason_text(longrepr: object) -> str:
    """Reason of a skipped report: pytest stores (file, line, 'Skipped: reason')."""
    text = longrepr[2] if isinstance(longrepr, tuple) and len(longrepr) == 3 else str(longrepr)
    return text.removeprefix("Skipped: ")


def input_skip_summary(skipped: Iterable[tuple[str, str]]) -> str | None:
    """One line for `(nodeid, reason)` skips whose reason is an input skip, else None."""
    ids = sorted(node for node, reason in skipped if reason.startswith(INPUT_SKIP_PREFIX))
    if not ids:
        return None
    return (
        f"INPUT-DEPENDENT SKIPS: {len(ids)} test(s) did NOT run for lack of inputs: "
        + ", ".join(ids)
    )
