# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent semantic mutation controls against frozen T1787 native contract tests."""

import json
import subprocess
from pathlib import Path

MUTATIONS = [
    ("00063BF0", "g_eax=(g_eax==0)", "g_eax=(g_eax!=0)"),
    ("0010E790", "g_eax+0x13", "g_eax+0x14"),
    ("00124C70", "g_eax>>21", "g_eax>>20"),
    ("002BE670", "g_eax*=3", "g_eax*=4"),
    ("000942D0", "g_ecx=~g_ecx", "g_ecx^=1"),
    ("00119C20", "g_eax*=5", "g_eax*=6"),
    ("002744A0", "g_edx+0x3C", "g_edx+0x40"),
    ("00242EC0", "g_eax=(g_edx==1)", "g_eax=(g_edx==2)"),
    ("0010E620", "g_eax+0x88", "g_eax+0x8C"),
    ("002744F0", "++g_eax", "g_eax+=2"),
    ("001B63B0", "g_eax=(g_edx==g_ecx)", "g_eax=(g_edx!=g_ecx)"),
    ("0003EA20", "if(g_ecx==0)", "if(g_ecx!=0)"),
]


def main() -> None:
    out = Path("tmp/native-mutants")
    out.mkdir(parents=True, exist_ok=True)
    text = Path("docs/data/t1787-engine/proved-all-12-drafts.c.txt").read_text()
    results = []
    for va, old, new in MUTATIONS:
        assert text.count(old) == 1
        draft = (out / (va + ".c")).resolve()
        draft.write_text(text.replace(old, new))
        for compiler in ("gcc", "clang"):
            for opt in (0, 3):
                binary = out / f"{va}-{compiler}-O{opt}"
                cmd = [
                    compiler,
                    f"-O{opt}",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Isrc/game",
                    f'-DT1787_DRAFT="{draft}"',
                    "tests/c/test_t1787_engine_campaign.c",
                    "-o",
                    str(binary),
                ]
                if compiler == "gcc":
                    cmd.insert(1, "-malign-data=abi")
                subprocess.run(cmd, check=True, capture_output=True)
                run = subprocess.run([str(binary)], capture_output=True, text=True)
                assert run.returncode == 1 and "FAIL" in run.stderr, (va, compiler, opt, run)
                results.append(
                    {
                        "va": va,
                        "compiler": compiler,
                        "opt": opt,
                        "outcome": "KILLED",
                        "summary": run.stdout.strip(),
                        "old": old,
                        "new": new,
                    }
                )
    Path("docs/data/t1787-engine/native-mutants.json").write_text(
        json.dumps(results, indent=2) + "\n"
    )
    print(f"T1787: {len(results)} native mutant builds killed")


if __name__ == "__main__":
    main()
