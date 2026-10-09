# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutation sweep for the T446 exception dispatch.

    python -m tools.shaderscan.mutate_seh                 # all mutations
    python -m tools.shaderscan.mutate_seh --only unwind-leaves-frames-linked

Same method as `mutate_limits`: each mutation swaps one exact string in `seh.py` or
`assemble.py` inside a scratch copy and runs the synthetic tests of
`tests/test_shaderscan_seh.py` (the real-XBE class skips there). An anchor that does not occur
exactly once is reported as ANCHOR-DRIFT. The repository is never modified. Exit status is
nonzero unless every mutation was KILLED.
"""

from __future__ import annotations

import argparse
import sys

from tools.shaderscan.mutate_limits import Mutation, run_one

TESTS = ("tests/conftest.py", "tests/test_shaderscan.py", "tests/test_shaderscan_seh.py")
PYTEST = ("tests/test_shaderscan_seh.py", "-q", "-x", "-p", "no:cacheprovider")

MUTATIONS = [
    Mutation(
        "continue-execution-ignores-non-continuable",
        "seh.py",
        "            if flags & EXCEPTION_NONCONTINUABLE:",
        "            if False:",
        "a handler could resume a record the raiser marked non-continuable",
    ),
    Mutation(
        "search-does-not-advance",
        "seh.py",
        "            walk.frame = walk.following\n            return self._raise_step(walk)",
        "            return self._raise_step(walk)",
        "ContinueSearch would call the same frame again",
    ),
    Mutation(
        "unwind-leaves-frames-linked",
        "seh.py",
        "            self._put(self.tib, walk.frame)",
        "            pass",
        "unwound frames would stay on the chain, so the catch frame is not the head",
    ),
    Mutation(
        "unwind-does-not-flag-the-record",
        "seh.py",
        "        flags = self._u32(record + 4) | EXCEPTION_UNWINDING",
        "        flags = self._u32(record + 4)",
        "handlers could not tell the unwind pass from the search pass",
    ),
    Mutation(
        "exit-unwind-is-not-flagged",
        "seh.py",
        "            flags |= EXCEPTION_EXIT_UNWIND",
        "            pass",
        "a null target would not be an exit unwind",
    ),
    Mutation(
        "unwind-drops-the-return-value",
        "seh.py",
        '        registers["eax"] = retval',
        '        registers["eax"] = 0',
        "the catch continuation would not see RtlUnwind's ReturnValue in eax",
    ),
    Mutation(
        "unwind-ignores-the-target-ip",
        "seh.py",
        "            eip_after=target_ip or caller,",
        "            eip_after=caller,",
        "execution would resume after the call and not at the unwind target",
    ),
    Mutation(
        "unwind-pops-the-wrong-arguments",
        "seh.py",
        "            esp_after=esp + 20,",
        "            esp_after=esp + 16,",
        "RtlUnwind takes four arguments, the resume stack would be off by a dword",
    ),
    Mutation(
        "raise-pops-the-wrong-arguments",
        "seh.py",
        "            esp_after=esp + 8,",
        "            esp_after=esp + 4,",
        "RtlRaiseException takes one argument, the resume stack would be off by a dword",
    ),
    Mutation(
        "unwind-keeps-the-handlers-registers",
        "seh.py",
        "            self._walks.pop()\n            self._restore(walk.registers)\n            self.uc.reg_write(UC_X86_REG_ESP, walk.esp_after)\n            return walk.eip_after\n        if walk.frame == END_OF_CHAIN:",
        "            self._walks.pop()\n            self.uc.reg_write(UC_X86_REG_ESP, walk.esp_after)\n            return walk.eip_after\n        if walk.frame == END_OF_CHAIN:",
        "the guest would resume with whatever the unwinding handlers left in the registers",
    ),
    Mutation(
        "resume-keeps-the-handlers-registers",
        "seh.py",
        "            self._walks.pop()\n            self._restore(walk.registers)\n            self.uc.reg_write(UC_X86_REG_ESP, walk.esp_after)\n            return walk.eip_after\n        if eax == DISPOSITION_CONTINUE_SEARCH:",
        "            self._walks.pop()\n            self.uc.reg_write(UC_X86_REG_ESP, walk.esp_after)\n            return walk.eip_after\n        if eax == DISPOSITION_CONTINUE_SEARCH:",
        "a continued exception would resume with the handler's registers",
    ),
    Mutation(
        "context-eip-is-wrong",
        "seh.py",
        "            (0xB8, eip),",
        "            (0xB8, esp),",
        "handlers would read the wrong resume address from the CONTEXT",
    ),
    Mutation(
        "context-has-no-flags",
        "seh.py",
        "        self._put(base, CONTEXT_FLAGS)",
        "        self._put(base, 0)",
        "the CONTEXT would not say which parts are valid",
    ),
    Mutation(
        "chain-may-repeat-a-frame",
        "seh.py",
        "            and frame > walk.last_frame\n",
        "            and True\n",
        "a frame linking to itself would be called until the frame cap",
    ),
    Mutation(
        "no-frame-cap",
        "seh.py",
        '        if walk.visited >= MAX_FRAMES or not self._frame_ok(walk, walk.frame):\n            self._walks.pop()\n            return SehStop(f"corrupt registration chain at {walk.frame:#x}")\n        walk.visited += 1\n        walk.last_frame = walk.frame\n        walk.following = self._u32(walk.frame)\n        return self._call_handler(walk)\n\n    def _call_handler',
        '        if not self._frame_ok(walk, walk.frame):\n            self._walks.pop()\n            return SehStop(f"corrupt registration chain at {walk.frame:#x}")\n        walk.visited += 1\n        walk.last_frame = walk.frame\n        walk.following = self._u32(walk.frame)\n        return self._call_handler(walk)\n\n    def _call_handler',
        "a long chain would be walked without a limit",
    ),
    Mutation(
        "frames-outside-the-stack-are-followed",
        "seh.py",
        "            self.stack_low <= frame < self.stack_high - 8\n",
        "            True\n",
        "a registration pointer outside the stack would be read",
    ),
    Mutation(
        "handler-stack-is-not-checked",
        "seh.py",
        "        if walk.call_esp + 4 != esp:",
        "        if False:",
        "a handler that cleans the wrong number of bytes would be accepted",
    ),
    Mutation(
        "unwind-target-missing-is-ignored",
        "seh.py",
        '            return SehStop("RtlUnwind target frame is not on the registration chain")',
        "            return walk.eip_after",
        "an unwind to a frame that is not registered would resume as if it worked",
    ),
    Mutation(
        "raise-ordinal-ignored",
        "seh.py",
        "        if ordinal == ORDINAL_RAISE_EXCEPTION:\n            return self._raise()",
        "        if ordinal == ORDINAL_RAISE_EXCEPTION:\n            return None",
        "a raise would stay a kernel stop under dispatch",
    ),
    Mutation(
        "dispatch-runs-without-being-asked",
        "assemble.py",
        "                if not dispatch or not KERNEL_THUNK_FLOOR <= stopped_at < KERNEL_THUNK_CEILING:",
        "                if not KERNEL_THUNK_FLOOR <= stopped_at < KERNEL_THUNK_CEILING:",
        "the production comparison would silently start assuming dispatch",
    ),
    Mutation(
        "handler-return-is-not-continued",
        "assemble.py",
        "                if not dispatch or uc.reg_read(UC_X86_REG_EIP) != DISPATCH_RETURN:\n                    return None",
        "                return None",
        "a handler's return would end the run as an exhausted budget",
    ),
    Mutation(
        "other-kernel-calls-are-swallowed",
        "assemble.py",
        "                if step is None:\n                    raise",
        "                if step is None:\n                    step = SENTINEL",
        "a kernel call that is not an exception service would look like a return",
    ),
    Mutation(
        "events-are-not-attached-to-the-run",
        "assemble.py",
        "        return replace(outcome, seh=tuple(self._seh.events))",
        "        return outcome",
        "a run would not say which handlers ran",
    ),
    Mutation(
        "events-leak-between-runs",
        "assemble.py",
        "        self._seh.reset()\n",
        "",
        "a second run would carry the first run's dispatch steps and walks",
    ),
    Mutation(
        "measure-ignores-the-flush-placement",
        "catch_path.py",
        "    others = [observed(emulator.run(source, flags, flush_to_guard=True, **options))]",
        "    others = [observed(emulator.run(source, flags, **options))]",
        "a result that reads past the input would be reported stable",
    ),
    Mutation(
        "measure-ignores-the-dirty-heaps",
        "catch_path.py",
        "    others += [observed(machine.run(source, flags, **options)) for machine in dirty]",
        "    others += []",
        "a result that reads uninitialised heap would be reported stable",
    ),
    Mutation(
        "measure-never-dispatches",
        "catch_path.py",
        '    options = {"dispatch_exceptions": True, "budget": budget, "output_args": output_args}',
        '    options = {"dispatch_exceptions": False, "budget": budget, "output_args": output_args}',
        "the tool would measure the no dispatch reference and call it the catch path",
    ),
    Mutation(
        "comparison-key-ignores-the-output-buffers",
        "catch_path.py",
        "            self.extras,\n",
        "",
        "a changed constants or errors output would not count as unstable",
    ),
    Mutation(
        "unhandled-is-reported-as-a-return",
        "assemble.py",
        "                Outcome.UNHANDLED,\n                seh_stop=stop.reason,",
        "                Outcome.RETURNED,\n                seh_stop=stop.reason,",
        "an exception nothing caught would look like the original returning",
    ),
]


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
        outcome = run_one(mutation, args.timeout, TESTS, PYTEST)
        print(f"{outcome:<14} {mutation.identifier}: {mutation.why}", flush=True)
        bad += not outcome.startswith("KILLED")
    print(f"{len(chosen) - bad} of {len(chosen)} mutations killed")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
