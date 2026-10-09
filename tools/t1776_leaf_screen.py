# SPDX-License-Identifier: GPL-3.0-or-later
"""Rerun T1772 static labels, draft screen and EXACT caller audit on repaired candidates.

Run from the task checkout with generated/ and build/ private artifacts available.
This deliberately includes registered roots to compare the original 28/23 what-if set.
"""

import json
from pathlib import Path

from tools import llm_replacement_draft as draft
from tools import name_additions as na
from tools import t1772_replacement_census as c
from tools.replace.manifest import ManifestEntry
from tools.vector_whitelist import admissible_scalar_fp_instruction

w = na.World(Path.cwd())
md = draft._capstone()
functions = c.load_merged_functions(w, md)
c.pilot.SIZE_ALLOWED = 1 << 30
rows = json.load(open("docs/data/t1776-extent-repair.json"))["rows"]
leaf = []
facts = {}
labels = {}
for r in rows:
    v = int(r["va"], 16)
    f = functions[v]
    info = c.static_labels(f, md, (w.text_lo, w.text_hi))
    labels[v] = info
    if (
        c.first_static_blocker(info["labels"])
        or info["calls"]
        or any(x.startswith("input:") for x in info["labels"])
    ):
        continue
    if info["vector"] not in ("", "vector-state", "fp-scalar-v1"):
        continue
    leaf.append(v)
    original = draft.admissible_vector_instruction
    if info["vector"] == "fp-scalar-v1":
        draft.admissible_vector_instruction = admissible_scalar_fp_instruction
    got = draft.screen_function(f, md, c.pilot.MIN_INSNS, 10000)
    draft.admissible_vector_instruction = original
    if got:
        facts[v] = got
entries = [
    ManifestEntry(
        va=v,
        name="hypothetical",
        convention="stdcall" if f["ret_pop"] else "cdecl",
        stack_args=f["stack_args_hint"],
        returns="eax",
        scratch=(),
        source="hypothetical.c",
    )
    for v, f in facts.items()
]
audit = c.caller_audit_rows(entries, Path("generated/lifted/gen"), Path("build/default.xbe"))
result = {
    "static_clean_leaves": [f"0x{v:08X}" for v in leaf],
    "screen_rejected": len(leaf) - len(facts),
    "eligible": [f"0x{v:08X}" for v in facts if audit[v]["ok"]],
    "audit": {f"0x{v:08X}": x for v, x in audit.items()},
}
Path("docs/data/t1776-leaf-screen.json").write_text(json.dumps(result, indent=2) + "\n")
print("leaf", len(leaf), "eligible", len(result["eligible"]))
