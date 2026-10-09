# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501  (mutation patterns are literal source text and cannot be wrapped)
"""Mutation test of the T561 output register start value, killed by the unit tests.

`tools/nv2a/mutation.py` judges a mutant by the numeric validation, which only runs the
compute form against the interpreter. The option also lives in the vertex form, the
wrappers' defaults, the CLIs and the `ModelInterpreter` / `validate_many` plumbing, none of
which the validation executes. Each mutant here is applied to a SCRATCH COPY of `tools/` and
`tests/` (never the checkout, so no stale bytecode and no concurrent edit is disturbed) and
the T561 tests run in that copy. The mutant is KILLED when they fail, a SURVIVOR is a gap.

`tools/mutate/sets` cannot hold these: its harness guards every mutant with a rebuilt ctest
binary, and pure Python changes no binary (it would report STALE-BINARY).

Every anchor must occur exactly once (`tests/test_nv2a_mutation.py` checks it without running).
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

TRANSLATE = "tools/nv2a/translate.py"
INTERP = "tools/nv2a/interp.py"
VALIDATE = "tools/nv2a/validate.py"
DEVICE = "tools/nv2a/vertex_device.py"

#: pytest `-k` expression selecting the T561 tests of the four test modules.
SELECTION = (
    "OutputInit or output_init or unwritten_w or other_start or share_the_output or nv_output "
    "or output_file or start_value or read_back or output_registers_start or cli_gives "
    "or cli_passes"
)
TEST_FILES = (
    "tests/test_nv2a_translate.py",
    "tests/test_nv2a_interp.py",
    "tests/test_nv2a_validate.py",
    "tests/test_nv2a_vertex_device.py",
)
TIMEOUT_SECONDS = 900


@dataclass(frozen=True)
class Mutant:
    label: str
    file: str
    old: str
    new: str


_ALWAYS_PASS = '{"output_init": self._output_init}'
_OPTIONAL = '{} if self._output_init == "zero" else {"output_init": self._output_init}'

MUTANTS: tuple[Mutant, ...] = (
    Mutant(
        "vertex form ignores the option",
        TRANSLATE,
        "init_text = output_init_text(output_init)",
        'init_text = "vec4(0.0)"',
    ),
    Mutant(
        "vertex form always starts at nv",
        TRANSLATE,
        'lines.append(f"        o[i] = {init_text};")',
        'lines.append("        o[i] = vec4(0.0, 0.0, 0.0, 1.0);")',
    ),
    Mutant(
        "the default flips to nv",
        TRANSLATE,
        'DEFAULT_OUTPUT_INIT = "zero"',
        'DEFAULT_OUTPUT_INIT = "nv"',
    ),
    Mutant(
        "compute default flips only",
        TRANSLATE,
        "def compute_test_source(t: Translation, *, output_init: str = DEFAULT_OUTPUT_INIT)",
        'def compute_test_source(t: Translation, *, output_init: str = "nv")',
    ),
    Mutant(
        "vertex default flips only",
        TRANSLATE,
        "    window_to_clip: bool = False,\n    output_init: str = DEFAULT_OUTPUT_INIT,\n) -> str:",
        '    window_to_clip: bool = False,\n    output_init: str = "nv",\n) -> str:',
    ),
    Mutant(
        "translate cli drops the option for vertex",
        TRANSLATE,
        "            window_to_clip=args.window_to_clip,\n            output_init=args.output_init,\n",
        "            window_to_clip=args.window_to_clip,\n",
    ),
    Mutant(
        "translate cli drops the option for compute",
        TRANSLATE,
        "else compute_test_source(translation, output_init=args.output_init)",
        "else compute_test_source(translation)",
    ),
    Mutant(
        "translator accepts an unknown option",
        TRANSLATE,
        "    if name not in OUTPUT_INITS:\n        raise ValueError",
        "    if False:\n        raise ValueError",
    ),
    Mutant(
        "interpreter accepts an unknown option",
        INTERP,
        "    if output_init not in OUTPUT_INITS:\n        raise InterpError",
        "    if False:\n        raise InterpError",
    ),
    Mutant("model interpreter drops the option", VALIDATE, f"extra = {_OPTIONAL}", "extra = {}"),
    Mutant(
        "model interpreter always passes the option",
        VALIDATE,
        f"extra = {_OPTIONAL}",
        f"extra = {_ALWAYS_PASS}",
    ),
    Mutant(
        "validate_many drops the option",
        VALIDATE,
        'source_options = {} if output_init == "zero" else {"output_init": output_init}',
        "source_options = {}",
    ),
    Mutant(
        "validate cli drops the interpreter option",
        VALIDATE,
        '_load("tools.nv2a.interp"), args.model, args.output_init',
        '_load("tools.nv2a.interp"), args.model',
    ),
    Mutant(
        "validate cli drops the translator option",
        VALIDATE,
        "            max_records=args.examples,\n            output_init=args.output_init,",
        "            max_records=args.examples,",
    ),
    Mutant(
        "device capture drops the option",
        DEVICE,
        "        float_controls=float_controls,\n        output_init=output_init,\n    )\n    wanted",
        "        float_controls=float_controls,\n    )\n    wanted",
    ),
    Mutant(
        "device compute form drops the option",
        DEVICE,
        "translate.compute_test_source(translation, output_init=output_init)",
        "translate.compute_test_source(translation)",
    ),
    Mutant(
        "device interpreter drops the option",
        DEVICE,
        "validate.ModelInterpreter(interp, model, output_init)",
        "validate.ModelInterpreter(interp, model)",
    ),
    Mutant(
        "device cli drops the option",
        DEVICE,
        "                    output_init=args.output_init,\n",
        "",
    ),
)


def anchor_counts(repo: Path = Path()) -> dict[str, int]:
    """How many times each mutant's anchor occurs in the checkout (must all be 1)."""
    return {m.label: (repo / m.file).read_text().count(m.old) for m in MUTANTS}


def run_mutant(mutant: Mutant, repo: Path) -> str:
    """`killed`, `SURVIVED`, `anchor-drift` or `error` for one mutant, run in a scratch copy."""
    with tempfile.TemporaryDirectory(prefix="nv2a-output-init-") as scratch:
        root = Path(scratch)
        for name in ("tools", "tests"):
            shutil.copytree(repo / name, root / name, ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copy2(repo / "pyproject.toml", root / "pyproject.toml")
        (root / "generated").symlink_to((repo / "generated").resolve())
        target = root / mutant.file
        text = target.read_text()
        if text.count(mutant.old) != 1:
            return "anchor-drift"
        target.write_text(text.replace(mutant.old, mutant.new))
        command = [sys.executable, "-B", "-m", "pytest", "-q", "-x", "-p", "no:cacheprovider"]
        done = subprocess.run(
            [*command, *TEST_FILES, "-k", SELECTION],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    return {0: "SURVIVED", 1: "killed"}.get(done.returncode, "error")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(), help="checkout to mutate a copy of")
    parser.add_argument("--only", help="run only mutants whose label contains this text")
    args = parser.parse_args(argv)
    chosen = [m for m in MUTANTS if not args.only or args.only in m.label]
    verdicts = [(m.label, run_mutant(m, args.repo)) for m in chosen]
    for label, verdict in verdicts:
        print(f"{verdict:12} {label}")
    survived = [label for label, verdict in verdicts if verdict != "killed"]
    print(f"{len(verdicts) - len(survived)} of {len(verdicts)} killed, {len(survived)} not killed")
    return 1 if survived else 0


if __name__ == "__main__":
    raise SystemExit(main())
