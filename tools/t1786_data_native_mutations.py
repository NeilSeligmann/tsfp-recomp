# SPDX-License-Identifier: GPL-3.0-or-later
"""Native negative controls for each frozen T1786 root; no harness tuple runs."""

from __future__ import annotations

import hashlib
import json
import resource
import subprocess
from pathlib import Path

MUTATIONS = {
    "000665C0": ("g_eax = g_ecx == 0", "g_eax = g_ecx != 0"),
    "00246D10": ("g_eax = g_ecx == 0", "g_eax = g_ecx != 0"),
    "00162060": ("g_edi = guest_read32(g_esp)", "g_edi = 0x87654321"),
    "00378510": ("g_eax = g_edx == 0x76DB78", "g_eax = g_edx != 0x76DB78"),
    "000BDB80": ("if (!g_eax)", "if (g_eax)"),
    "000D6A50": ("if (!g_eax)", "if (g_eax)"),
    "00227A30": ("+ 0x48, 0", "+ 0x49, 0"),
    "003397F0": ("g_eax *= 0x58", "g_eax *= 0x54"),
    "00227A50": ("+ 0x3C, g_ecx", "+ 0x40, g_ecx"),
    "002DB4D0": ("guest_read32(g_esi)", "guest_read32(g_esi + 4)"),
    "00158D90": ("0x74C37C, 0", "0x74C37C, 1"),
    "000827D0": ("g_eax == 0xFFFFFFFFu", "g_eax != 0xFFFFFFFFu"),
}


def run(root: Path) -> list[dict]:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    frozen = root / "docs/data/t1786-data/all-12-drafts.c.txt"
    original = frozen.read_text()
    test = (root / "tests/c/test_game_data_campaign.c").read_text()
    dest = root / "tmp/t1786-native-mutations"
    dest.mkdir(parents=True, exist_ok=True)
    results = []
    for va, (before, after) in MUTATIONS.items():
        begin = original.index(f"GAME_REPLACE_EXACT({va},")
        end = original.find("GAME_REPLACE_EXACT(", begin + 1)
        if end < 0:
            end = len(original)
        body = original[begin:end]
        assert body.count(before) == 1, va
        mutated = original[:begin] + body.replace(before, after) + original[end:]
        draft = dest / f"mutant-{va}.c"
        draft.write_text(mutated)
        case = dest / f"test-{va}.c"
        case.write_text(test.replace("../../docs/data/t1786-data/all-12-drafts.c.txt", str(draft)))
        for compiler in ("gcc", "clang"):
            for opt in (0, 3):
                binary = dest / f"mutant-{va}-{compiler}-o{opt}"
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-O{opt}"]
                if compiler == "gcc":
                    command.append("-malign-data=abi")
                command += ["-Isrc/game", str(case), "-o", str(binary)]
                subprocess.run(command, cwd=root, check=True)
                result = subprocess.run([str(binary)], cwd=root, capture_output=True, timeout=30)
                assert result.returncode != 0, (va, compiler, opt)
                results.append(
                    {
                        "va": f"0x{va}",
                        "compiler": compiler,
                        "opt": opt,
                        "mutation": [before, after],
                        "exit": result.returncode,
                        "source_sha256": hashlib.sha256(mutated.encode()).hexdigest(),
                        "stderr": result.stderr.decode(errors="replace").strip(),
                    }
                )
    print(f"T1786 native negatives: {len(results)} compiled mutants killed")
    return results


if __name__ == "__main__":
    root = Path.cwd()
    results = run(root)
    (root / "tmp/t1786-native-mutations/results.json").write_text(
        json.dumps(results, indent=2) + "\n"
    )
