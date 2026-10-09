# SPDX-License-Identifier: GPL-3.0-or-later
"""T441: the disc-less steady state through the swap replay, refusals against closing slices.

Boots the six-flag, no-disc title (`docs/steady-state-profile.md`) cut by guest progress
(`--stop-after-calls 0x3D8E50:N`, N Swap dispatches) several times with different replay settings
and prints:

* viewport placement uses the opt-in T477 target-size inference because this recorded stream omits
  viewport registers; this does not establish NV2A viewport or clip-range behavior;

* the methods the decoder does not interpret in that loop (the frame profile's list, lenient
  decoder, combiner on) with pair counts, plus those only the swap replay reports (no combiner);
* per method the slice that closes it and whether that was MEASURED here: a method unhandled with
  the output-state groups off and absent with `--gpu-replay-output-state` is closed by that T267
  group, one that remains is open and says why (no group, no decoder, or a decoder the swap replay
  never enables);
* the strict refusals (the first method strict mode stops at, with and without the output state);
* the register combiner (T478): one more lenient boot with `--gpu-replay-combiner`, its column in
  the table (a method unhandled in the plain lenient replay and absent here is closed by the
  combiner option, measured only for the frames the combiner boot got through before its first
  refusal) and the frame profile's per-draw COMBINER CENSUS (which draws the combiner plans and
  which it refuses, with the reasons);
* the stand-in texture (T497): `--gpu-replay-standin-texture` on top of the combiner. The title's
  textures are not decoded, so the 27 draws that read t0 are refused; with ONE stand-in texture (NOT
  the title's, the tool says so everywhere) they plan, and the frame can be drawn and looked at. Two
  more boots (compared like the others), the frame profile's stand-in census (which draws plan, with
  which inferences), and the combiner modules of the definitions the loop sets that the title's own
  definitions did not give (made from the census's printed words, `replay_modules blocks`);
* the render target texture bridge (T510), two more boots on top of the combiner and the output
  state, NO stand-in: a CENSUS (`--gpu-replay-rt-texture-census`, census only, it replays no pixel
  and produces no picture) that classifies every draw's stage 0 texture, and a STRICT replay
  (`--gpu-replay-rt-texture`) that refuses by name at its first draw whose texture is not an
  earlier pass's image of the same frame. The refusal is a measurement, not a tool failure;
* the lenient replay's counters (presents, replayed, refused, draws) and a SHA-256 per dumped
  frame, compared across two boots. Equal digests mean the replay is deterministic on this host and
  device. They say nothing about whether the picture is what the console draws: there is no
  reference image.

Everything is read from the host's own output, nothing is guessed. Frames are written under
`--work-dir` (default a fresh directory under the system temp dir) and are never to be committed.
The vertex-program modules (`<name>.spv`) are generated from the user's corpus into the work
directory (`--corpus`, default `generated/shaders/corpus`) unless `--spv-dir` names a ready set.

Exit status: 0 done, 1 the two boots' frame digests differ, 2 an input is missing (host, xbe,
corpus, glslangValidator), 3 no Vulkan device could be opened.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SWAP_ADDRESS = 0x003D8E50
SIX_FLAGS = (
    "--ac97-ready", "--headless-effects", "--headless-streams", "--headless-buffers",
    "--headless-listener", "--headless-first-vblank",
)  # fmt: skip
PGRAPH_C = ROOT / "src/gpu/gpu_pgraph.c"
PGRAPH_H = ROOT / "src/gpu/gpu_pgraph.h"
RENDER_STATE_TABLE_C = ROOT / "src/gpu/d3d8_render_state_table.c"
DOCUMENTED_UNHANDLED = 62  # docs/steady-state-profile.md at T422, before T440's Clear words

# T482 measured the startup execution-mode packet in CreateDevice; retain a strict replay of the
# recorded word. The decoder's explicit old-stream inference remains available to callers.
REPLAY_LENIENT = ("--gpu-replay-lenient", "--gpu-replay-viewport-from-target")
OUTPUT_STATE = "--gpu-replay-output-state"
COMBINER = "--gpu-replay-combiner"
WINDOW_TO_CLIP = "--gpu-replay-window-to-clip"
STANDIN = "--gpu-replay-standin-texture"
STANDIN_TEXELS = "--gpu-replay-standin-texel-units"  # T713, INFERRED
STANDIN_TEXEL_PROGRAM = "--gpu-replay-standin-texel-program"  # T719, INFERRED, per draw
STANDIN_NORMALISED_PROGRAM = "--gpu-replay-standin-normalised-program"  # T719, INFERRED, per draw
DRAW_DUMP = "--gpu-replay-draw-dump"  # T724, observation only
PROGRAM_PREFIX = re.compile(r"[0-9a-f]{8,64}")
RT_TEXTURE = "--gpu-replay-rt-texture"
RT_TEXTURE_CENSUS = "--gpu-replay-rt-texture-census"
SURFACE_SOURCE = "--gpu-replay-surface-source"  # T633 (1) and T596, opt-in, measured in xemu
TARGET_PERSIST = "--gpu-replay-target-persist"  # T633 (2), opt-in, measured in xemu
# A loud colour no title would pick, so a picture made from it cannot be mistaken for the title's.
DEFAULT_STANDIN = "2x2:FF00FFFF"

# Words in the title's own state table that no T267 group decodes, and the point state (T125).
POLYGON_OFFSET = {0x0330, 0x0334, 0x0338, 0x0384, 0x0388, 0x09F8, 0x0310}
POINT_STATE = {0x043C, 0x0318, 0x031C}
# Fixed-function lighting controls. The title emits these while using vertex programs;
# the replay has no fixed-function lighting model and strict mode still refuses them.
FIXED_FUNCTION_LIGHTING = {0x0294, 0x02A4, 0x0314, 0x03B8, 0x03BC}
# CLEAR_SURFACE is an event the decoder handles apart from its output-word table.
CLEAR_SURFACE = 0x1D94


class Unavailable(Exception):
    """An input the run needs is missing (exit status 2)."""


class NoDevice(Exception):
    """The replay could not open a Vulkan device (exit status 3)."""


# --- pure parsing -------------------------------------------------------------


def parse_profile_unhandled(output: str) -> dict[int, int]:
    """Method -> pair count from the frame profile's `does not interpret` line."""
    match = re.search(r"methods the decoder does not interpret[^\n]*?: (\d+): ([^\n]*)", output)
    if match is None:
        return {}
    found = re.findall(r"([0-9A-F]{4}) x(\d+)", match[2])
    methods = {int(method, 16): int(count) for method, count in found}
    if len(methods) != int(match[1]):
        raise ValueError(f"the profile says {match[1]} methods and lists {len(methods)}")
    return methods


def parse_profile_frames(output: str) -> list[str]:
    """The frame profile's digest summary lines, verbatim, for the report."""
    keys = ("distinct over", "equal to the previous frame", "the last frame to show")
    return [line.strip() for line in output.splitlines() if line.startswith(keys)]


def parse_replay_unhandled(output: str) -> list[int]:
    """The distinct methods the swap replay logged as UNHANDLED, in first-seen order."""
    seen: list[int] = []
    for text in re.findall(r"UNHANDLED method 0x([0-9A-Fa-f]+)", output):
        method = int(text, 16)
        if method not in seen:
            seen.append(method)
    return seen


def parse_replay_unhandled_frames(output: str) -> dict[int, int]:
    """Method -> the first frame whose present logged it UNHANDLED."""
    first: dict[int, int] = {}
    pattern = r"frame (\d+): UNHANDLED method 0x([0-9A-Fa-f]+)"
    for frame, method in re.findall(pattern, output):
        first.setdefault(int(method, 16), int(frame))
    return first


@dataclass(frozen=True)
class CensusOutcome:
    planned: bool
    draws: int
    first_frame: int
    first_draw: int
    text: str


@dataclass(frozen=True)
class CombinerCensus:
    draws: int
    planned: int
    refused: int
    outcomes: list[CensusOutcome]


def parse_combiner_census(output: str) -> CombinerCensus | None:
    """The frame profile's per-draw combiner census (T478), None when the host printed none."""
    head = re.search(
        r"^combiner census \([^)]*\): (\d+) draw\(s\), (\d+) with a plan, (\d+) refused",
        output,
        re.M,
    )
    if head is None:
        return None
    outcomes = [
        CensusOutcome(kind == "plan", int(draws), int(frame), int(draw), text.strip())
        for kind, draws, frame, draw, text in re.findall(
            r"^  combiner (plan|REFUSED) x(\d+), first at frame (\d+) draw (\d+): ([^\n]*)$",
            output,
            flags=re.M,
        )
    ]
    return CombinerCensus(int(head[1]), int(head[2]), int(head[3]), outcomes)


def parse_standin_census(output: str) -> CombinerCensus | None:
    """The frame profile's census with a stand-in texture at stage 0 (T497), None if not printed."""
    head = re.search(
        r"^combiner census with a STAND-IN texture at stage 0 \([^)]*\): (\d+) draw\(s\), "
        r"(\d+) with a plan, (\d+) refused",
        output,
        re.M,
    )
    if head is None:
        return None
    outcomes = [
        CensusOutcome(kind == "plan", int(draws), int(frame), int(draw), text.strip())
        for kind, draws, frame, draw, text in re.findall(
            r"^  stand-in (plan|REFUSED) x(\d+), first at frame (\d+) draw (\d+): ([^\n]*)$",
            output,
            flags=re.M,
        )
    ]
    return CombinerCensus(int(head[1]), int(head[2]), int(head[3]), outcomes)


@dataclass(frozen=True)
class TextureCensusLine:
    """One outcome of the T510 render target texture census: the host's words and its draws."""

    text: str
    draws: int


@dataclass(frozen=True)
class TextureCensus:
    """The T510 census: draws classified in total and one line per distinct outcome."""

    frames: int
    classified: int
    lines: list[TextureCensusLine]


def parse_texture_census(output: str) -> TextureCensus | None:
    """The host's `RENDER TARGET TEXTURE CENSUS (T510)` summary and its `texture census (T510)`
    lines, None when the host printed no summary (a host that predates the flag)."""
    head = re.search(
        r"^gpu replay\s+RENDER TARGET TEXTURE CENSUS \(T510\):[^\n]*?(\d+) frame\(s\), "
        r"(\d+) draw\(s\) classified",
        output,
        flags=re.M,
    )
    if head is None:
        return None
    lines = [
        TextureCensusLine(text.strip(), int(draws))
        for text, draws in re.findall(
            r"^gpu replay\s+texture census \(T510\): ([^\n]*): (\d+) draw\(s\)$",
            output,
            flags=re.M,
        )
    ]
    return TextureCensus(int(head[1]), int(head[2]), lines)


def texture_census_category(text: str) -> str:
    """The short category of a census line: `no earlier image`, `no texture read`, `resolved`
    (stage 0 from a render target an earlier pass drew), `unsupported format`, `address mode` (a
    mode the sampler does not implement), `unreadable bytes` (the texture's guest memory cannot be
    read, T774), `kept image` or `guest surface` (T633, only with the surface source) or `other`."""
    for category, needle in (
        ("kept image", "from kept render target image"),  # T633 (1): an earlier frame's image
        ("guest surface", "from guest memory surface"),  # T633 (1): the guest memory under it
        (
            "address mode",
            "texture: address mode",
        ),  # an address word other than clamp (or DXT1 wrap)
        ("unreadable bytes", "texture: unreadable bytes"),  # the DXT1 texture's bytes (T774)
        ("no earlier image", "no earlier image"),
        ("no texture read", "no texture read by the combiner"),
        ("resolved", "from render target"),
        ("unsupported format", "unsupported format"),
    ):
        if needle in text:
            return category
    return "other"


def texture_census_by_category(census: TextureCensus) -> dict[str, int]:
    """Category -> draws, summed over the census lines (the order of first appearance)."""
    totals: dict[str, int] = {}
    for line in census.lines:
        category = texture_census_category(line.text)
        totals[category] = totals.get(category, 0) + line.draws
    return totals


def parse_texture_refusal(stopped: str) -> tuple[int, int, str] | None:
    """`(frame, draw, category text)` of a strict `--gpu-replay-rt-texture` refusal, from the
    STOPPED text (`frame F refused: replay (...) of draw D ... render target texture: <text>`)."""
    match = re.match(
        r"frame (\d+) refused: replay .*? of draw (\d+) .*?render target texture: (.*)$", stopped
    )
    return (int(match[1]), int(match[2]), match[3]) if match else None


COMBINER_WORDS = 57


def parse_standin_definitions(output: str) -> dict[str, list[int]]:
    """Module name -> the 57 combiner words of each planned outcome in the stand-in census."""
    found: dict[str, list[int]] = {}
    pattern = r"^  stand-in definition (combiner_[0-9a-f]{64})((?: [0-9A-F]{8})+)$"
    for name, words in re.findall(pattern, output, flags=re.M):
        values = [int(word, 16) for word in words.split()]
        if len(values) != COMBINER_WORDS:
            raise ValueError(f"{name} has {len(values)} words, a definition has {COMBINER_WORDS}")
        found[name] = values
    return found


def parse_standin_summary(output: str) -> tuple[int, int, str] | None:
    """`(draws that sampled the stand-in, draws replayed, the host's line)`, from the run's end."""
    match = re.search(
        r"^gpu replay\s+(STAND-IN TEXTURE \(T497\):[^\n]*"
        r"sampled by (\d+) of (\d+) replayed draw\(s\)[^\n]*)$",
        output,
        flags=re.M,
    )
    return (int(match[2]), int(match[3]), match[1]) if match else None


def definition_block(words: Sequence[int]) -> bytes:
    """The 240-byte definition `replay_modules blocks` reads: 57 state words and three zeros."""
    if len(words) != COMBINER_WORDS:
        raise ValueError(f"need {COMBINER_WORDS} words, got {len(words)}")
    return struct.pack("<60I", *words, 0, 0, 0)


def parse_stopped_frame(stopped: str) -> int | None:
    """The frame a latched replay stopped at, from its STOPPED text (`frame N refused: ...`)."""
    match = re.match(r"frame (\d+) refused", stopped)
    return int(match[1]) if match else None


@dataclass(frozen=True)
class OffsetCounters:
    """T511: the host's `polygon offset` line: draws applied, draws unobserved, pairs ignored."""

    applied: int
    unobserved: int
    ignored: int


def parse_offset_counters(output: str) -> OffsetCounters | None:
    """The host's own line after the replay counters, None for a host that predates it."""
    match = re.search(
        r"^gpu replay\s+polygon offset \(T511\): applied (\d+), unobserved (\d+), "
        r"IGNORED pairs (\d+)$",
        output,
        flags=re.M,
    )
    return OffsetCounters(int(match[1]), int(match[2]), int(match[3])) if match else None


@dataclass(frozen=True)
class ReplayStats:
    presents: int
    replayed: int
    empty: int
    refused: int
    skipped: int
    draws: int
    dumps: int
    stopped: str
    offset: OffsetCounters | None = None


def parse_replay_stats(output: str) -> ReplayStats | None:
    match = re.search(
        r"^gpu replay\s+presents (\d+), replayed (\d+), empty (\d+), refused (\d+), skipped (\d+), "
        r"draws (\d+), dumps (\d+)(?:, STOPPED: ([^\n]*))?$",
        output,
        flags=re.M,
    )
    if match is None:
        return None
    return ReplayStats(
        *(int(match[index]) for index in range(1, 8)), match[8] or "", parse_offset_counters(output)
    )


def parse_surface_summary(output: str) -> str | None:
    """The host's one `SURFACE MODEL (T633, T596)` summary line (the counters and the three
    choices, each INFERRED, the overlay colour key REFUSED by name), verbatim, or None for a host
    without it."""
    match = re.search(r"^gpu replay\s+(SURFACE MODEL \(T633, T596\)[^\n]*)$", output, flags=re.M)
    return match[1] if match else None


def run_surface_boots(
    rt_flags: Sequence[str], run: Callable[[str, Sequence[str]], Boot]
) -> SurfaceBoots:
    """The three `--surface-model` boots on top of `rt_flags`: census with the surface source,
    strict with it, strict with it and target persistence. `run(label, flags)` runs one boot."""
    census = run("surface-census", (*rt_flags, RT_TEXTURE_CENSUS, SURFACE_SOURCE))
    strict = run("surface-strict", (*rt_flags, RT_TEXTURE, SURFACE_SOURCE))
    persist = run("surface-persist", (*rt_flags, RT_TEXTURE, SURFACE_SOURCE, TARGET_PERSIST))
    return SurfaceBoots(
        parse_texture_census(census.output),
        parse_replay_stats(census.output),
        parse_replay_stats(strict.output),
        parse_replay_stats(persist.output),
        parse_surface_summary(persist.output),
    )


def parse_first_refusal(stopped: str) -> tuple[int, int] | None:
    """`(pair index, method)` of a strict decoder refusal, from the STOPPED text."""
    match = re.search(r"at pair (\d+): pair \d+ method 0x([0-9A-Fa-f]+)", stopped)
    return (int(match[1]), int(match[2], 16)) if match else None


def parse_output_words(source: str) -> dict[int, str]:
    """Method -> T267 group name from the decoder's single output-word table in gpu_pgraph.c."""
    pattern = r"\[GPU_PGRAPH_OUT_\w+(?: \+ \d+)?\] = \{0x([0-9A-Fa-f]+)u, GPU_PGRAPH_OUTPUT_(\w+)\}"
    return {int(method, 16): group for method, group in re.findall(pattern, source)}


def parse_clear_surface(header: str) -> int | None:
    match = re.search(r"#define GPU_PGRAPH_CLEAR_SURFACE 0x([0-9A-Fa-f]+)u", header)
    return int(match[1], 16) if match else None


def parse_render_state_names(source: str) -> dict[int, str]:
    """Method -> D3D8 render state name from the committed table (doubly derived names only)."""
    pattern = (
        r'\{ 0x([0-9A-Fa-f]+), 0, D3D8_RS_IMMEDIATE, ([0-9]), [0-9]+, [0-9]+, "([A-Z0-9_]+)" \}'
    )
    return {
        int(method, 16): name
        for method, sources, name in re.findall(pattern, source)
        if int(sources) >= 2
    }


def is_combiner_word(method: int) -> bool:
    """The methods `gpu_pgraph_combiner_index` maps (T75): the 57 combiner words."""
    ranges = ((0x0260, 0x027C), (0x0A60, 0x0ADC), (0x1E40, 0x1E60))
    singles = (0x0288, 0x028C, 0x17F8, 0x1E20, 0x1E24, 0x1E70, 0x1E74, 0x1E78)
    return method & 3 == 0 and (
        any(low <= method <= high for low, high in ranges) or method in singles
    )


def method_name(method: int, render_state: dict[int, str], nv2a: dict[int, str]) -> str:
    if method in nv2a:
        return nv2a[method]
    if method in render_state:
        return render_state[method]
    if 0x1B00 <= method <= 0x1BFF:
        stage, offset = divmod(method - 0x1B00, 0x40)
        known = nv2a.get(0x1B00 + offset)
        return f"TEXTURE[{stage}] {known or f'+0x{offset:02X}'}"
    return "(no committed name)"


# --- the slice that closes each method ----------------------------------------


@dataclass(frozen=True)
class Closure:
    slice: str
    state: str  # CLOSED (measured), OPEN, UNMEASURED
    note: str


def classify(
    method: int,
    output_words: dict[int, str],
    *,
    closed_by_output_state: bool | None,
    closed_by_combiner: bool | None = None,
) -> Closure:
    """The slice closing `method`. `closed_by_output_state` and `closed_by_combiner`: was it absent
    with that flag on (None: the run did not measure it)."""
    if method in output_words:
        group = output_words[method]
        name = f"T267 {group}" + (" (producer T440)" if group == "CLEAR" else "")
        if closed_by_output_state is True:
            return Closure(name, "CLOSED", "MEASURED: absent with --gpu-replay-output-state")
        if closed_by_output_state is False:
            return Closure(name, "OPEN", "MEASURED: still unhandled with the group on, a gap")
        return Closure(name, "CLOSED", "by the group table (this run did not measure it)")
    if is_combiner_word(method):
        if closed_by_combiner is True:
            return Closure(
                "T75 combiner",
                "CLOSED",
                "MEASURED: absent with --gpu-replay-combiner (T478). Decoded, not yet drawn: "
                "see the combiner census",
            )
        if closed_by_combiner is False:
            return Closure(
                "T75 combiner",
                "OPEN",
                "MEASURED: still unhandled with --gpu-replay-combiner, a gap",
            )
        return Closure(
            "T75 combiner",
            "OPEN",
            "decoded only with --gpu-replay-combiner (T478), this run did not measure it",
        )
    if method in POLYGON_OFFSET:
        return Closure(
            "T267 remainder", "OPEN", "in the title's own state table, no group (NOT DONE)"
        )
    if 0x1B00 <= method <= 0x1BFF:
        return Closure(
            "T84a1 / T75 textures", "UNMEASURED", "SetTexture state, no decoder or binding"
        )
    if method in POINT_STATE:
        return Closure(
            "T125 point state", "OPEN", "a measured emitter writes it, no decoder acts on it"
        )
    if method in FIXED_FUNCTION_LIGHTING:
        return Closure(
            "fixed-function lighting",
            "OPEN",
            "MEASURED emitter; ignored by vertex-program replay "
            "(INFERRED safe for this title stream)",
        )
    return Closure("none", "UNMEASURED", "no committed emitter, name or group")


@dataclass
class Row:
    method: int
    name: str
    pairs: int | None  # the profile's count, None when only the replay reported it
    in_profile: bool
    in_replay_off: bool
    in_replay_on: bool | None
    in_replay_combiner: bool | None
    closure: Closure


def build_rows(
    profile: dict[int, int],
    replay_off: list[int],
    replay_on: list[int] | None,
    output_words: dict[int, str],
    render_state: dict[int, str],
    nv2a: dict[int, str],
    combiner_unhandled: set[int] | None = None,
    combiner_measured: set[int] | None = None,
) -> list[Row]:
    """`combiner_unhandled`: methods the combiner boot logged. `combiner_measured`: the methods that
    boot could have logged (its replay stops at its first refusal, so a method first seen after that
    frame in the plain lenient boot is not measured there). Both None: no combiner boot."""
    rows = []
    for method in sorted(set(profile) | set(replay_off)):
        closed = None if replay_on is None else method not in replay_on
        in_combiner: bool | None = None
        if combiner_unhandled is not None and (
            combiner_measured is None or method in combiner_measured
        ):
            in_combiner = method in combiner_unhandled
        rows.append(
            Row(
                method=method,
                name=method_name(method, render_state, nv2a),
                pairs=profile.get(method),
                in_profile=method in profile,
                in_replay_off=method in replay_off,
                in_replay_on=None if replay_on is None else method in replay_on,
                in_replay_combiner=in_combiner,
                closure=classify(
                    method,
                    output_words,
                    closed_by_output_state=closed,
                    closed_by_combiner=None if in_combiner is None else not in_combiner,
                ),
            )
        )
    return rows


def render_table(rows: Sequence[Row]) -> str:
    """The table. Rows only the replay reports (the combiner words) are folded into one row each."""
    mark = {True: "unhandled", False: "handled", None: "-"}
    lines = [
        "| method | name | pairs | replay | +output state | +combiner | slice | state | note |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    folded: dict[tuple[str, str, str], list[Row]] = {}
    for row in rows:
        if row.in_profile:
            lines.append(
                f"| 0x{row.method:04X} | {row.name} | {row.pairs} | {mark[row.in_replay_off]} "
                f"| {mark[row.in_replay_on]} | {mark[row.in_replay_combiner]} "
                f"| {row.closure.slice} | {row.closure.state} | {row.closure.note} |"
            )
        else:
            key = (row.closure.slice, row.closure.state, row.closure.note)
            folded.setdefault(key, []).append(row)
    for (slice_name, state, note), group in folded.items():
        low, high = group[0].method, group[-1].method
        lines.append(
            f"| {len(group)} replay-only word(s) 0x{low:04X}..0x{high:04X} | not in the profile "
            f"(its decoder has the combiner on, the swap replay does not) | - | unhandled "
            f"| {mark[group[0].in_replay_on]} | {mark[group[0].in_replay_combiner]} | {slice_name} "
            f"| {state} | {note} |"
        )
    return "\n".join(lines)


def summarise(rows: Sequence[Row]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        key = f"{row.closure.slice}: {row.closure.state}"
        counts[key] = counts.get(key, 0) + 1
    return dict(sorted(counts.items()))


# --- frames -------------------------------------------------------------------


def digest_frames(directory: Path) -> dict[str, str]:
    """File name -> SHA-256 of every PNG the replay dumped (offscreen passes included)."""
    return {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(directory.glob("*.png"))
    }


def aggregate_digest(frames: dict[str, str]) -> str:
    text = "".join(f"{name} {digest}\n" for name, digest in sorted(frames.items()))
    return hashlib.sha256(text.encode()).hexdigest()


def compare_boots(first: dict[str, str], second: dict[str, str]) -> list[str]:
    """Differences between two boots' digests. Empty means equal. Nothing to compare is an error."""
    if not first or not second:
        return ["no frame was dumped, nothing was compared"]
    differences = [f"{name} only in one boot" for name in sorted(set(first) ^ set(second))]
    both = sorted(set(first) & set(second))
    differences += [f"{name} differs" for name in both if first[name] != second[name]]
    return differences


def _unfilter(kind: int, line: bytearray, previous: bytearray, channels: int) -> None:
    for index in range(len(line)):
        left = line[index - channels] if index >= channels else 0
        up = previous[index]
        corner = previous[index - channels] if index >= channels else 0
        if kind == 1:
            predictor = left
        elif kind == 2:
            predictor = up
        elif kind == 3:
            predictor = (left + up) >> 1
        elif kind == 4:
            estimate = left + up - corner
            distances = ((abs(estimate - left), left), (abs(estimate - up), up),
                         (abs(estimate - corner), corner))  # fmt: skip
            predictor = min(distances, key=lambda item: item[0])[1]
        else:
            predictor = 0
        line[index] = (line[index] + predictor) & 255


def decode_png(path: Path) -> tuple[int, int, int, list[bytes]]:
    """`(width, height, channels, rows)` of an 8 bit RGB or RGBA PNG, no dependency."""
    data = path.read_bytes()
    position, compressed = 8, b""
    width = height = colour_type = 0
    while position < len(data):
        (length,) = struct.unpack(">I", data[position : position + 4])
        kind = data[position + 4 : position + 8]
        body = data[position + 8 : position + 8 + length]
        if kind == b"IHDR":
            width, height, _depth, colour_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            compressed += body
        position += 12 + length
    channels = {2: 3, 6: 4}[colour_type]
    raw, stride = zlib.decompress(compressed), width * channels
    previous, rows = bytearray(stride), []
    for row in range(height):
        start = row * (stride + 1)
        line = bytearray(raw[start + 1 : start + 1 + stride])
        _unfilter(raw[start], line, previous, channels)
        previous = line
        rows.append(bytes(line))
    return width, height, channels, rows


def png_summary(path: Path) -> dict[str, object]:
    """Size and colour histogram of an 8 bit RGB or RGBA PNG: what the picture holds."""
    width, height, channels, rows = decode_png(path)
    histogram: dict[str, int] = {}
    for line in rows:
        for pixel in range(width):
            key = line[pixel * channels : (pixel + 1) * channels].hex()
            histogram[key] = histogram.get(key, 0) + 1
    top = sorted(histogram.items(), key=lambda item: -item[1])[:4]
    return {"width": width, "height": height, "distinct_colours": len(histogram), "top": top}


# --- running ------------------------------------------------------------------


@dataclass
class Boot:
    output: str
    returncode: int
    frames: dict[str, str] = field(default_factory=dict)
    directory: Path | None = None


def run_boot(
    host: Path,
    xbe: Path,
    swaps: int,
    work: Path,
    label: str,
    *,
    replay: Sequence[str] | None,
    spv: Path | None,
    device: str,
    timeout: int,
    thread_timeout: int = 0,
    window_to_clip: bool = False,
) -> Boot:
    """One cut boot. `replay` None is the frame profile run, else the swap replay, those flags.
    `window_to_clip` (T560) states the modules were made with `--window-to-clip`."""
    hdd = Path(tempfile.mkdtemp(prefix=f"{label}-hdd-", dir=work))
    command = [str(host), str(xbe), "--hdd", str(hdd), "--trace", "1", *SIX_FLAGS,
               "--native-shader-assembler", "--profile-calls",
               "--stop-after-calls", f"0x{SWAP_ADDRESS:X}:{swaps}"]  # fmt: skip
    if thread_timeout:
        command += ["--thread-timeout", str(thread_timeout)]
    dump = None
    if replay is not None:
        assert spv is not None
        dump = work / f"{label}-frames"
        dump.mkdir(parents=True, exist_ok=True)
        command += ["--gpu-replay", str(spv), "--gpu-replay-dump", str(dump), *replay]
        if window_to_clip:
            command.append(WINDOW_TO_CLIP)
    environment = {**os.environ, "VKRUN_DEVICE": device} if device else None
    try:
        done = subprocess.run(
            command, capture_output=True, text=True, errors="replace", timeout=timeout,
            env=environment, check=False,
        )  # fmt: skip
    finally:
        shutil.rmtree(hdd, ignore_errors=True)
    output = done.stdout + done.stderr
    refused = ("passive audio requires", "requires a complete retained original compiler")
    if any(text in output for text in refused):
        raise Unavailable(f"the host refused the boot: {output.strip().splitlines()[-1][:200]}")
    return Boot(output, done.returncode, digest_frames(dump) if dump else {}, dump)


def standin_label(args: argparse.Namespace) -> str:
    """The stand-in as the report names it, with every opt-in inference (T713) stated."""
    label = args.standin
    if args.standin_texel_units:
        label += " sampled with oT0 in TEXELS (T713, INFERRED)"
    if args.standin_texel_program or args.standin_normalised_program:
        label += (
            f" with the unit chosen per draw by vertex program"
            f" (T719, INFERRED, measured lists: texel "
            f"{','.join(args.standin_texel_program) or 'none'}, normalised "
            f"{','.join(args.standin_normalised_program) or 'none'})"
        )
    if getattr(args, "standin_derived", None):
        label += " (the lists were DERIVED from the measured oT0 range, T724, INFERRED)"
    if args.output_init != "zero":
        label += f", vertex outputs start at {args.output_init} (T713, INFERRED until HQ20)"
    return label


def check_unit_programs(args: argparse.Namespace) -> None:
    """T719: refuse a malformed, overlapping or per-boot-mixed program list before any boot (the
    host
    refuses them too).
    """
    listed = [*args.standin_texel_program, *args.standin_normalised_program]
    if not listed:
        return
    if args.standin_texel_units:
        raise SystemExit(
            "--standin-texel-program/--standin-normalised-program replace "
            "--standin-texel-units, give one"
        )
    for item in listed:
        if not PROGRAM_PREFIX.fullmatch(item):
            raise SystemExit(f"program digest prefix {item!r} is not 8..64 lowercase hex digits")
    for index, item in enumerate(listed):
        for other in listed[index + 1 :]:
            if item.startswith(other) or other.startswith(item):
                raise SystemExit(f"program digest prefixes {item} and {other} overlap")


def derive_unit_lists(args: argparse.Namespace, common: dict, spv: Path) -> None:
    """T724, INFERRED: boot once with the stand-in and a draw dump, MEASURE each sampling
    draw's oT0 range through the translated program (`tools.nv2a.standin_units`) and set the
    T719 lists from it. A program it cannot classify stays out of both lists, so the host
    refuses its draws by name."""
    from tools.nv2a import standin_units

    dump = common["work"] / "draws.jsonl"
    dump.unlink(missing_ok=True)
    flags = (*standin_flags(args.standin, False), DRAW_DUMP, str(dump))
    run_boot(label="standin-probe", replay=flags, spv=spv, **common)
    try:
        derivation = standin_units.derive(
            standin_units.read_draws(dump), Path(args.standin_corpus), args.output_init
        )
    except standin_units.UnitError as error:
        raise Unavailable(f"the stand-in unit derivation failed: {error}") from error
    args.standin_texel_program, args.standin_normalised_program = (
        list(item) for item in derivation.lists()
    )
    args.standin_derived = derivation.as_json()
    if not (args.standin_texel_program or args.standin_normalised_program):
        raise Unavailable(
            "no sampling program could be classified, refused: "
            f"{derivation.refused() or 'no sampling draw'}"
        )
    check_unit_programs(args)


def standin_flags(
    standin: str,
    texel_units: bool,
    texel_programs: tuple[str, ...] = (),
    normalised_programs: tuple[str, ...] = (),
) -> tuple[str, ...]:
    """Host flags of a stand-in boot. `texel_units` (T713, INFERRED, opt-in) adds the texel mode,
    the program lists
    (T719, INFERRED, opt-in) choose the unit per draw instead."""
    flags = (*REPLAY_LENIENT, COMBINER, STANDIN, standin)
    if texel_units:
        flags = (*flags, STANDIN_TEXELS)
    for item in texel_programs:
        flags = (*flags, STANDIN_TEXEL_PROGRAM, item)
    for item in normalised_programs:
        flags = (*flags, STANDIN_NORMALISED_PROGRAM, item)
    return flags


def prepare_modules(
    corpus: Path,
    out: Path,
    glslang: str,
    *,
    undo_viewport: bool = False,
    window_to_clip: bool = False,
    output_init: str = "zero",
) -> Path:
    """Generate the `.spv` set (static programs and reachable generated ones) into `out`.
    `window_to_clip` (T560, INFERRED) makes the window coordinate programs land in clip space."""
    from tools.nv2a import vsh_modules

    manifest_path = corpus / "manifest.json"
    if not manifest_path.exists():
        raise Unavailable(f"no corpus at {corpus} (python -m tools.nv2a.corpus <default.xbe>)")
    if shutil.which(glslang) is None:
        raise Unavailable(f"{glslang} not found (sudo apt-get install -y glslang-tools)")
    manifest = json.loads(manifest_path.read_text())
    scratch = out / "corpus"
    for kind in ("static", "generated"):
        (scratch / kind).mkdir(parents=True, exist_ok=True)
    for digest in manifest["static"]:
        shutil.copy(corpus / "static" / f"{digest}.bin", scratch / "static")
    reachable = sorted(manifest["generated_reachable"])
    for digest in reachable:
        shutil.copy(corpus / "generated" / f"{digest}.bin", scratch / "generated")
    # The replay selects by the program's digest, so any key will do for a reachable program.
    derived = {
        "static": manifest["static"],
        "key_mask": 0xFFFF,
        "generated_keys": {hex(index): digest for index, digest in enumerate(reachable)},
    }
    (scratch / "manifest.json").write_text(json.dumps(derived))
    options: dict[str, object] = {"window_to_clip": True} if window_to_clip else {}
    if output_init != "zero":
        options["output_init"] = output_init  # T713, INFERRED, the default call is unchanged
    vsh_modules.generate(
        scratch, out / "vsh", spirv=True, glslang=glslang, undo_viewport=undo_viewport, **options
    )
    return out / "vsh" / "spv"


def prepare_combiner_modules(xbe: Path, directory: Path, work: Path, glslang: str) -> int:
    """Generate the `combiner_<sha256>.spv` fragment modules of every definition the title can set
    (T478) and copy them into `directory`, which the vertex modules are read from too. Returns how
    many."""
    from tools.nv2a_combiner import replay_modules

    if shutil.which(glslang) is None:
        raise Unavailable(f"{glslang} not found (sudo apt-get install -y glslang-tools)")
    out = work / "combiner"
    code = replay_modules.main(["--out", str(out), "--glslang", glslang, "corpus", str(xbe)])
    modules = sorted((out / "spv").glob("combiner_*.spv"))
    if code != 0 or not modules:
        raise Unavailable(f"replay_modules wrote no combiner module from {xbe} (exit {code})")
    for module in modules:
        shutil.copy(module, directory / module.name)
    return len(modules)


def prepare_standin_modules(
    definitions: dict[str, list[int]], directory: Path, work: Path, glslang: str
) -> list[str]:
    """Make the combiner modules the loop's own definitions need that `directory` lacks (T497).

    The title's own definitions (`prepare_combiner_modules`) do not cover everything the loop sets:
    the planned outcomes of the stand-in census are the definitions it does. Each is turned into a
    module by `replay_modules blocks` and its name must equal the name the host planned (a word
    miscopied would name another module). Returns the names it added."""
    missing = {
        name: words
        for name, words in definitions.items()
        if not (directory / f"{name}.spv").exists()
    }
    if not missing:
        return []
    from tools.nv2a_combiner import replay_modules

    if shutil.which(glslang) is None:
        raise Unavailable(f"{glslang} not found (sudo apt-get install -y glslang-tools)")
    out = work / "combiner-live"
    blocks = work / "combiner-live.blocks"
    blocks.write_bytes(b"".join(definition_block(words) for words in missing.values()))
    code = replay_modules.main(["--out", str(out), "--glslang", glslang, "blocks", str(blocks)])
    made = {path.stem: path for path in (out / "spv").glob("combiner_*.spv")}
    if code != 0:
        raise Unavailable(f"replay_modules could not make the loop's own definitions (exit {code})")
    absent = sorted(set(missing) - set(made))
    if absent:
        raise Unavailable(
            f"the host planned {absent[0]} but replay_modules made another name from its words"
            " (a definition the translator refuses, or a copy error)"
        )
    for name in missing:
        shutil.copy(made[name], directory / f"{name}.spv")
    return sorted(missing)


def check_device(stats: ReplayStats | None, output: str) -> ReplayStats:
    if stats is None:
        raise Unavailable("the host printed no replay counters (did it start?): " + output[-300:])
    pattern = r"(no Vulkan|vkEnumerate|could not open a (Vulkan )?device|no physical device)"
    if re.search(pattern, output, re.I):
        raise NoDevice(stats.stopped or "no Vulkan device")
    return stats


@dataclass
class SurfaceBoots:
    """T633 and T596, the opt-in `--surface-model` boots: the census with the surface source
    (replays no pixel), a strict boot with it, and one with target persistence as well. INFERRED,
    announced."""

    census: TextureCensus | None
    stats_census: ReplayStats | None
    stats_strict: ReplayStats | None
    stats_persist: ReplayStats | None
    summary: str | None  # the host's `SURFACE MODEL` line of the persistence boot, verbatim


@dataclass
class Report:
    swaps: int
    profile: dict[int, int]
    profile_frames: list[str]
    strict_first: tuple[int, int] | None
    strict_output_first: tuple[int, int] | None
    replay_off: list[int]
    replay_on: list[int]
    stats_off: ReplayStats
    stats_on: ReplayStats
    stats_combiner: ReplayStats | None
    combiner_horizon: int | None
    census: CombinerCensus | None
    standin: str
    stats_standin: ReplayStats | None
    standin_summary: tuple[int, int, str] | None
    standin_census: CombinerCensus | None
    standin_modules: list[str]
    frames_standin: list[dict[str, str]]
    frames_off: list[dict[str, str]]
    frames_on: list[dict[str, str]]
    rows: list[Row]
    pictures: dict[str, dict[str, object]]
    device: str
    texture_census: TextureCensus | None = None
    stats_census: ReplayStats | None = None
    stats_rt: ReplayStats | None = None
    rt_ran: bool = False
    surface: SurfaceBoots | None = None  # T633, only with --surface-model


def nv2a_names() -> dict[int, str]:
    from tools.d3dscan.methods import NV2A_METHODS

    return dict(NV2A_METHODS)


def last_presented(boot: Boot) -> list[Path]:
    return sorted(boot.directory.glob("frame_??????.png"))[-1:] if boot.directory else []


def measure(args: argparse.Namespace) -> Report:
    host, xbe = Path(args.host), Path(args.xbe)
    for path in (host, xbe):
        if not path.exists():
            raise Unavailable(f"{path} does not exist")
    work = Path(args.work_dir) if args.work_dir else Path(tempfile.mkdtemp(prefix="steady-replay-"))
    work.mkdir(parents=True, exist_ok=True)
    spv = (
        Path(args.spv_dir)
        if args.spv_dir
        else prepare_modules(
            Path(args.corpus),
            work,
            args.glslang,
            window_to_clip=args.window_to_clip,
            output_init=args.output_init,
        )
    )
    if not args.spv_dir and not args.no_combiner:
        prepare_combiner_modules(xbe, spv, work, args.glslang)
    common = {"host": host, "xbe": xbe, "swaps": args.swaps, "work": work, "device": args.device,
              "timeout": args.timeout, "thread_timeout": args.thread_timeout,
              "window_to_clip": args.window_to_clip}  # fmt: skip
    profile_boot = run_boot(label="profile", replay=None, spv=None, **common)
    profile = parse_profile_unhandled(profile_boot.output)
    if not profile:
        raise Unavailable("the frame profile listed no unhandled method (retained shader chunk?)")
    strict = run_boot(label="strict", replay=(), spv=spv, **common)
    strict_output = run_boot(label="strict-output", replay=(OUTPUT_STATE,), spv=spv, **common)
    off = [run_boot(label=f"off{n}", replay=REPLAY_LENIENT, spv=spv, **common)
           for n in range(args.boots)]  # fmt: skip
    on = [run_boot(label=f"on{n}", replay=(*REPLAY_LENIENT, OUTPUT_STATE), spv=spv, **common)
          for n in range(args.boots)]  # fmt: skip
    combiner = None
    standin: list[Boot] = []
    standin_modules: list[str] = []
    if not args.no_combiner:
        combiner = run_boot(label="combiner", replay=(*REPLAY_LENIENT, COMBINER), spv=spv, **common)
        if "the combiner is on and there is no combiner_" in combiner.output:
            raise Unavailable(
                f"{spv} holds no combiner module (omit --spv-dir or pass --no-combiner)"
            )
        # T497: modules for the definitions the loop sets that the title's own lack, then the boots
        if not args.spv_dir:
            standin_modules = prepare_standin_modules(
                parse_standin_definitions(profile_boot.output), spv, work, args.glslang
            )
        if args.standin_derive_units:
            derive_unit_lists(args, common, spv)
        standin = [
            run_boot(label=f"standin{n}",
                     replay=standin_flags(args.standin, args.standin_texel_units,
                                          tuple(args.standin_texel_program),
                                          tuple(args.standin_normalised_program)),
                     spv=spv, **common)
            for n in range(args.boots)
        ]  # fmt: skip
    # T510: the render target texture census and strict boots, after every existing boot
    census_boot = rt_boot = None
    surface = None
    if not args.no_combiner:
        rt_flags = (*REPLAY_LENIENT, OUTPUT_STATE, COMBINER)
        census_boot = run_boot(
            label="rt-census", replay=(*rt_flags, RT_TEXTURE_CENSUS), spv=spv, **common
        )
        rt_boot = run_boot(label="rt-strict", replay=(*rt_flags, RT_TEXTURE), spv=spv, **common)
        if getattr(args, "surface_model", False):
            surface = run_surface_boots(
                rt_flags,
                lambda label, flags: run_boot(label=label, replay=flags, spv=spv, **common),
            )
    stats_off = check_device(parse_replay_stats(off[0].output), off[0].output)
    stats_on = check_device(parse_replay_stats(on[0].output), on[0].output)
    strict_stats = parse_replay_stats(strict.output)
    strict_output_stats = parse_replay_stats(strict_output.output)
    clear = parse_clear_surface(PGRAPH_H.read_text())
    output_words = parse_output_words(PGRAPH_C.read_text()) | ({clear: "CLEAR"} if clear else {})
    replay_off, replay_on = (
        parse_replay_unhandled(off[0].output),
        parse_replay_unhandled(on[0].output),
    )
    state_names = parse_render_state_names(RENDER_STATE_TABLE_C.read_text())
    stats_combiner = (
        check_device(parse_replay_stats(combiner.output), combiner.output) if combiner else None
    )
    stats_standin = (
        check_device(parse_replay_stats(standin[0].output), standin[0].output) if standin else None
    )
    combiner_unhandled = parse_replay_unhandled(combiner.output) if combiner else None
    # The combiner replay latches at its first refusal and decodes nothing after it, so it can only
    # speak for the methods the plain lenient boot first logged at or before that frame.
    horizon = parse_stopped_frame(stats_combiner.stopped) if stats_combiner else None
    first_seen = parse_replay_unhandled_frames(off[0].output)
    measured = (
        {method for method, frame in first_seen.items() if horizon is None or frame <= horizon}
        if combiner
        else None
    )
    rows = build_rows(
        profile, replay_off, replay_on, output_words, state_names, nv2a_names(),
        combiner_unhandled, measured,
    )  # fmt: skip
    pictures = {f"{tag}:{path.name}": png_summary(path)
                for tag, boot in (("lenient", off[0]), ("lenient+output", on[0]),
                                  ("lenient+combiner", combiner),
                                  ("lenient+combiner+standin(NOT the title's texture)",
                                   standin[0] if standin else None))
                if boot is not None
                for path in last_presented(boot)}  # fmt: skip
    return Report(
        swaps=args.swaps,
        profile=profile,
        profile_frames=parse_profile_frames(profile_boot.output),
        strict_first=parse_first_refusal(strict_stats.stopped) if strict_stats else None,
        strict_output_first=(
            parse_first_refusal(strict_output_stats.stopped) if strict_output_stats else None
        ),  # fmt: skip
        replay_off=replay_off,
        replay_on=replay_on,
        stats_off=stats_off,
        stats_on=stats_on,
        stats_combiner=stats_combiner,
        combiner_horizon=horizon,
        census=parse_combiner_census(profile_boot.output),
        standin=standin_label(args),
        stats_standin=stats_standin,
        standin_summary=parse_standin_summary(standin[0].output) if standin else None,
        standin_census=parse_standin_census(profile_boot.output),
        standin_modules=standin_modules,
        frames_standin=[boot.frames for boot in standin],
        frames_off=[boot.frames for boot in off],
        frames_on=[boot.frames for boot in on],
        rows=rows,
        pictures=pictures,
        device=args.device or "(first device)",
        texture_census=parse_texture_census(census_boot.output) if census_boot else None,
        stats_census=parse_replay_stats(census_boot.output) if census_boot else None,
        stats_rt=parse_replay_stats(rt_boot.output) if rt_boot else None,
        rt_ran=census_boot is not None,
        surface=surface,
    )


def describe_boots(label: str, boots: list[dict[str, str]]) -> str:
    first, last = boots[0], boots[-1]
    differences = compare_boots(first, last) if len(boots) > 1 else ["only one boot"]
    verdict = (
        f"IDENTICAL across {len(boots)} boots"
        if not differences
        else "NOT IDENTICAL: " + "; ".join(differences)
    )
    return (
        f"Frame digests ({label}): {len(first)} PNG(s), aggregate "
        f"{aggregate_digest(first)[:16]}, {verdict}"
    )


def render_offset_counters(rows: Sequence[tuple[str, ReplayStats | None]]) -> list[str]:
    """T511: the polygon offset and IGNORED counters of each boot, one table row per boot."""
    lines = [
        "Polygon offset counters (replayed draws that ran with a Vulkan depth bias, draws with an "
        "enabled non-zero offset and no depth test, IGNORED-group pairs):",
        "",
        "| boot | offset_applied | offset_unobserved | pairs_ignored |",
        "|---|---|---|---|",
    ]
    for label, stats in rows:
        if stats is None:
            continue
        counters = stats.offset
        cells = (
            f"{counters.applied} | {counters.unobserved} | {counters.ignored}"
            if counters
            else "not reported | not reported | not reported"
        )
        lines.append(f"| {label} | {cells} |")
    return lines


def render_texture_census(census: TextureCensus | None) -> list[str]:
    """The census table. CENSUS ONLY: nothing was replayed, no picture comes from it."""
    if census is None:
        return ["Render target texture census (T510): not reported (does the host have the flag?)"]
    lines = [
        f"Render target texture census (T510, --gpu-replay-rt-texture-census): {census.classified} "
        f"draw(s) classified over {census.frames} frame(s). CENSUS ONLY: it replays no pixel and "
        "never latches, so the picture of the replay is not produced by it.",
        "",
        "| category | draws | census line |",
        "|---|---|---|",
    ]
    lines += [
        f"| {texture_census_category(line.text)} | {line.draws} | {line.text} |"
        for line in census.lines
    ]
    totals = texture_census_by_category(census)
    lines.append("")
    lines.append("Per category: " + "; ".join(f"{name} x{draws}" for name, draws in totals.items()))
    return lines


def render_texture_strict(stats: ReplayStats | None) -> list[str]:
    """The strict `--gpu-replay-rt-texture` boot: counters and the host's refusal verbatim."""
    if stats is None:
        return ["Render target texture strict replay (T510): not reported (does the host have it?)"]
    out = [
        "Render target texture strict replay (T510, --gpu-replay-rt-texture): presents "
        f"{stats.presents}, replayed {stats.replayed}, refused {stats.refused}, draws {stats.draws}"
    ]
    refusal = parse_texture_refusal(stats.stopped) if stats.stopped else None
    if refusal:
        frame, draw, reason = refusal
        out.append(
            f"It refuses by name at frame {frame} draw {draw}: {reason}. The refusal is a "
            "measurement of what this bridge does not yet draw, not a tool failure."
        )
    if stats.stopped:
        out.append(f"First refusal, verbatim: STOPPED: {stats.stopped}")
    else:
        out.append("It did not refuse a frame.")
    return out


def render_surface_model(surface: SurfaceBoots | None) -> list[str]:
    """T633 and T596, the opt-in surface model boots: what the three choices did to the first
    refusal. INFERRED, announced by the host, never a claim about the title's frames."""
    if surface is None:
        return ["Surface model (T633, T596): not reported (does the host have the flags?)"]
    out = [
        "Surface model (T633, T596, --gpu-replay-surface-source and --gpu-replay-target-persist, "
        "opt-in, xemu-level MEASURED rules of T736, the guest read timing INFERRED): the kept "
        "image of an earlier frame, else the guest memory under the surface, stands for a surface "
        "no pass of the frame drew; a target pass starts from the image an earlier frame left "
        "(the surface source implies it)."
    ]
    if surface.census is not None:
        out.append(
            f"Census with the surface source: {surface.census.classified} draw(s) classified over "
            f"{surface.census.frames} frame(s). CENSUS ONLY: it replays no pixel."
        )
        out.append(
            "Per category: "
            + "; ".join(
                f"{name} x{draws}"
                for name, draws in texture_census_by_category(surface.census).items()
            )
        )
    for label, stats in (
        ("surface source", surface.stats_strict),
        ("surface source + target persistence", surface.stats_persist),
    ):
        if stats is None:
            out.append(f"Strict replay with the {label}: not reported")
            continue
        out.append(
            f"Strict replay with the {label}: presents {stats.presents}, "
            f"replayed {stats.replayed}, refused {stats.refused}, draws {stats.draws}"
        )
        out.append(
            f"First refusal, verbatim: STOPPED: {stats.stopped}"
            if stats.stopped
            else "It did not refuse a frame."
        )
    if surface.summary:
        out.append(f"Host summary: {surface.summary}")
    return out


def render_report(report: Report) -> str:
    out = [
        f"# T441 steady-state replay, cut at {report.swaps} Swap dispatches, {report.device}",
        "",
    ]
    out.append(
        f"Frame profile (lenient decoder, combiner on): {len(report.profile)} method(s) the decoder"
        " "
        f"does not interpret (T422 documented {DOCUMENTED_UNHANDLED}, before T440's five Clear "
        f"words). Swap replay (no combiner): {len(report.replay_off)} unhandled, with "
        f"{OUTPUT_STATE} {len(report.replay_on)}."
    )
    out += ["", render_table(report.rows), ""]
    out.append("Per slice: " + "; ".join(f"{k} x{n}" for k, n in summarise(report.rows).items()))
    out.append("")
    for label, first in (
        ("strict", report.strict_first),
        ("strict + output state", report.strict_output_first),
    ):
        where = f"pair {first[0]}, method 0x{first[1]:04X}" if first else "none, not refused"
        out.append(f"First strict refusal ({label}): {where}")
    for label, stats in (
        ("lenient", report.stats_off),
        ("lenient + output state", report.stats_on),
    ):
        tail = f", STOPPED: {stats.stopped}" if stats.stopped else ""
        out.append(
            f"Replay counters ({label}): presents {stats.presents}, replayed {stats.replayed}, "
            f"empty {stats.empty}, refused {stats.refused}, skipped {stats.skipped}, "
            f"draws {stats.draws}, dumps {stats.dumps}{tail}"
        )
    if report.stats_combiner is not None:
        stats = report.stats_combiner
        tail = f", STOPPED: {stats.stopped}" if stats.stopped else ""
        out.append(
            f"Replay counters (lenient + combiner): presents {stats.presents}, replayed "
            f"{stats.replayed}, empty {stats.empty}, refused {stats.refused}, skipped "
            f"{stats.skipped}, draws {stats.draws}, dumps {stats.dumps}{tail}"
        )
        if report.combiner_horizon is not None:
            out.append(
                f"The combiner boot stopped at frame {report.combiner_horizon}: its column speaks "
                f"only for methods the plain lenient boot first logged by then"
            )
    if report.census is not None:
        census = report.census
        out.append(
            f"Combiner census (every inference allowed, no test texture): {census.draws} draw(s), "
            f"{census.planned} with a plan, {census.refused} refused"
        )
        out += [
            f"  {'plan' if o.planned else 'REFUSED'} x{o.draws}, first at frame {o.first_frame} "
            f"draw {o.first_draw}: {o.text}"
            for o in census.outcomes
        ]
    if report.standin_census is not None:
        census = report.standin_census
        out.append(
            f"Stand-in census (a {report.standin} texture at stage 0, NOT the title's texture, "
            f"every inference allowed): {census.draws} draw(s), {census.planned} with a plan, "
            f"{census.refused} refused (the vertex program is not looked at here)"
        )
        out += [
            f"  {'plan' if o.planned else 'REFUSED'} x{o.draws}, first at frame {o.first_frame} "
            f"draw {o.first_draw}: {o.text}"
            for o in census.outcomes
        ]
    if report.standin_modules:
        out.append(
            f"Combiner modules made for the loop's own definitions (the title's did not have "
            f"them): {len(report.standin_modules)}: " + ", ".join(report.standin_modules)
        )
    if report.stats_standin is not None:
        stats = report.stats_standin
        tail = f", STOPPED: {stats.stopped}" if stats.stopped else ""
        out.append(
            f"Replay counters (lenient + combiner + stand-in {report.standin}): presents "
            f"{stats.presents}, replayed {stats.replayed}, empty {stats.empty}, refused "
            f"{stats.refused}, skipped {stats.skipped}, draws {stats.draws}, "
            f"dumps {stats.dumps}{tail}"
        )
        if report.standin_summary is not None:
            sampled, total, line = report.standin_summary
            out.append(
                f"Stand-in summary: {sampled} of {total} replayed draw(s) sampled it. {line}"
            )
    out += [
        "",
        *render_offset_counters(
            [
                ("lenient", report.stats_off),
                ("lenient + output state", report.stats_on),
                ("lenient + combiner", report.stats_combiner),
                ("lenient + combiner + stand-in", report.stats_standin),
            ]
        ),
        "",
    ]
    if report.frames_standin:
        out.append(
            describe_boots(
                "lenient + combiner + stand-in (NOT the title's texture)", report.frames_standin
            )
        )
    out.append(describe_boots("lenient", report.frames_off))
    out.append(describe_boots("lenient + output state", report.frames_on))
    out += [f"Picture {name}: {summary}" for name, summary in report.pictures.items()]
    if report.rt_ran:
        out += ["", *render_texture_census(report.texture_census), ""]
        out += render_texture_strict(report.stats_rt)
    if report.surface is not None:
        out += ["", *render_surface_model(report.surface), ""]
    out += ["Frame profile:", *[f"  {line}" for line in report.profile_frames]]
    return "\n".join(out)


def surface_json(surface: SurfaceBoots | None) -> dict[str, object] | None:
    """The `--surface-model` boots for `--json`, None when they did not run."""
    if surface is None:
        return None
    census = surface.census
    return {
        "census_only": True,
        "census_by_category": texture_census_by_category(census) if census else None,
        "census_lines": [line.__dict__ for line in census.lines] if census else None,
        "strict": asdict(surface.stats_strict) if surface.stats_strict else None,
        "persist": asdict(surface.stats_persist) if surface.stats_persist else None,
        "summary": surface.summary,
    }


def texture_json(report: Report) -> dict[str, object] | None:
    """The T510 boots for `--json`, None when they did not run (`--no-combiner`)."""
    if not report.rt_ran:
        return None
    census = report.texture_census
    refusal = parse_texture_refusal(report.stats_rt.stopped) if report.stats_rt else None
    return {
        "census_only": True,
        "census": (
            {
                "frames": census.frames,
                "classified": census.classified,
                "lines": [
                    {"category": texture_census_category(line.text), **line.__dict__}
                    for line in census.lines
                ],
                "by_category": texture_census_by_category(census),
            }
            if census
            else None
        ),
        "census_counters": asdict(report.stats_census) if report.stats_census else None,
        "strict": (
            asdict(report.stats_rt) | {"first_refusal": refusal} if report.stats_rt else None
        ),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--host", default="build/tsfp_host", help="a tsfp_host with the shader chunk"
    )
    parser.add_argument("--xbe", default="build/default.xbe")
    parser.add_argument(
        "--swaps", type=int, default=30, help="cut after N Swap dispatches (2 per present)"
    )
    parser.add_argument("--boots", type=int, default=2, help="lenient boots per setting, compared")
    parser.add_argument("--corpus", default="generated/shaders/corpus")
    parser.add_argument(
        "--spv-dir", help="a ready directory of <module>.spv files (skips generating)"
    )
    parser.add_argument("--glslang", default="glslangValidator")
    parser.add_argument(
        "--device", default="", help="$VKRUN_DEVICE for the replay (hardware, llvmpipe)"
    )
    parser.add_argument(
        "--work-dir", help="where frames and modules go (default: a temp directory)"
    )
    parser.add_argument("--timeout", type=int, default=300, help="seconds per boot")
    parser.add_argument(
        "--thread-timeout", type=int, default=0,
        help="the host's watchdog in ms (a replay of thousands of frames needs more than its 10 s)",
    )  # fmt: skip
    parser.add_argument(
        "--no-combiner", action="store_true",
        help="skip the combiner modules and the --gpu-replay-combiner boot (T478)",
    )  # fmt: skip
    parser.add_argument(
        "--standin", default=DEFAULT_STANDIN,
        help="WxH:RRGGBBAA, the stand-in texture of the T497 boots (NOT the title's texture)",
    )  # fmt: skip
    parser.add_argument(
        "--standin-texel-units", action="store_true",
        help="T713, INFERRED, opt-in: the stand-in boots sample it with oT0 in texels "
        "(--gpu-replay-standin-texel-units), so a 640x480 checker maps one to one",
    )  # fmt: skip
    parser.add_argument(
        "--standin-texel-program", action="append", default=[], metavar="HEX",
        help="T719, INFERRED, opt-in, repeatable: a vertex program digest prefix (8..64 lowercase "
        "hex) whose oT0 was MEASURED in texels. With --standin-normalised-program the unit is "
        "chosen per draw, a stand-in draw in neither list refuses",
    )  # fmt: skip
    parser.add_argument(
        "--standin-normalised-program", action="append", default=[], metavar="HEX",
        help="T719, INFERRED, opt-in, repeatable: a vertex program digest prefix whose oT0 was "
        "MEASURED normalised (0..1)",
    )  # fmt: skip
    parser.add_argument(
        "--standin-derive-units", action="store_true",
        help="T724, INFERRED, opt-in: derive the T719 lists by MEASURING each sampling draw's "
        "oT0 range through the translated vertex program (needs --standin-corpus), replaces "
        "the two list flags",
    )  # fmt: skip
    parser.add_argument(
        "--standin-corpus", metavar="DIR",
        help="T724: a tools.nv2a.corpus directory holding the programs by digest",
    )  # fmt: skip
    parser.add_argument(
        "--output-init", choices=("zero", "nv"), default="zero",
        help="T713, INFERRED until HQ20, opt-in: the modules start every vertex output at "
        "zero (default) or nv (0, 0, 0, 1). With --spv-dir it has no effect",
    )  # fmt: skip
    parser.add_argument(
        "--window-to-clip", action="store_true",
        help="T560, INFERRED, opt-in: the modules convert window coordinate programs and every "
        "replay boot says so (--gpu-replay-window-to-clip). With --spv-dir it only states that "
        "DIR holds such modules",
    )  # fmt: skip
    parser.add_argument(
        "--surface-model", action="store_true",
        help="T633/T596, opt-in (xemu-level rules of T736): after the render target texture boots "
        "run three more, the census with --gpu-replay-surface-source, a strict boot with it and "
        "one with --gpu-replay-target-persist as well, and report the first refusal each one "
        "reaches. "
        "Needs the combiner boots (not with --no-combiner)",
    )  # fmt: skip
    parser.add_argument(
        "--json", action="store_true", help="print the table as JSON instead of text"
    )
    args = parser.parse_args(argv)
    check_unit_programs(args)
    if args.standin_derive_units and not args.standin_corpus:
        parser.error("--standin-derive-units needs --standin-corpus")
    if args.standin_derive_units and (
        args.standin_texel_program or args.standin_normalised_program or args.standin_texel_units
    ):
        parser.error(
            "--standin-derive-units replaces the program lists and the texel units, give one"
        )
    if args.surface_model and args.no_combiner:
        parser.error("--surface-model needs the combiner boots: drop --no-combiner")
    if args.boots < 2:
        parser.error("--boots must be at least 2: determinism needs two boots to compare")
    try:
        report = measure(args)
    except Unavailable as error:
        print(f"steady_replay: {error}", file=sys.stderr)
        return 2
    except NoDevice as error:
        print(f"steady_replay: no Vulkan device: {error}", file=sys.stderr)
        return 3
    if args.json:
        rows = [row.__dict__ | {"closure": row.closure.__dict__} for row in report.rows]
        census = (
            report.census.__dict__ | {"outcomes": [o.__dict__ for o in report.census.outcomes]}
            if report.census
            else None
        )
        offsets = {
            label: stats.offset.__dict__ if stats.offset else None
            for label, stats in (
                ("lenient", report.stats_off),
                ("lenient + output state", report.stats_on),
            )
        }
        print(
            json.dumps(
                {
                    "rows": rows,
                    "summary": summarise(report.rows),
                    "census": census,
                    "offset_counters": offsets,
                    "render_target_texture": texture_json(report),
                    "surface_model": surface_json(report.surface),
                },
                indent=1,
            )
        )
    else:
        print(render_report(report))
    differences = [
        *compare_boots(report.frames_off[0], report.frames_off[-1]),
        *compare_boots(report.frames_on[0], report.frames_on[-1]),
        *(
            compare_boots(report.frames_standin[0], report.frames_standin[-1])
            if report.frames_standin
            else []
        ),
    ]
    return 1 if differences else 0


if __name__ == "__main__":
    raise SystemExit(main())
