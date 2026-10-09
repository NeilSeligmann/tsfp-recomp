# SPDX-License-Identifier: GPL-3.0-or-later
"""ROUTE B: the register layout and the combiner formulas as the public documentation gives them.

Written by hand from three sources, described and not copied:

  [GL]   the OpenGL NV_register_combiners extension specification (Khronos registry):
         the mapping table, scale and bias, dot product, mux, the final combiner equation.
  [RNN]  the envytools register database, `nv10_3d` types `nv10_rc_in_alpha`, `nv10_rc_in_rgb`,
         `nv10_rc_out`, `nv10_rc_final0` and `nv10_rc_final1`, and `nv20_3d` for the method
         numbers. The NV10 and NV20 combiner is the ancestor of the NV2A's.
  [NXDK] the register header of the nxdk `pbkit`, which carries NV2A (Xbox) field names for
         the same methods, including the ones that did not exist before the NV2A (texture
         stage programs, the dot-product mapping).
  [XEMU] read as a REFERENCE ONLY (`docs/provenance.md`: GPL, shader translation downstream
         of Cxbx), used here to settle which bit of which word, and to see where it disagrees
         with [GL]. Nothing of it is copied.

`probe.py` is route A, the title's own assembler. `tests/test_nv2a_combiner_routes.py`
checks that the two agree, field by field.
"""

from __future__ import annotations

# (high bit, low bit) of each field inside an operand byte, A in the top byte of a word.
OPERAND_FIELDS = {"source": (3, 0), "usage": (4, 4), "mapping": (7, 5)}
OPERAND_SHIFT = {"D": 0, "C": 8, "B": 16, "A": 24}

SOURCES = {
    0: "ZERO",
    1: "CONSTANT_COLOR0",
    2: "CONSTANT_COLOR1",
    3: "FOG",
    4: "PRIMARY_COLOR",
    5: "SECONDARY_COLOR",
    8: "TEXTURE0",
    9: "TEXTURE1",
    10: "TEXTURE2",
    11: "TEXTURE3",
    12: "SPARE0",
    13: "SPARE1",
    14: "SPARE0_PLUS_SECONDARY_COLOR",
    15: "E_TIMES_F",
}

MAPPINGS = {
    0: "UNSIGNED_IDENTITY",
    1: "UNSIGNED_INVERT",
    2: "EXPAND_NORMAL",
    3: "EXPAND_NEGATE",
    4: "HALF_BIAS_NORMAL",
    5: "HALF_BIAS_NEGATE",
    6: "SIGNED_IDENTITY",
    7: "SIGNED_NEGATE",
}

#: [GL] table 4, as text. The reference model's `map_input` is tested against these.
MAPPING_FORMULAS = {
    "UNSIGNED_IDENTITY": "max(0, e)",
    "UNSIGNED_INVERT": "1 - min(max(e, 0), 1)",
    "EXPAND_NORMAL": "2 * max(0, e) - 1",
    "EXPAND_NEGATE": "-2 * max(0, e) + 1",
    "HALF_BIAS_NORMAL": "max(0, e) - 0.5",
    "HALF_BIAS_NEGATE": "-max(0, e) + 0.5",
    "SIGNED_IDENTITY": "e",
    "SIGNED_NEGATE": "-e",
}

#: [RNN] nv10_rc_out and [NXDK] SET_COMBINER_*_OCW.
OUTPUT_FIELDS = {
    "CD_DST": (3, 0),
    "AB_DST": (7, 4),
    "SUM_DST": (11, 8),
    "CD_DOT_ENABLE": (12, 12),
    "AB_DOT_ENABLE": (13, 13),
    "MUX_ENABLE": (14, 14),
    "OP": (17, 15),
    "BLUETOALPHA_CD": (18, 18),
    "BLUETOALPHA_AB": (19, 19),
}
#: [NXDK] names for the OP field. [RNN] splits it into BIAS (bit 15) and SCALE (17:16) with
#: SCALE 0 none, 1 x2, 2 x4, 3 x1/2, which is the same encoding.
OP_VALUES = {
    "NOSHIFT": 0,
    "NOSHIFT_BIAS": 1,
    "SHIFTLEFTBY1": 2,
    "SHIFTLEFTBY1_BIAS": 3,
    "SHIFTLEFTBY2": 4,
    "SHIFTRIGHTBY1": 6,
}

#: [NXDK] SET_COMBINER_CONTROL.
CONTROL_FIELDS = {
    "ITERATION_COUNT": (7, 0),
    "MUX_SELECT": (11, 8),
    "FACTOR0": (15, 12),
    "FACTOR1": (31, 16),
}
MUX_SELECT_VALUES = {"LSB": 0, "MSB": 1}
FACTOR_VALUES = {"SAME_FACTOR_ALL": 0, "EACH_STAGE": 1}

#: [RNN] nv10_rc_final1 and [NXDK] SPECULAR_FOG_CW1. G, F, E sit at 15:8, 23:16, 31:24.
FINAL1_FLAGS = {
    "SPECULAR_CLAMP": 7,
    "SPECULAR_ADD_INVERT_R5": 6,
    "SPECULAR_ADD_INVERT_R12": 5,
}
FINAL1_OPERAND_SHIFT = {"G": 8, "F": 16, "E": 24}

#: [NXDK] SET_SHADER_STAGE_PROGRAM, 5 bits per stage.
STAGE_PROGRAM_BITS = 5
STAGE_PROGRAMS = {
    "PROGRAM_NONE": 0,
    "2D_PROJECTIVE": 1,
    "3D_PROJECTIVE": 2,
    "CUBE_MAP": 3,
    "PASS_THROUGH": 4,
    "CLIP_PLANE": 5,
    "BUMPENVMAP": 6,
    "BUMPENVMAP_LUMINANCE": 7,
    "BRDF": 8,
    "DOT_ST": 9,
    "DOT_ZW": 10,
    "DOT_RFLCT_DIFF": 11,
    "DOT_RFLCT_SPEC": 12,
    "DOT_STR_3D": 13,
    "DOT_STR_CUBE": 14,
    "DEPENDENT_AR": 15,
    "DEPENDENT_GB": 16,
    "DOT_PRODUCT": 17,
    "DOT_RFLCT_SPEC_CONST": 18,
}

#: Where each block of a definition starts, as (first dword, count). The render-state index.
BLOCKS = {
    "ALPHA_ICW": (0, 8),
    "SPECULAR_FOG_CW0": (8, 1),
    "SPECULAR_FOG_CW1": (9, 1),
    "FACTOR0": (10, 8),
    "FACTOR1": (18, 8),
    "ALPHA_OCW": (26, 8),
    "COLOR_ICW": (34, 8),
    "SHADER_CLIP_PLANE_MODE": (42, 1),
    "SPECULAR_FOG_FACTOR": (43, 2),
    "COLOR_OCW": (45, 8),
    "COMBINER_CONTROL": (53, 1),
    "SHADER_STAGE_PROGRAM": (54, 1),
    "DOT_RGBMAPPING": (55, 1),
    "SHADER_OTHER_STAGE_INPUT": (56, 1),
}

#: Where the documents disagree with each other, or with the title's assembler. Each is
#: argued in `docs/combiner-translator.md` section 4.
DISAGREEMENTS = {
    "final_mappings": (
        "[GL] allows only UNSIGNED_IDENTITY and UNSIGNED_INVERT on final combiner inputs. "
        "[RNN] and [NXDK] give the field 3 bits and the title's assembler emits all eight."
    ),
    "final1_flags": (
        "[RNN] names bit 7 only (color sum clamp). [NXDK] adds bit 6 (invert R5) and a field "
        "whose mask 0x3f overlaps bits 5 and 6 and is read as bit 5 (invert R12)."
    ),
    "unsigned_identity_upper_clamp": (
        "[GL] table 4 gives max(0, e) with no upper clamp, [GL] prose says the final "
        "combiner inputs are clamped to [0, 1]. The model follows the table."
    ),
    "initial_spare0": (
        "[GL] says spare0 alpha starts as texture 0 alpha and spare0 colour is undefined. "
        "[XEMU] starts spare0 alpha at 1.0 when texture stage 0 is off. The model follows "
        "[XEMU] there and [GL] otherwise, and measures whether the title can tell."
    ),
}


def field(word: int, high: int, low: int) -> int:
    """Extract bits `high:low` of `word`."""
    return (word >> low) & ((1 << (high - low + 1)) - 1)


def operand(word: int, letter: str, name: str) -> int:
    """One field of operand `letter` (A to D) of an input control word."""
    high, low = OPERAND_FIELDS[name]
    shift = OPERAND_SHIFT[letter]
    return field(word, high + shift, low + shift)


def final1_operand(word: int, letter: str, name: str) -> int:
    high, low = OPERAND_FIELDS[name]
    shift = FINAL1_OPERAND_SHIFT[letter]
    return field(word, high + shift, low + shift)
