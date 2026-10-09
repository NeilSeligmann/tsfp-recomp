# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile semantic defects of the three T1789 admissions against native controls."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = Path("docs/data/t1789-mapmaker/all-drafts.c.txt").read_text()
    test = Path("tests/native/t1789_mapmaker.c").read_text()
    scratch = Path("/workspace/tmp/t1789-native-mutants")
    scratch.mkdir(parents=True, exist_ok=True)
    mutants = [
        ("336850-inverted", "g_eax=(g_ecx!=0)", "g_eax=(g_ecx==0)"),
        ("336850-wrong-global", "g_ecx=guest_read32(0x765C30)", "g_ecx=guest_read32(0x765C28)"),
        ("308690-short-clear", "g_ecx=0x2D", "g_ecx=0x2C"),
        ("308690-cached-pop", "g_edi=guest_read32(g_esp)", "g_edi=0x44332211"),
        ("2fe450-lower-bound", "(int32_t)g_eax>=0x35", "(int32_t)g_eax>=0x34"),
        ("2fe450-fallback", "g_edx==11", "g_edx==10"),
    ]
    receipts = []
    for name, old, new in mutants:
        assert old in source
        draft = scratch / f"{name}.h"
        draft.write_text(source.replace(old, new, 1))
        test_path = scratch / f"{name}.c"
        test_path.write_text(
            test.replace("../../docs/data/t1789-mapmaker/all-drafts.c.txt", str(draft))
        )
        for compiler in ("gcc", "clang"):
            exe = scratch / f"{name}-{compiler}"
            command = [
                compiler,
                "-O3",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(Path("src/game").resolve()),
                str(test_path),
                "-o",
                str(exe),
            ]
            if compiler == "gcc":
                command.insert(1, "-malign-data=abi")
            built = subprocess.run(command, capture_output=True, text=True)
            assert built.returncode == 0, built.stderr
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            assert run.returncode != 0, name + " survived"
            receipts.append(
                {
                    "mutant": name,
                    "compiler": compiler,
                    "build_exit": built.returncode,
                    "exit": run.returncode,
                    "stderr": run.stderr,
                    "command": command,
                    "killed": True,
                }
            )
    args.out.write_text(json.dumps(receipts, indent=2) + "\n")
    print("T1789 native mutations: 12 compiled defects killed")


if __name__ == "__main__":
    main()
