# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the compact T1710 HUD catalogue from original bytes (read only)."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from collections import Counter
from pathlib import Path

from capstone import CS_AC_READ, CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

from tools.frontend_labels import load
from tools.name_candidates import Image, load_known_names, read_functions


def element(name: str) -> str:
    """Ordered semantic tags, INFERRED from existing names, never subsystem moves."""
    if name.endswith(
        "per_frame_update_orchestrator_letterbox_noise_slot_health_ease_fade_panel_slide_popup_ring_banner_and_nav_entry_pick"
    ) or name.endswith("draw_overlay_with_letterbox_bars_and_player_panels"):
        return "frame orchestration"
    groups = [
        ("radar", ("radar", "blip", "array_7a5dc0")),
        ("health", ("health_fraction",)),
        ("ammo/inventory", ("ammo", "owned_weapon", "weapon_icon")),
        (
            "scoreboard/results",
            (
                "scoreboard",
                "stat_summary",
                "match_stat",
                "team_score",
                "roster",
                "rank_label",
                "award_label",
                "round_times",
            ),
        ),
        ("messages", ("message", "voice_line", "popup_ring", "player_text", "text_popup")),
        (
            "notices/prompts",
            ("notice", "prompt", "button_label", "button_mask", "button_icon", "button_hint"),
        ),
        ("letterbox", ("letterbox",)),
        ("banner", ("banner",)),
        (
            "timer/countdown",
            ("timer_fraction", "countdown", "mm_ss_timer", "elapsed_time", "time_text"),
        ),
        (
            "targeting/crosshair",
            (
                "crosshair",
                "reticle",
                "reticule",
                "lockon",
                "lock_on",
                "holoedge",
                "aim_texture",
                "target_name",
                "targeting",
                "objective",
                "directional",
                "nav_entry",
                "array_7a5da0",
                "scanlines",
                "homing",
                "robot_hud",
            ),
        ),
        ("text/font", ("text_", "font_", "outlined_text", "value_text")),
        (
            "overlays/effects",
            (
                "overlay",
                "noise",
                "wobble",
                "speckle",
                "fade_gray",
                "render_target",
                "post_process",
                "flash_rect",
                "transition_effect",
                "shimmer",
                "viewport_fade",
                "fill_viewport",
                "screen_effect",
            ),
        ),
        (
            "player/status/layout",
            (
                "player",
                "slot_",
                "layout_scales",
                "portrait",
                "amountbar",
                "effect_value",
                "detonation",
                "record_30",
                "state_7490ac",
                "per_frame_update",
            ),
        ),
    ]
    for tag, words in groups:
        if any(word in name for word in words):
            return tag
    return "shared panels/primitives"


def generate(root: Path, xbe: Path, iso: Path | None, out: Path) -> None:
    rows = [
        r
        for r in csv.DictReader((root / "tools/data/function_names.csv").open())
        if r["name"].startswith("game_hud_")
    ]
    funcs = {
        int(r["entry_va"], 16): r for r in read_functions(root / "generated/retail/functions.csv")
    }
    known = load_known_names(root)
    activity = json.loads((root / "docs/data/t1598-ui-hud-mapedit-lists/report.json").read_text())[
        "functions"
    ]
    image = Image(xbe, root / "generated/retail/functions.csv")
    labels = load(iso) if iso else None
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    result = []
    empty_bodies = 0
    for row in rows:
        va = int(row["entry_va"], 16)
        body = image.body(md, va, int(funcs[va]["size_bytes"])) if va in funcs else None
        reads, draws, calls, ids, dynamic = {}, {}, {}, {}, []
        insns = body.instructions if body else ()
        empty_bodies += not bool(insns)
        for index, ins in enumerate(insns):
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.access & CS_AC_READ and ins.mnemonic != "lea":
                    mem = op.mem
                    base, regindex = ins.reg_name(mem.base), ins.reg_name(mem.index)
                    # Stack locals and argument loads are not identified HUD state.
                    if base in ("esp", "ebp") or regindex in ("esp", "ebp"):
                        continue
                    expr = ins.op_str[ins.op_str.find("[") : ins.op_str.find("]") + 1]
                    # One exact instruction witness per distinct expression/width.
                    reads.setdefault((expr, op.size), ins.address)
            if (
                ins.mnemonic in ("call", "jmp")
                and ins.operands
                and ins.operands[0].type == X86_OP_IMM
            ):
                target = ins.operands[0].imm & 0xFFFFFFFF
                if target != va:
                    calls.setdefault(target, ins.address)
                    name = known.get(target, "")
                    if "draw" in name or "render" in name:
                        draws.setdefault(target, ins.address)
                if target == 0x3D7B0 and ins.mnemonic == "call":
                    # Nearest push in contiguous straight-line predecessors is arg0;
                    # stop on transfers or stack adjustment rather than guess dataflow.
                    label = None
                    cursor = ins.address
                    for prev in reversed(insns[max(0, index - 8) : index]):
                        if prev.address + prev.size != cursor:
                            break
                        cursor = prev.address
                        if prev.mnemonic == "push":
                            if prev.operands[0].type == X86_OP_IMM:
                                value = prev.operands[0].imm
                                if 0 <= value < 0x1DBC:
                                    label = value
                            break
                        if (
                            prev.mnemonic.startswith(("j", "ret", "call", "pop"))
                            or "esp" in prev.op_str
                        ):
                            break
                    if label is None:
                        dynamic.append(hex(ins.address))
                    else:
                        ids.setdefault(label, ins.address)
        act = activity.get(row["entry_va"], {})
        seen = bool(act.get("census") or act.get("sampled"))

        def edges(edges: dict[int, int]) -> str:
            return (
                "; ".join(
                    f"{t:#010x} {known.get(t, 'unnamed')} @{s:#x}" for t, s in sorted(edges.items())
                )
                or "none observed in decoded paths"
            )

        label_text = []
        for ident, site in sorted(ids.items()):
            value = labels.text(ident) if labels else "unresolved (ISO not supplied)"
            label_text.append(f"{ident:#x}={value!r} @{site:#x}")
        hints = []
        for match in re.finditer(
            r"(?:text_ids?|labels?)_((?:0x[0-9a-f]+)(?:(?:_and_|_or_|_to_|_)(?:0x[0-9a-f]+))*)",
            row["name"],
        ):
            hints.append(match.group(1))
        result.append(
            dict(
                entry_va=row["entry_va"],
                name=row["name"],
                element=element(row["name"]),
                element_confidence="INFERRED",
                state_reads="; ".join(
                    f"{expr}:{width}B @{site:#x}" for (expr, width), site in sorted(reads.items())
                )
                or "none observed in decoded paths; consult name evidence for unresolved body",
                draw_callees=edges(draws),
                direct_callees=edges(calls),
                label_ids="; ".join(label_text) or "none statically resolved",
                dynamic_label_calls="; ".join(dynamic) or "none observed",
                label_name_hints="; ".join(hints) or "none",
                execution="observed host census+sampler"
                if act.get("census") and act.get("sampled")
                else "observed host census"
                if act.get("census")
                else "observed host sampler"
                if seen
                else "not observed in pinned T1601 set",
                body_evidence=body.label if body else "entry absent from function table",
                naming_evidence=row["evidence"],
            )
        )
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(result[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(result)
    print(
        json.dumps(
            {
                "rows": len(result),
                "elements": dict(Counter(r["element"] for r in result)),
                "execution": dict(Counter(r["execution"] for r in result)),
                "labels": sum(bool(r["label_ids"] != "none statically resolved") for r in result),
                "empty_bodies": empty_bodies,
                "xbe_sha256": hashlib.sha256(image.data).hexdigest(),
            },
            sort_keys=True,
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    generate(args.root, args.xbe, args.iso, args.out)


if __name__ == "__main__":
    main()
