# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce T1478 list-004 native integration and bounded compiled negatives."""

import argparse
import json
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutants", action="store_true")
    args = parser.parse_args()
    root = Path("tmp/t1478-native")
    root.mkdir(parents=True, exist_ok=True)
    rows = json.loads(Path("docs/data/t1478-census/native-results.json").read_text())
    results = []
    source_path = "src/game/game_census_180000_list004.c"
    source = Path(source_path).read_text()
    draft = Path("docs/data/t1478-census/list004-draft.c.txt").read_text()
    text_body = draft[
        draft.index("/* Copy the selected prefix") : draft.index("/* Validate each of four handles")
    ]
    stamp_body = draft[
        draft.index("/* Refresh a matching timestamp") : draft.index("/* Clear two pairs")
    ]
    expected = text_body.replace(
        "game_wrap_text_with_selected_affixes",
        "game_text_build_string_from_arg_with_language_prefix_and_suffix_tables_by_7497a8_bits_4_5",
    ) + stamp_body.replace(
        "game_record_key_timestamp_up_to_ten",
        "game_list_stride8_at_0xdc_add_or_stamp_entry_with_global_7de318_max_10",
    )
    assert source[source.index("/* Copy the selected prefix") :] == expected

    variants = [("control", source)]
    if args.mutants:
        variants.extend(
            [
                (
                    "truncated-suffix",
                    source.replace(
                        "const uint32_t length = g_ecx - suffix;", "const uint32_t length = 1u;"
                    ),
                ),
                (
                    "capacity-nine",
                    source.replace("if (count_bits == 10u)", "if (count_bits == 9u)"),
                ),
                (
                    "stale-count-index",
                    source.replace(
                        "guest_write32(g_eax + g_edx * 8u + 0xDCu, key);",
                        "guest_write32(g_eax + (uint32_t)count * 8u + 0xDCu, key);",
                    ),
                ),
            ]
        )
    for name, text in variants:
        candidate = root / f"{name}.c"
        candidate.write_text(text)
        for row in rows if name == "control" else rows[:1]:
            command = list(row["command"])
            command[command.index(source_path)] = str(candidate)
            opt = "0" if "-O0" in command else "3"
            binary = root / f"{name}-o{opt}"
            command[-1] = str(binary)
            subprocess.run(command, check=True, timeout=180)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=90)
            record = {
                "name": name,
                "opt": opt,
                "command": command,
                "exit": run.returncode,
                "output": run.stdout,
                "error": run.stderr,
            }
            results.append(record)
            print(name, opt, run.returncode, run.stdout.strip(), flush=True)
            assert (run.returncode == 0) == (name == "control"), record
    (root / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
