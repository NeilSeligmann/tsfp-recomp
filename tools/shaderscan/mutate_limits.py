# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutation sweep for the T202 completion and comparison logic.

    python -m tools.shaderscan.mutate_limits                 # all mutations
    python -m tools.shaderscan.mutate_limits --only compare-ignores-reference-outcome

Each mutation swaps one exact string in `assemble.py` or `verify.py` inside a scratch copy
and runs the synthetic tests (`tests/test_shaderscan_limits.py` without the real-XBE class,
plus the assembler class of `tests/test_shaderscan.py`). The `old` string must occur exactly
once or the run reports ANCHOR-DRIFT, because a drifted anchor changes nothing and would look
like a killed mutant. The repository is never modified. Exit status is nonzero unless every
mutation was KILLED.
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
TESTS = (
    "tests/conftest.py",
    "tests/test_shaderscan.py",
    "tests/test_shaderscan_limits.py",
    "tests/test_shaderscan_text_corpus.py",
    "tests/test_shaderscan_overrun.py",
)


@dataclass(frozen=True)
class Mutation:
    identifier: str
    file: str
    old: str
    new: str
    why: str


MUTATIONS = [
    Mutation(
        "run-ignores-the-return-sentinel",
        "assemble.py",
        "        if stopped_at != SENTINEL:\n            if bytes(",
        "        if False:\n            if bytes(",
        "a budget-exhausted run would be read as a return with whatever EAX holds",
    ),
    Mutation(
        "run-ignores-stack-cleanup",
        "assemble.py",
        "        if stopped_esp != expected_esp:",
        "        if False:",
        "a wrong RET imm16 would be accepted as a return",
    ),
    Mutation(
        "run-budget-reads-eax",
        "assemble.py",
        "            return Run(Outcome.BUDGET_EXHAUSTED, **where)",
        "            return Run(Outcome.RETURNED, Assembled(uc.reg_read(UC_X86_REG_EAX), b''), **where)",
        "the original T202 defect: an intermediate EAX exposed as an HRESULT",
    ),
    Mutation(
        "run-halt-undetected",
        "assemble.py",
        '== b"\\xf4":',
        '== b"\\xf5":',
        "hlt would be reported as an exhausted budget",
    ),
    Mutation(
        "run-kernel-stop-is-a-fault",
        "assemble.py",
        "            if KERNEL_THUNK_FLOOR <= stopped_at < KERNEL_THUNK_CEILING:",
        "            if False:",
        "the original's own reject path would be indistinguishable from a native fault",
    ),
    Mutation(
        "run-fault-loses-the-address",
        "assemble.py",
        "                seen.address,\n                seen.access,",
        "                None,\n                seen.access,",
        "a fault would no longer say what it touched",
    ),
    Mutation(
        "run-zero-budget-allowed",
        "assemble.py",
        "        if limit <= 0:",
        "        if limit < 0:",
        "Unicorn treats a count of zero as unlimited and would hang on a runaway",
    ),
    Mutation(
        "run-heap-not-cleared-after-unfinished",
        "assemble.py",
        "used = HEAP_SIZE if self._unclean else min(self._heap_used, HEAP_SIZE)",
        "used = min(self._heap_used, HEAP_SIZE)",
        "a run that wrecked the bump pointer would leak heap contents into the next",
    ),
    Mutation(
        "run-exception-chain-not-reset",
        "assemble.py",
        "        self._reset_tib(uc)\n        uc.mem_write(SOURCE_BASE, bytes(SOURCE_SIZE))",
        "        uc.mem_write(SOURCE_BASE, bytes(SOURCE_SIZE))",
        "frames left registered by an aborted run would start the next run",
    ),
    Mutation(
        "run-stack-not-cleared",
        "assemble.py",
        "        uc.mem_write(STACK_BASE, bytes(STACK_SIZE))\n        self._reset_tib(uc)",
        "        self._reset_tib(uc)",
        "stack garbage would leak between runs",
    ),
    Mutation(
        "run-registers-not-cleared",
        "assemble.py",
        "        for register in _CLEARED_REGISTERS:\n            uc.reg_write(register, 0)\n        uc.reg_write(UC_X86_REG_EFLAGS, 2)\n        uc.reg_write(UC_X86_REG_ESP, esp)\n        self._last_fault",
        "        for register in ():\n            uc.reg_write(register, 0)\n        uc.reg_write(UC_X86_REG_EFLAGS, 2)\n        uc.reg_write(UC_X86_REG_ESP, esp)\n        self._last_fault",
        "register state would leak between runs",
    ),
    Mutation(
        "run-source-pad-not-cleared",
        "assemble.py",
        "        uc.mem_write(SOURCE_BASE, bytes(SOURCE_SIZE))\n",
        "",
        "the zero pad after the source would hold an earlier run's writes",
    ),
    Mutation(
        "run-flush-at-the-start",
        "assemble.py",
        "            source_address = GUARD_BASE + GUARD_SIZE - len(source)",
        "            source_address = GUARD_BASE",
        "the guard placement would no longer fault on a read past the end",
    ),
    Mutation(
        "compare-ignores-reference-outcome",
        "verify.py",
        "    if reference.outcome != Outcome.RETURNED.value:\n        return Verdict(\n            Status.NOT_COMPARED,",
        "    if False:\n        return Verdict(\n            Status.NOT_COMPARED,",
        "native output would be compared with an original that never returned",
    ),
    Mutation(
        "compare-ignores-instability",
        "verify.py",
        "    if not stable:",
        "    if False:",
        "a result that depends on memory past the input would be treated as a value",
    ),
    Mutation(
        "compare-native-non-return-passes",
        "verify.py",
        "    if native.outcome != Outcome.RETURNED.value:\n        return Verdict(Status.MISMATCH",
        "    if False:\n        return Verdict(Status.MISMATCH",
        "a native fault where the original returned would not fail",
    ),
    Mutation(
        "compare-skips-hresult",
        "verify.py",
        "    if native.hresult != reference.hresult:",
        "    if False:",
        "a different HRESULT would match",
    ),
    Mutation(
        "compare-skips-length",
        "verify.py",
        "    if native.output_length != reference.output_length:",
        "    if False:",
        "a different output length would match",
    ),
    Mutation(
        "compare-skips-digest",
        "verify.py",
        "    if native.output_sha256 != reference.output_sha256:",
        "    if False:",
        "different output bytes would match",
    ),
    Mutation(
        "overread-ignores-the-guard-fault",
        "verify.py",
        '    if flush.fault is not None and flush.fault.region == "past_source_guard":\n        return True\n',
        "",
        "a read past the end that faults flush would not be reported",
    ),
    Mutation(
        "overread-ignores-differences",
        "verify.py",
        "    return _stable_key(padded) != _stable_key(flush)",
        "    return False",
        "a result that changes with the padding would not be reported",
    ),
    Mutation(
        "measure-flush-without-guard",
        "verify.py",
        "    flush = emulator.run(case.source, case.flags, flush_to_guard=True, **options)",
        "    flush = emulator.run(case.source, case.flags, **options)",
        "the second placement would be the first again and nothing could differ",
    ),
    Mutation(
        "compare-all-missing-native-passes",
        "verify.py",
        'report.verdicts[name] = Verdict(Status.MISMATCH, "no native observation")',
        'report.verdicts[name] = Verdict(Status.NOT_COMPARED, "no native observation")',
        "a native run that skipped a returned case would pass",
    ),
    Mutation(
        "compare-all-drops-instability",
        "verify.py",
        "            stable=measured.stable,\n",
        "",
        "an overreading original would be compared",
    ),
    Mutation(
        "report-clean-without-a-comparison",
        "verify.py",
        "            and self.count(Status.MATCH) > 0\n",
        "            and True\n",
        "a report in which nothing was compared would pass",
    ),
    Mutation(
        "heap-dependence-ignored-by-stable",
        "verify.py",
        "        return not self.overread and not self.heap_dependent and not self.heap_overrun",
        "        return not self.overread and not self.heap_overrun",
        "a result that changes with stale heap contents would be compared (T387)",
    ),
    Mutation(
        "heap-dependence-needs-every-fill",
        "verify.py",
        "    return any(\n        _stable_key(padded)",
        "    return all(\n        _stable_key(padded)",
        "one clean dirty run would hide a dependence another fill exposes",
    ),
    Mutation(
        "heap-dependence-never-measured",
        "verify.py",
        "        depends_on_heap(\n            padded,\n            dirty_emulators,\n            case,\n            budget=budget,\n            output_args=output_args,\n            dispatch_exceptions=dispatch_exceptions,\n        ),\n",
        "        False,\n",
        "the dirty-heap runs would be dropped from every measurement",
    ),
    Mutation(
        "heap-dependence-loses-its-reason",
        "verify.py",
        "            unstable_reason=measured.unstable_reason,\n",
        "            unstable_reason=OVERREAD_REASON,\n",
        "a heap dependent case would be reported as an overread",
    ),
    Mutation(
        "heap-dependence-lost-in-the-reference-file",
        "verify.py",
        '        bool(raw.get("heap_dependent", False)),',
        "        False,",
        "a saved reference would forget which cases depend on the heap",
    ),
    Mutation(
        "allocator-ignores-the-fill-byte",
        "assemble.py",
        "bytes([fill & 0xFF])",
        "bytes([0])",
        "every dirty run would be as clean as the zero heap",
    ),
    Mutation(
        "allocator-fills-zeroed-blocks",
        "assemble.py",
        'b"\\xf6\\x44\\x24\\x08\\x08\\x75"',
        'b"\\xf6\\x44\\x24\\x08\\x08\\x74"',
        "a block asked for zeroed would come back dirty, unlike the title heap",
    ),
    Mutation(
        "overrun-ignored-by-stable",
        "verify.py",
        "        return not self.overread and not self.heap_dependent and not self.heap_overrun",
        "        return not self.overread and not self.heap_dependent",
        "a case whose original writes past a heap block would be compared (T485)",
    ),
    Mutation(
        "overrun-never-measured",
        "verify.py",
        "        padded.overrun_bytes > 0,\n",
        "        False,\n",
        "every measurement would say the original stayed inside its heap blocks",
    ),
    Mutation(
        "reference-run-does-not-track-allocations",
        "verify.py",
        "padded = emulator.run(case.source, case.flags, track_allocations=True, **options)",
        "padded = emulator.run(case.source, case.flags, **options)",
        "the padded run would never look for a heap overrun",
    ),
    Mutation(
        "overrun-loses-its-reason",
        "verify.py",
        "        return HEAP_REASON if self.heap_dependent else OVERRUN_REASON",
        "        return HEAP_REASON",
        "an overrunning case would be reported as heap dependent",
    ),
    Mutation(
        "overrun-lost-in-the-reference-file",
        "verify.py",
        '        bool(raw.get("heap_overrun", False)),',
        "        False,",
        "a saved reference would forget which cases overrun a heap block",
    ),
    Mutation(
        "overrun-ignores-the-spare-bytes",
        "assemble.py",
        "            found += self._nonzero(pointer + request, block_end)",
        "            found += 0",
        "a write into the spare bytes of a block, where the title heap keeps a header, is missed",
    ),
    Mutation(
        "overrun-ignores-the-tail",
        "assemble.py",
        "        return found + self._nonzero(end, min(end + OVERRUN_TAIL, HEAP_BASE + HEAP_SIZE - 4))",
        "        return found",
        "a write past the last block is missed",
    ),
    Mutation(
        "overrun-reads-the-wrong-size-argument",
        "assemble.py",
        "hooked.mem_read(esp_now + 0xC, 4)",
        "hooked.mem_read(esp_now + 0x8, 4)",
        "the request size would be taken from the flags argument",
    ),
    Mutation(
        "overrun-records-the-wrong-block",
        "assemble.py",
        "self._read_u32(BUMP_SLOT),\n",
        "self._read_u32(BUMP_SLOT) + 16,\n",
        "every block would be taken to start one header later",
    ),
    Mutation(
        "overrun-claims-a-filled-heap",
        "assemble.py",
        "            self.heap_fill is not None\n            or self.heap_limit is not None",
        "            False\n            or self.heap_limit is not None",
        "the filled spare bytes of a dirty heap would read as an overrun",
    ),
    Mutation(
        "overrun-claims-a-limited-heap",
        "assemble.py",
        "            or self.heap_limit is not None\n            or self.heap_fail_at is not None",
        "            or self.heap_fail_at is not None",
        "a capped allocator that returned NULL could report an overrun it did not make",
    ),
    Mutation(
        "overrun-claims-a-failing-heap",
        "assemble.py",
        "            or self.heap_fail_at is not None\n        ):\n            return 0",
        "        ):\n            return 0",
        "an allocator that fails one request could report an overrun it did not make",
    ),
    Mutation(
        "overrun-never-reported",
        "assemble.py",
        "            overrun_bytes=overrun,\n",
        "            overrun_bytes=0,\n",
        "the run would never carry what it measured",
    ),
    Mutation(
        "heap-tail-left-dirty",
        "assemble.py",
        "bump - HEAP_BASE + OVERRUN_TAIL if bump >= HEAP_BASE else 0",
        "bump - HEAP_BASE if bump >= HEAP_BASE else 0",
        "bytes written past the last block would leak into the next run",
    ),
    Mutation(
        "observation-allows-an-unfinished-hresult",
        "verify.py",
        "        if self.outcome != Outcome.RETURNED.value and (",
        "        if False and (",
        "an Observation could carry an HRESULT for a run that never returned",
    ),
    Mutation(
        "extras-not-read-on-failure",
        "assemble.py",
        "        extras = tuple(\n            (index, self._read_buffer(EXTRA_SLOT_BASE + 4 * index)) for index in output_args\n        )",
        "        extras = tuple(\n            (index, self._read_buffer(EXTRA_SLOT_BASE + 4 * index)) for index in output_args\n        ) if status == 0 else ()",
        "the compilation-errors text a failing source produces would never be observed",
    ),
    Mutation(
        "extra-outputs-share-one-slot",
        "assemble.py",
        "            arguments[index] = EXTRA_SLOT_BASE + 4 * index",
        "            arguments[index] = EXTRA_SLOT_BASE",
        "the constants and errors buffers could not be told apart",
    ),
    Mutation(
        "extra-output-may-replace-a-fixed-argument",
        "assemble.py",
        "index in (1, 2, 3, 5):",
        "index in (1, 2, 3):",
        "an extra output could overwrite the compiled-shader output slot",
    ),
    Mutation(
        "extra-output-index-unchecked",
        "assemble.py",
        "            if not 0 <= index < len(arguments) or",
        "            if False or",
        "a negative or past-the-end argument index would silently edit the wrong stack word",
    ),
    Mutation(
        "null-extra-slot-reads-as-empty",
        "assemble.py",
        "        if buffer == 0:\n            return None",
        '        if buffer == 0:\n            return b""',
        "an output the assembler left NULL could not be told from an empty buffer",
    ),
    Mutation(
        "unreadable-extra-buffer-reads-as-null",
        "assemble.py",
        '        except UcError:\n            return b""',
        "        except UcError:\n            return None",
        "an output slot that is not an XGBuffer would read as not supplied",
    ),
    Mutation(
        "oversized-extra-buffer-read-as-bytes",
        "assemble.py",
        "            if pointer == 0 or length > SOURCE_SIZE:",
        "            if pointer == 0:",
        "a buffer claiming more than the scratch mapping holds would be read as data",
    ),
    Mutation(
        "case-budget-ignored",
        "verify.py",
        "    budget = budget if budget is not None else case.budget\n",
        "",
        "a long program that needs more than the default budget would be called exhausted",
    ),
    Mutation(
        "case-budget-beats-an-explicit-budget",
        "verify.py",
        "    budget = budget if budget is not None else case.budget\n",
        "    budget = case.budget if case.budget is not None else budget\n",
        "a command line budget would be silently overridden",
    ),
    Mutation(
        "static-init-not-run",
        "assemble.py",
        "        if spec.static_init is not None:",
        "        if False:",
        "the float table the title's start-up fills would stay empty and every def would fault",
    ),
    Mutation(
        "static-init-not-baked",
        "assemble.py",
        "        self._pristine = bytes(uc.mem_read(self._base, len(self._pristine)))\n",
        "",
        "the initialiser's writes would be undone by the first run's image rewrite",
    ),
    Mutation(
        "static-init-stack-unchecked",
        "assemble.py",
        " or uc.reg_read(UC_X86_REG_ESP) != esp + 4:",
        ":",
        "an initialiser with the wrong RET imm16 would be accepted",
    ),
    Mutation(
        "observe-drops-the-errors-output",
        "verify.py",
        "            *errors,\n",
        "            None,\n            None,\n",
        "the reference would not record the compilation-errors text",
    ),
    Mutation(
        "observe-reads-the-wrong-argument-for-constants",
        "verify.py",
        "extras.get(CONSTANTS_ARGUMENT)",
        "extras.get(ERRORS_ARGUMENT)",
        "constants and errors would be swapped in the reference",
    ),
    Mutation(
        "compare-ignores-the-extra-outputs",
        "verify.py",
        '    for label in ("constants", "errors"):',
        "    for label in ():",
        "a native run could return other error text or constants than the original and match",
    ),
    Mutation(
        "compare-checks-extra-length-only",
        "verify.py",
        '        elif getattr(native, f"{label}_sha256") != getattr(reference, f"{label}_sha256"):',
        "        elif False:",
        "same length but different error text or constants would match",
    ),
    Mutation(
        "stable-key-ignores-the-extra-outputs",
        "verify.py",
        "        observed.constants_sha256,\n        observed.constants_length,\n        observed.errors_sha256,\n        observed.errors_length,\n",
        "",
        "error text that changes with the heap fill or the placement would look stable",
    ),
    Mutation(
        "heap-runs-skip-the-extra-outputs",
        "verify.py",
        "                budget=budget,\n                output_args=output_args,\n                dispatch_exceptions=dispatch_exceptions,\n            )\n        )\n        for emulator in dirty_emulators",
        "                budget=budget,\n                dispatch_exceptions=dispatch_exceptions,\n            )\n        )\n        for emulator in dirty_emulators",
        "every case with an extra output would look heap dependent",
    ),
    Mutation(
        "reference-run-skips-the-extra-outputs",
        "verify.py",
        "    options = dict(budget=budget, output_args=output_args, dispatch_exceptions=dispatch_exceptions)",
        "    options = dict(budget=budget, dispatch_exceptions=dispatch_exceptions)",
        "the measurement would record no constants or errors",
    ),
    Mutation(
        "unfinished-observation-may-carry-extras",
        "verify.py",
        "            or self.constants_length is not None\n            or self.errors_length is not None\n",
        "",
        "an extra output could be attached to a run that never returned",
    ),
    Mutation(
        "null-buffer-digests-as-empty",
        "verify.py",
        "    if data is None:\n        return None, None",
        "    if data is None:\n        return None, 0",
        "NULL and empty output buffers would compare equal",
    ),
]


LIMITS_PYTEST = (
    "tests/test_shaderscan_limits.py",
    "tests/test_shaderscan.py",
    "tests/test_shaderscan_text_corpus.py",
    "tests/test_shaderscan_overrun.py",
    "-q",
    "-x",
    "-k",
    "not TestRealOriginal and (TestAssemblerEmulation or not test_shaderscan.py)",
    "-p",
    "no:cacheprovider",
)


def run_one(
    mutation: Mutation,
    timeout: float,
    tests: tuple[str, ...] = TESTS,
    pytest_args: tuple[str, ...] = LIMITS_PYTEST,
) -> str:
    source = (ROOT / "tools/shaderscan" / mutation.file).read_text()
    if source.count(mutation.old) != 1:
        return f"ANCHOR-DRIFT ({source.count(mutation.old)} occurrences)"
    with tempfile.TemporaryDirectory(prefix="shaderscan-mutant-") as scratch_name:
        scratch = Path(scratch_name)
        (scratch / "tools").mkdir()
        for entry in (ROOT / "tools").iterdir():
            if entry.name not in ("shaderscan", "__pycache__"):
                (scratch / "tools" / entry.name).symlink_to(entry)
        shutil.copytree(
            ROOT / "tools/shaderscan",
            scratch / "tools/shaderscan",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        (scratch / "tools/shaderscan" / mutation.file).write_text(
            source.replace(mutation.old, mutation.new)
        )
        (scratch / "tests").mkdir()
        for name in tests:
            shutil.copy(ROOT / name, scratch / name)
        shutil.copy(ROOT / "pyproject.toml", scratch / "pyproject.toml")
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        environment.pop("PYTHONPATH", None)
        command = [
            sys.executable,
            "-m",
            "pytest",
            *pytest_args,
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
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds per mutant")
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
