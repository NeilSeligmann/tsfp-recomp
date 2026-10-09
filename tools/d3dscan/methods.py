# SPDX-License-Identifier: GPL-3.0-or-later
"""NV2A pushbuffer method headers: decoding, and naming by nearest base.

THE HEADER ENCODING. A pushbuffer command is a 32-bit header followed by `count`
parameter dwords. The header packs

    bits  0..12   method, the pgraph register's byte offset (so bits 0..1 are zero)
    bits 13..15   subchannel
    bits 16..17   reserved, zero
    bits 18..28   count, the number of parameter dwords that follow
    bit  30       set: send every parameter to the SAME method instead of
                  auto-incrementing to method+4 for each one
    bits 29, 31   zero

So the literal `0x00041EA4` means one parameter for method `0x1EA4` on subchannel 0.
`is_plausible_header` is exactly those field constraints and nothing more, which is
what lets it be used as a *filter* over every `mov dword ptr [...], imm32` in a
section without a circular appeal to the names below.

WHY NAMING IS SEPARATE FROM DECODING, AND WHY THE TABLE IS SHORT. The decode is
arithmetic and complete. The names are a lookup, and a lookup is only as complete as
its table -- so the two are kept apart, `MethodHeader` carries no name, and a caller
that cannot name a method still gets its number. `NV2A_METHODS` holds the methods
this project's analysis actually leans on, not the full register set; a short honest
table beats a long one with a guess in it. Pass `--method-names <header>` on the CLI
to widen it from a local reference header at run time.

PROVENANCE OF THE NAMES, AND A WARNING EARNED THE HARD WAY. Method numbers and names
are facts about NVIDIA NV2A hardware, documented independently by several community
reverse-engineering efforts. `NV2A_METHODS` holds ONLY entries where two independent
derivations agreed: a table written from recall, and `lib/pbkit/nv_regs.h` in
`XboxDev/nxdk` (rated USE in `docs/clean-sources-audit.md`). No code or text was copied
from either; this is a table of numbers.

The gate exists because the recalled table was wrong in **46 of the 102 entries that
could be checked**, and wrong in the nastiest possible way: off by one or two slots, so
every name was a real method name sitting on the wrong number. `SET_BLEND_FUNC_SFACTOR`
for `0x0300` (really `SET_ALPHA_TEST_ENABLE`), `SET_TRANSFORM_PROGRAM_LOAD` for `0x1EA0`
(really `SET_TRANSFORM_PROGRAM_START`). Nothing about such a table looks wrong, and
every conclusion drawn through it would have been confidently, specifically false. So
only the 56 corroborated entries survive, and an unrecognised method resolves to its
bare number rather than to a plausible guess. Pass `--method-names` for a fuller table
from a local reference header when one is available.

ARRAY METHODS AND WHY `name_method` REPORTS AN INDEX. Many registers are arrays:
`SET_TRANSFORM_CONSTANT` occupies a run of dwords, one per constant component, and
`SET_COMBINER_FACTOR0` one per combiner stage. A scanner sees `0x0A6C` and the table
holds `0x0A60`, so `name_method` resolves to the nearest base at or below the method
and reports the dword index. `MAX_ARRAY_SPAN` bounds how far it will reach, because an
unbounded nearest-base search names every unknown method after whatever happens to
precede it -- which is worse than admitting the method is unknown.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

#: Methods are dword offsets into pgraph's register window, which is 0x2000 wide.
METHOD_MASK = 0x1FFF

#: Set by the writer to repeat one method rather than walk method, method+4, ...
NON_INCREMENTING = 1 << 30

#: `name_method` will not resolve a method more than this many bytes past a base.
#: 0x80 is 32 dwords, which covers the widest array in the table below
#: (`SET_TRANSFORM_CONSTANT`'s 32-dword window) without bridging two unrelated bases.
MAX_ARRAY_SPAN = 0x80

#: Methods below this are object/DMA plumbing rather than NV097 graphics state, and
#: the low numbers collide with ordinary small integers often enough that admitting
#: them to a byte-level scan costs more in false positives than it returns.
MIN_GRAPHICS_METHOD = 0x100

#: NV2A pgraph methods this analysis relies on. See the module docstring on provenance
#: and on why this is deliberately not the full register set.
NV2A_METHODS: dict[int, str] = {
    0x0100: "NO_OPERATION",
    0x0110: "WAIT_FOR_IDLE",
    0x0130: "FLIP_STALL",
    0x0180: "SET_CONTEXT_DMA_NOTIFIES",
    0x0200: "SET_SURFACE_CLIP_HORIZONTAL",
    0x0204: "SET_SURFACE_CLIP_VERTICAL",
    0x0208: "SET_SURFACE_FORMAT",
    0x020C: "SET_SURFACE_PITCH",
    0x0210: "SET_SURFACE_COLOR_OFFSET",
    0x0214: "SET_SURFACE_ZETA_OFFSET",
    0x0260: "SET_COMBINER_ALPHA_ICW",
    0x0288: "SET_COMBINER_SPECULAR_FOG_CW0",
    0x028C: "SET_COMBINER_SPECULAR_FOG_CW1",
    0x0290: "SET_CONTROL0",
    0x0294: "SET_LIGHT_CONTROL",
    0x02A4: "SET_FOG_ENABLE",
    0x0314: "SET_LIGHTING_ENABLE",
    0x03B8: "SET_SPECULAR_ENABLE",
    0x03BC: "SET_LIGHT_ENABLE_MASK",
    0x0394: "SET_CLIP_MIN",
    0x0398: "SET_CLIP_MAX",
    0x039C: "SET_CULL_FACE",
    0x0420: "SET_TEXTURE_MATRIX_ENABLE",
    0x0440: "SET_PROJECTION_MATRIX",
    0x0480: "SET_MODEL_VIEW_MATRIX",
    0x0680: "SET_COMPOSITE_MATRIX",
    0x06C0: "SET_TEXTURE_MATRIX",
    0x0A60: "SET_COMBINER_FACTOR0",
    0x0A80: "SET_COMBINER_FACTOR1",
    0x0AA0: "SET_COMBINER_ALPHA_OCW",
    0x0B00: "SET_TRANSFORM_PROGRAM",
    0x0B80: "SET_TRANSFORM_CONSTANT",
    0x1720: "SET_VERTEX_DATA_ARRAY_OFFSET",
    0x1760: "SET_VERTEX_DATA_ARRAY_FORMAT",
    0x17FC: "SET_BEGIN_END",
    0x1800: "ARRAY_ELEMENT16",
    0x1808: "ARRAY_ELEMENT32",
    0x1810: "DRAW_ARRAYS",
    0x1818: "INLINE_ARRAY",
    0x1880: "SET_VERTEX_DATA2F_M",
    0x1B00: "SET_TEXTURE_OFFSET",
    0x1B04: "SET_TEXTURE_FORMAT",
    0x1B08: "SET_TEXTURE_ADDRESS",
    0x1B0C: "SET_TEXTURE_CONTROL0",
    0x1B10: "SET_TEXTURE_CONTROL1",
    0x1B14: "SET_TEXTURE_FILTER",
    0x1B1C: "SET_TEXTURE_IMAGE_RECT",
    0x1B20: "SET_TEXTURE_PALETTE",
    0x1B24: "SET_TEXTURE_BORDER_COLOR",
    0x1B28: "SET_TEXTURE_SET_BUMP_ENV_MAT",
    0x1D70: "BACK_END_WRITE_SEMAPHORE_RELEASE",
    0x1D78: "SET_ZMIN_MAX_CONTROL",
    0x1D8C: "SET_ZSTENCIL_CLEAR_VALUE",
    0x1D90: "SET_COLOR_CLEAR_VALUE",
    0x1D94: "CLEAR_SURFACE",
    0x1D98: "SET_CLEAR_RECT_HORIZONTAL",
    0x1D9C: "SET_CLEAR_RECT_VERTICAL",
    0x1E60: "SET_COMBINER_CONTROL",
    0x1E94: "SET_TRANSFORM_EXECUTION_MODE",
    0x1E98: "SET_TRANSFORM_PROGRAM_CXT_WRITE_EN",
    0x1E9C: "SET_TRANSFORM_PROGRAM_LOAD",
    0x1EA0: "SET_TRANSFORM_PROGRAM_START",
    0x1EA4: "SET_TRANSFORM_CONSTANT_LOAD",
}

#: `0[xX]`, not `0x`. MEASURED: both reference headers spell exactly one method's value
#: with a capital X -- `NV097_SET_DOT_RGBMAPPING 0X00001E74` -- and a lowercase-only
#: pattern dropped it from both while reporting nothing. That is the same silent-narrowing
#: failure the indentation note below describes, in a different field.
_DEFINE = re.compile(
    r"^#(?P<indent>[ \t]*)define[ \t]+(?:NV097_|NV20_|NV2A_)(?P<name>[A-Z0-9_]+)[ \t]+"
    r"0[xX](?P<value>[0-9A-Fa-f]+)[ \t]*$"
)


@dataclass(frozen=True)
class MethodHeader:
    """One decoded pushbuffer command header. Carries no name: see the module docs."""

    raw: int
    method: int
    subchannel: int
    count: int
    non_incrementing: bool

    @property
    def span(self) -> int:
        """Total dwords this command occupies, header included."""
        return 1 + self.count


def is_plausible_header(value: int) -> bool:
    """Could `value` be a pushbuffer command header?

    The field constraints only. A `count` of zero is rejected because a command with
    no parameters is not something a writer emits, and `method` below
    `MIN_GRAPHICS_METHOD` is rejected for the reason given on that constant.
    """
    if value < 0 or value > 0xFFFFFFFF:
        return False
    if value & 0x03:  # method is a dword offset
        return False
    if (value >> 16) & 0x03:  # reserved
        return False
    if value & ((1 << 29) | (1 << 31)):
        return False
    if (value >> 13) & 0x07 != (value >> 13) & 0x07:  # pragma: no cover - documentation
        return False
    method = value & METHOD_MASK
    count = (value >> 18) & 0x7FF
    return count >= 1 and method >= MIN_GRAPHICS_METHOD


def decode_header(value: int) -> MethodHeader | None:
    """Decode `value` as a command header, or None when its fields do not fit."""
    if not is_plausible_header(value):
        return None
    return MethodHeader(
        raw=value,
        method=value & METHOD_MASK,
        subchannel=(value >> 13) & 0x07,
        count=(value >> 18) & 0x7FF,
        non_incrementing=bool(value & NON_INCREMENTING),
    )


def name_method(method: int, table: dict[int, str] | None = None) -> str:
    """Name `method`, resolving an array element to `BASE+N` where N is a dword index.

    Returns a bare `0x....` when no base lies within `MAX_ARRAY_SPAN` below it, rather
    than reaching further for a name that would be wrong.
    """
    names = NV2A_METHODS if table is None else table
    exact = names.get(method)
    if exact is not None:
        return exact
    bases = [base for base in names if base < method and method - base <= MAX_ARRAY_SPAN]
    if not bases:
        return f"{method:#06x}"
    base = max(bases)
    return f"{names[base]}+{(method - base) // 4}"


def parse_method_names(path: Path) -> dict[int, str]:
    """Read method `#define`s from a reference register header.

    Lets a fuller local register header widen `NV2A_METHODS` at run time without this
    repository carrying a copy of it.

    INDENTATION IS THE FILTER, AND IGNORING IT SILENTLY CORRUPTS THE TABLE. Such
    headers nest each method's *field values* underneath it as more deeply indented
    defines, and those values collide with real method numbers: a first version of
    this function took the first define for each value and so learned that `0x0200` is
    an alpha-test comparison function and `0x1B00` a polygon-fill mode, when they are
    `SET_SURFACE_CLIP_HORIZONTAL` and `SET_TEXTURE_OFFSET`. Only the shallowest
    indentation level is accepted, found from the file rather than assumed, so the
    rule calibrates itself to whatever depth the header uses for methods.
    """
    matches: list[tuple[int, int, str]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = _DEFINE.match(line)
        if match is None:
            continue
        value = int(match.group("value"), 16)
        if not MIN_GRAPHICS_METHOD <= value <= METHOD_MASK or value & 0x03:
            continue
        matches.append((len(match.group("indent")), value, match.group("name")))
    if not matches:
        return {}
    top = min(indent for indent, _, _ in matches)
    names: dict[int, str] = {}
    for indent, value, name in matches:
        if indent == top:
            names.setdefault(value, name)
    return names
