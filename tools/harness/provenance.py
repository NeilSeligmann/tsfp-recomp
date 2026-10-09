# SPDX-License-Identifier: GPL-3.0-or-later
"""Which lifted tree the subject under test was built from.

WHY THIS MODULE EXISTS
----------------------
`results.csv` recorded the seed and the case index, so any case could be replayed
exactly -- but not WHICH LIFTED TREE the subject was built from. Three successive
wrong answers went into the project record and were retracted because of that one
omission: "0 DISAGREE" became "384 DISAGREE, a real lifter defect" became "the
defect was already fixed; the subject was built from a lift ten hours older than
the fix". An instrument that measures something other than what is shipped, with
nothing in its output saying so, is worse than a wrong answer, because a wrong
answer invites checking.

So every row of the results CSV and every summary carries the provenance, and the
provenance comes from the SUBJECT BINARY ITSELF rather than from a command-line
flag. `build_subject.sh` bakes it in with -D at the compile of driver.c and the
driver reports it on its READY line, for the same reason `stub=1` is derived from
the objects by inspection: a value a human can assert is a value a human can
assert wrongly.

TWO FIELDS, AND WHY BOTH
------------------------
`gen_dir` answers "which tree", `tree_sha` answers "which contents of it".
`generated/lifted/gen` is regenerated in place, so a stale subject has a gen_dir
that matches the shipped tree exactly while measuring different bytes. The path
alone would have let the original failure through a second time.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

#: The tree the repo ships and that CMake builds the host from by default. A run
#: against anything else is legitimate (the single-variable control that settled
#: the g_ebp question measured two trees on purpose) but must be deliberate.
SHIPPED_GEN_DIR = Path("generated/lifted/gen")

#: File kinds that make up a lifted tree's identity. `.inc` is included because
#: `kernel_arity.inc` is #included by the generated chunks, so a change to it
#: changes what the objects do.
TREE_SUFFIXES = frozenset({".c", ".h", ".inc"})

#: What a subject built without `build_subject.sh` reports. Deliberately not an
#: empty string: a blank CSV cell reads as "no provenance needed here", and this
#: has to read as "nobody recorded it".
UNKNOWN = "UNKNOWN"

#: Width of the digest carried on every CSV row. The full 64 hex digits live in
#: the summary and the sidecar; 16 is 64 bits of discrimination, which is ample
#: against the tens of trees this project will ever hold, and keeps the per-row
#: cost of universal provenance down.
SHORT_SHA_CHARS = 16


def tree_digest(gen_dir: Path | str) -> str:
    """The 64-hex content digest of a lifted tree.

    Mirrors `harness_tree_digest` in tools/harness/tree_digest.sh exactly, down to
    the two spaces `sha256sum` puts between hash and name and the newline it puts
    after every line including the last. `test_the_shell_and_python_tree_digests_agree`
    pins the two together: a Python reader that computed a different digest from the
    shell writer would make every provenance check pass vacuously.
    """
    directory = Path(gen_dir)
    if not directory.is_dir():
        raise NotADirectoryError(f"not a directory: {directory}")
    names = sorted(
        entry.name
        for entry in directory.iterdir()
        if not entry.is_dir() and entry.suffix in TREE_SUFFIXES
    )
    if not names:
        raise ValueError(f"no {'/'.join(sorted(TREE_SUFFIXES))} files in {directory}")
    listing = "".join(
        f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n" for name in names
    )
    return hashlib.sha256(listing.encode()).hexdigest()


def short_sha(full: str) -> str:
    """Truncate a digest for the CSV, passing UNKNOWN through unchanged."""
    if full == UNKNOWN:
        return UNKNOWN
    return full[:SHORT_SHA_CHARS]


@dataclass(frozen=True)
class Provenance:
    """What the subject says it was built from. Never inferred, never defaulted."""

    gen_dir: str = UNKNOWN
    tree_sha: str = UNKNOWN
    #: How many hand-written replacements (src/game) are linked into the subject, and the
    #: short content digest of src/game when it was built. A subject with none reports
    #: `0` and `NONE`. Kept apart from `known` on purpose: every existing subject is
    #: "known" without them.
    repl_count: int = 0
    repl_sha: str = "NONE"

    @property
    def known(self) -> bool:
        return self.gen_dir != UNKNOWN and self.tree_sha != UNKNOWN

    @classmethod
    def from_ready_line(cls, line: str) -> Provenance:
        """Parse the driver's READY line.

        `gen_dir=` is emitted LAST and consumes the rest of the line, so a path
        containing a space cannot swallow a following field or be truncated by one.
        """
        gen_dir = UNKNOWN
        head = line
        marker = " gen_dir="
        at = line.find(marker)
        if at >= 0:
            head = line[:at]
            gen_dir = line[at + len(marker) :].strip() or UNKNOWN
        tree_sha = UNKNOWN
        repl_count = 0
        repl_sha = "NONE"
        for token in head.split():
            key, sep, value = token.partition("=")
            if not (sep and value):
                continue
            if key == "tree_sha":
                tree_sha = value
            elif key == "repl" and value.isdigit():
                repl_count = int(value)
            elif key == "repl_sha":
                repl_sha = value
        return cls(gen_dir=gen_dir, tree_sha=tree_sha, repl_count=repl_count, repl_sha=repl_sha)

    def describe(self) -> str:
        text = f"gen_dir={self.gen_dir} tree_sha={self.tree_sha}"
        if self.repl_count:
            text += f" replacements={self.repl_count} repl_sha={self.repl_sha}"
        return text


def shipped_mismatch(
    provenance: Provenance, shipped_gen_dir: Path | str = SHIPPED_GEN_DIR
) -> str | None:
    """Why this subject does not measure the tree the repo ships, or None.

    Three distinguishable answers, because they call for different reactions:

    - unknown provenance: the subject was not built by `build_subject.sh`, so
      nothing at all is known about what it measures.
    - a different directory: a deliberate control, or the wrong binary picked up
      from a scratch directory. Indistinguishable from the outside, so the caller
      has to be made to choose.
    - the same directory, different contents: a STALE subject. This is the exact
      shape of the failure that produced three retracted answers, and the shape a
      path-only check cannot see.
    """
    if not provenance.known:
        return (
            "the subject reports no provenance, so what it measures is unknown. "
            "Rebuild it with tools/harness/build_subject.sh, which bakes the "
            "gen_dir and the tree digest into the binary."
        )
    shipped = Path(shipped_gen_dir)
    try:
        expected = short_sha(tree_digest(shipped))
    except (NotADirectoryError, ValueError) as exc:
        return f"the shipped tree {shipped} could not be digested: {exc}"
    if provenance.tree_sha == expected:
        return None
    if Path(provenance.gen_dir).resolve() == shipped.resolve():
        return (
            f"STALE SUBJECT: it was built from {provenance.gen_dir} as that tree stood "
            f"at tree_sha={provenance.tree_sha}, but that directory now digests to "
            f"{expected}. The path matches and the contents do not, which is exactly "
            "the failure that made a ten-hour-old lift look like a lifter defect."
        )
    return (
        f"the subject was built from {provenance.gen_dir} (tree_sha={provenance.tree_sha}), "
        f"not from the shipped tree {shipped} (tree_sha={expected})."
    )
