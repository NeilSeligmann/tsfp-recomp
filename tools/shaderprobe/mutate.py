# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutation sweep for `tools.shaderprobe`: apply one textual defect at a time, run the tests.

    python -m tools.shaderprobe.mutate               # all mutations
    python -m tools.shaderprobe.mutate --only analyze-call-length

Each mutation swaps one exact string. The `old` string must occur exactly once or the run
reports ANCHOR-DRIFT, because a drifted anchor that silently changes nothing looks exactly
like a mutant the tests killed. The package under test is COPIED into a scratch tree whose
`tools/` is symlinks to the real one except `shaderprobe`, so the repository is never
modified and a killed run leaves nothing to restore. The tests that need the user's XBE
(`*_real_image`) are deselected: this sweep tests the logic, not the image.

Outcomes: KILLED (a test failed), SURVIVED (all passed, so a test is missing), INVALID
(the mutant did not import), ANCHOR-DRIFT. Exit status is nonzero unless every mutation
was KILLED.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Mutation:
    identifier: str
    file: str
    old: str
    new: str
    why: str


MUTATIONS = [
    Mutation(
        "instrument-entry-label",
        "instrument.py",
        "label == pending_entry",
        "label != pending_entry",
        "the hook lands after the wrong label and logs the wrong moment",
    ),
    Mutation(
        "instrument-entry-not-requested",
        "instrument.py",
        "pending_entry = function if function in entries else None",
        "pending_entry = None",
        "no entry hook is added and every caller attribution is missing",
    ),
    Mutation(
        "instrument-argument-count",
        "instrument.py",
        "ENTRY_ARGUMENTS = 5",
        "ENTRY_ARGUMENTS = 4",
        "the key argument of the batcher falls off the record",
    ),
    Mutation(
        "instrument-argument-offset",
        "instrument.py",
        "MEM32(esp + {4 * (i + 1)})",
        "MEM32(esp + {4 * i})",
        "each argument is read one slot early",
    ),
    Mutation(
        "instrument-store-key-test",
        "instrument.py",
        "int(store.group(2), 16) == key_global",
        "int(store.group(2), 16) != key_global",
        "every OTHER global is logged and the key is not",
    ),
    Mutation(
        "instrument-store-fields-swapped",
        "instrument.py",
        "report.stores.append((function, label))",
        "report.stores.append((label, function))",
        "the writer function is reported as its label",
    ),
    Mutation(
        "instrument-assignment-ignored",
        "instrument.py",
        "assigned = re.match(",
        "assigned = None and re.match(",
        "a store in a form the rewrite does not log would pass unreported",
    ),
    Mutation(
        "instrument-address-taken-ignored",
        "instrument.py",
        'head.endswith(("++", "--", "&"))',
        'head.endswith(("++", "--"))',
        "a pointer taken to the key could be stored through unseen",
    ),
    Mutation(
        "instrument-include-before-anchor",
        "instrument.py",
        "ANCHOR_INCLUDE + STDIO_INCLUDE, 1)",
        "STDIO_INCLUDE + ANCHOR_INCLUDE, 1)",
        "stdio.h before the chunk's own header changes include order",
    ),
    Mutation(
        "instrument-destination-check",
        "instrument.py",
        "if destination.exists():",
        "if destination.exists() and False:",
        "an old copy would be trusted as current",
    ),
    Mutation(
        "instrument-missing-entry-unchecked",
        "instrument.py",
        "missing = sorted(entries - total.entries)",
        "missing = []",
        "an entry hook that was never added would read as 'never called'",
    ),
    Mutation(
        "instrument-no-store-unchecked",
        "instrument.py",
        "if not total.stores:",
        "if False:",
        "a lift with no key store would produce an empty log that means nothing",
    ),
    Mutation(
        "instrument-duplicate-unchecked",
        "instrument.py",
        "if duplicated:",
        "if False:",
        "a function defined in two chunks would log twice",
    ),
    Mutation(
        "instrument-write-skipped",
        "instrument.py",
        "        if report.entries or report.stores:\n            chunk.write_text(new_text)",
        "        if False:\n            chunk.write_text(new_text)",
        "the copy would be an unmodified lift",
    ),
    Mutation(
        "log-unreadable-word-accepted",
        "log.py",
        'if value.startswith("unreadable="):',
        "if False:",
        "an unreadable key word would parse as a key",
    ),
    Mutation(
        "log-fnv-prime",
        "log.py",
        "1099511628211",
        "1099511628213",
        "no logged hash would match a builder",
    ),
    Mutation(
        "log-unreadable-hash-parsed-as-good",
        "log.py",
        'digest.removeprefix("unreadable:")',
        "digest",
        "an unreadable buffer would parse as a real hash",
    ),
    Mutation(
        "profile-draw-address-dropped",
        "profile.py",
        "(0x3D4FB0, 0x3D5050)",
        "(0x3D4FB0,)",
        "one draw entry would go unprobed",
    ),
    Mutation(
        "log-stop-address-unparsed",
        "log.py",
        'last.thread, last.reason, int(address.group("address"), 16)',
        "last.thread, last.reason, None",
        "what ended the boot would be reported without its address",
    ),
    Mutation(
        "analyze-stops-dropped",
        "analyze.py",
        "report.stops = [(s.thread, s.reason, s.address, s.detail) for s in capture.stops]",
        "report.stops = []",
        "the report would claim it does not know what ended the boot",
    ),
    Mutation(
        "profile-second-batcher-argument",
        "profile.py",
        "((0x18EA0, 3), (0x18FF0, 2))",
        "((0x18EA0, 3), (0x18FF0, 3))",
        "the second batcher's key would be read from the wrong argument and disagree with its literals",
    ),
    Mutation(
        "profile-assembler-length-argument",
        "profile.py",
        "assembler_length: int = 2",
        "assembler_length: int = 3",
        "the hashed length would be the flags argument",
    ),
    Mutation(
        "analyze-call-length",
        "analyze.py",
        "CALL_LENGTH = 5",
        "CALL_LENGTH = 6",
        "every call site would be attributed one byte off and none would match",
    ),
    Mutation(
        "analyze-literal-base-test",
        "analyze.py",
        "sorted(k for k in observed if k not in census.base_keys)",
        "sorted(k for k in observed if k not in census.variant_keys)",
        "a key formed only by an OR or AND variation would read as a literal",
    ),
    Mutation(
        "analyze-draw-keys",
        "analyze.py",
        "if probe.address in profile.xdk_draws:",
        "if probe.address in ():",
        "the key at each draw would not be recorded",
    ),
    Mutation(
        "analyze-literal-agreement",
        "analyze.py",
        "if argument == site.value:",
        "if argument != site.value:",
        "agreement and disagreement would swap",
    ),
    Mutation(
        "analyze-length-unchecked",
        "analyze.py",
        "            and emitted[1] == probe.args[profile.assembler_length]\n",
        "            and True\n",
        "a builder emitting a different length would still reconcile",
    ),
    Mutation(
        "analyze-flags-unchecked",
        "analyze.py",
        "            and emitted[2] == probe.args[profile.assembler_flags]\n",
        "            and True\n",
        "different assembler flags would still reconcile",
    ),
    Mutation(
        "analyze-hash-unchecked",
        "analyze.py",
        "emitted[0] == probe.digest",
        "True",
        "any source would reconcile with any key",
    ),
    Mutation(
        "analyze-mask-not-applied",
        "analyze.py",
        "emission(owner, key & owner.mask)",
        "emission(owner, key)",
        "the builder would be run on an unmasked key the title never passes it",
    ),
    Mutation(
        "analyze-named-class-swapped",
        "analyze.py",
        '"static, named by a loader site"\n                    if pointer in census.named_programs\n                    else "static, NEVER NAMED"',
        '"static, NEVER NAMED"\n                    if pointer in census.named_programs\n                    else "static, named by a loader site"',
        "the dead-program question would be answered backwards",
    ),
    Mutation(
        "analyze-unnamed-loaded",
        "analyze.py",
        "[a for a in loaded if a not in census.named_programs]",
        "[a for a in loaded if a in census.named_programs]",
        "a hit on a never-named program would be hidden",
    ),
    Mutation(
        "analyze-nonliteral-count",
        "analyze.py",
        "report.nonliteral_sites_reached[batcher] = sum(",
        "report.nonliteral_sites_reached[batcher] = 0 * sum(",
        "a reached non-literal site would read as unreached",
    ),
    Mutation(
        "analyze-literal-only-for-imm",
        "analyze.py",
        'if site.kind == "imm":\n                argument',
        "if True:\n                argument",
        "a non-literal site would be compared with a literal it does not have",
    ),
    Mutation(
        "analyze-unknown-caller-hidden",
        "analyze.py",
        "report.batcher_sites_unknown.append(call.caller)",
        "pass",
        "a call from outside the static site list would vanish",
    ),
    Mutation(
        "cli-empty-kind-accepted",
        "cli.py",
        "if not (capture.probes and capture.enters and capture.stores):",
        "if False:",
        "an empty capture would be reported as a successful run",
    ),
    Mutation(
        "cli-hang-not-killed",
        "cli.py",
        "os.killpg(process.pid, signal.SIGKILL)",
        "pass",
        "a hung host would hold the run for its whole lifetime",
    ),
    Mutation(
        "cli-hdd-not-recreated",
        "cli.py",
        "if args.hdd_dir.exists():\n        shutil.rmtree(args.hdd_dir)",
        "if False:\n        shutil.rmtree(args.hdd_dir)",
        "a previous boot's files would change this boot",
    ),
    Mutation(
        "cli-native-assembler-flag",
        "cli.py",
        '    "--native-shader-assembler",\n',
        "",
        "without the retained assembler the boot stops at the first assembler call",
    ),
    Mutation(
        "cli-probe-spec-not-passed",
        "cli.py",
        "TSFP_XDK_PROBE=RETAIL.probe_spec()",
        "TSFP_XDK_PROBE=''",
        "the host would log nothing and the run would look like an empty boot",
    ),
]


def run_one(mutation: Mutation, timeout: float) -> str:
    source = (ROOT / "tools/shaderprobe" / mutation.file).read_text()
    if source.count(mutation.old) != 1:
        return f"ANCHOR-DRIFT ({source.count(mutation.old)} occurrences)"
    with tempfile.TemporaryDirectory(prefix="shaderprobe-mutant-") as scratch_name:
        scratch = Path(scratch_name)
        (scratch / "tools").mkdir()
        for entry in (ROOT / "tools").iterdir():
            if entry.name not in ("shaderprobe", "__pycache__"):
                (scratch / "tools" / entry.name).symlink_to(entry)
        shutil.copytree(
            ROOT / "tools/shaderprobe",
            scratch / "tools/shaderprobe",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        target = scratch / "tools/shaderprobe" / mutation.file
        target.write_text(source.replace(mutation.old, mutation.new))
        (scratch / "tests").mkdir()
        shutil.copy(ROOT / "tests/test_shaderprobe.py", scratch / "tests/test_shaderprobe.py")
        shutil.copy(ROOT / "pyproject.toml", scratch / "pyproject.toml")
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        environment.pop("PYTHONPATH", None)
        command = [
            sys.executable,
            "-m",
            "pytest",
            "tests/test_shaderprobe.py",
            "-q",
            "-x",
            "-k",
            "not real_image",
            "-p",
            "no:cacheprovider",
        ]
        try:
            result = subprocess.run(
                command,
                cwd=scratch,
                env=environment,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return "KILLED (timeout)"
    tail = result.stdout[-400:]
    if result.returncode == 0:
        return "SURVIVED"
    if "ImportError" in result.stdout or "SyntaxError" in result.stdout or "no tests ran" in tail:
        return "INVALID " + tail.strip().splitlines()[-1]
    return "KILLED"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--only", help="run one mutation by id")
    parser.add_argument("--list", action="store_true", help="list ids and exit")
    parser.add_argument("--timeout", type=float, default=300.0, help="seconds per mutant")
    args = parser.parse_args(argv)
    chosen = [m for m in MUTATIONS if args.only in (None, m.identifier)]
    if not chosen:
        print(f"no mutation named {args.only!r}", file=sys.stderr)
        return 2
    if len({m.identifier for m in MUTATIONS}) != len(MUTATIONS):
        print("duplicate mutation ids", file=sys.stderr)
        return 2
    if args.list:
        for mutation in chosen:
            print(mutation.identifier)
        return 0
    bad = 0
    for mutation in chosen:
        outcome = run_one(mutation, args.timeout)
        print(f"{outcome:<14} {mutation.identifier}: {mutation.why}", flush=True)
        bad += not outcome.startswith("KILLED")
    print(f"{len(chosen) - bad} of {len(chosen)} mutations killed")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
