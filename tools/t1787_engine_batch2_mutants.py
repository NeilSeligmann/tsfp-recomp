# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import pathlib
import subprocess

p = pathlib.Path("docs/data/t1787-engine/batch2")
d = (p / "frozen-draft.c.txt").read_text()
mutations = [
    ("bit7", "(~(g_eax>>7))&1", "(~(g_eax>>6))&1"),
    ("shiftmask", "g_ecx&31", "g_ecx&30"),
    ("indexmask", "g_eax=guest_read32(g_edx+0xA0)&4;", "g_eax=guest_read32(g_edx+0xA0)&8;"),
    ("animoffset", "g_ecx+0xD4", "g_ecx+0xD8"),
    ("state0", "g_ecx=(g_edx==0)", "g_ecx=(g_edx==1)"),
    ("field3c", "g_edx+0x3C", "g_edx+0x40"),
    ("copyoffset", "g_eax+0x24,g_edx", "g_eax+0x2C,g_edx"),
    ("mode", "g_eax=0x1B26", "g_eax=0x1B27"),
    ("category", "g_eax==0x40000", "g_eax==0x40001"),
    ("state2", "g_eax==1||g_eax==2", "g_eax==1||g_eax==3"),
    (
        "flagmask",
        "++g_eax;g_eax=guest_read32(g_edx+0xA0)&4",
        "++g_eax;g_eax=guest_read32(g_edx+0xA0)&8",
    ),
    ("signed", "((int32_t)g_edx<(int32_t)g_ecx)", "(g_edx<g_ecx)"),
]
rows = []
for name, old, new in mutations:
    assert old in d
    f = pathlib.Path("tmp/batch2-mutant.c.txt")
    f.write_text(d.replace(old, new, 1))
    runs = []
    for cc in ["gcc", "clang"]:
        for opt in [0, 3]:
            cmd = [
                cc,
                "-O" + str(opt),
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Isrc/game",
                '-DT1787_BATCH2_DRAFT="' + str(f.resolve()) + '"',
                "tests/c/test_t1787_engine_batch2.c",
                "-o",
                "tmp/batch2-mutant",
            ]
            subprocess.run(cmd, check=True, capture_output=True)
            r = subprocess.run(["tmp/batch2-mutant"], capture_output=True, text=True)
            assert r.returncode != 0, name
            runs.append(
                {
                    "compiler": cc,
                    "opt": opt,
                    "exit": r.returncode,
                    "stdout": r.stdout,
                    "stderr": r.stderr,
                }
            )
    rows.append(
        {
            "name": name,
            "old": old,
            "new": new,
            "draft_sha256": hashlib.sha256(f.read_bytes()).hexdigest(),
            "runs": runs,
        }
    )
p.joinpath("native-mutants.json").write_text(json.dumps(rows, indent=2) + "\n")
print("48 native mutant executions KILLED")
