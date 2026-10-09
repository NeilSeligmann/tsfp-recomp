# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/gpu/gpu_combiner.c`, the register-combiner plan and refusals (T75).

T259. The T75 commit records "19 C mutants and 9 Python mutants killed", of which the Python-side
file (`tests/test_gpu_combiner_replay.py`) keeps 22 committed with their anchors: factor byte
order, key word dropped, unused stages in the key, fog, missing test texture, texture mode,
initial state, unwritten word, refused-only-when-allowed, the sum register's spare0 read, the E/F
product refusal. They are NOT repeated here. This set covers the rest of the plan, which is the
code that decides what is REFUSED and what is flagged INFERRED, grouped by what a survivor would
let through:

    masks       the legal source and destination register sets, the key masks, the field
                positions of the output word. A wrong mask admits a configuration the hardware
                rejects, or refuses one the title uses
    analysis    which registers a stage reads and writes, what counts as read-before-written
                (the INITIAL_STATE inference), which factor constants are read
    NEED        which words must have been written, and which are inferred as zero
    texture     stage programs, the test-texture requirement, size limits
    inference   which condition sets which INFERRED bit
    key         canonicalisation: the words that change the translation and only those. A
                mutation that changes the key changes the module NAME, so a title would look up
                a module that was never generated
    constants   where the unpacked factor and final constants land
    SPIR-V      the interface scan the replay uses to check varyings

EQUIVALENT MUTANTS FOUND AND LEFT OUT (a survivor that no input can kill is not a missing test):
  - `note_read`'s second branch (`reg == REG_SPARE0 && (missing & COMPONENT_ALPHA) != 0u`) is DEAD
    CODE: every caller passes ONE component (`operand_components` returns RGB or ALPHA, never
    both), so `missing` can never hold both bits. Merging or dropping that branch changes nothing.
  - `word_count < 5u` -> `< 4u` in the SPIR-V scan: a four-word buffer then fails a few lines later
    with the same `false` (no entry point). Only `*out_mask` differs, and it is not a contract.

Most kills come from `test_gpu_combiner_edges` (added by T259 for the survivors of this set) and
`test_gpu_combiner` and the parity suite. None of them needs a Vulkan device, so a run
with no device kills all of them (`_NEEDS_DEVICE` below is empty).
"""

_FILE = "src/gpu/gpu_combiner.c"
# The C-versus-Python plan parity runs in pytest (a runner built from the MUTATED C source), so
# most of the plan is pinned there and not by `test_gpu_combiner` alone. Half a second.
_PARITY = (
    'pytest:tests/test_gpu_combiner_replay.py -k "agree or structure_indices or header_names '
    'or key_ignores or replay_form or replay_shader or final_only"'
)
_PRIMARY = ["test_gpu_combiner", "test_gpu_combiner_edges", "test_gpu_pgraph_replay", _PARITY]


def _row(mutation_id: str, old: str, new: str, why: str, targets: list[str] | None = None) -> dict:
    return {
        "id": f"gpu-cmb-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": list(targets or _PRIMARY),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ masks and positions
    _row(
        "sources-general-admits-6",
        "#define SOURCES_GENERAL 0x3F3Fu",
        "#define SOURCES_GENERAL 0x3F7Fu",
        "a general stage may read register 6 (not a source on this hardware).",
    ),
    _row(
        "sources-general-refuses-5",
        "#define SOURCES_GENERAL 0x3F3Fu",
        "#define SOURCES_GENERAL 0x3F1Fu",
        "a general stage may not read the secondary colour (register 5), which titles do.",
    ),
    _row(
        "destinations-admit-1",
        "#define DESTINATIONS_GENERAL 0x3F31u",
        "#define DESTINATIONS_GENERAL 0x3F33u",
        "a stage may write constant colour 0, which is read-only.",
    ),
    _row(
        "destinations-refuse-spare1",
        "#define DESTINATIONS_GENERAL 0x3F31u",
        "#define DESTINATIONS_GENERAL 0x1F31u",
        "a stage may not write spare1.",
    ),
    _row(
        "sources-final-refuses-prod",
        "#define SOURCES_FINAL 0xFF3Fu",
        "#define SOURCES_FINAL 0x7F3Fu",
        "the final combiner may not read the E times F product, which it may.",
    ),
    _row(
        "sources-final-admits-6",
        "#define SOURCES_FINAL 0xFF3Fu",
        "#define SOURCES_FINAL 0xFF7Fu",
        "the final combiner may read register 6.",
    ),
    _row(
        "control-key-mux-bit",
        "#define CONTROL_KEY_MASK (0xFFu | CONTROL_MUX_MSB | CONTROL_FACTOR0_EACH | CONTROL_FACTOR1_EACH)",
        "#define CONTROL_KEY_MASK (0xFFu | CONTROL_FACTOR0_EACH | CONTROL_FACTOR1_EACH)",
        "the mux-MSB control bit is dropped from the key, so two configurations that translate "
        "differently share one module name.",
    ),
    _row(
        "control-key-factor0-each-bit",
        "#define CONTROL_KEY_MASK (0xFFu | CONTROL_MUX_MSB | CONTROL_FACTOR0_EACH | CONTROL_FACTOR1_EACH)",
        "#define CONTROL_KEY_MASK (0xFFu | CONTROL_MUX_MSB | CONTROL_FACTOR1_EACH)",
        "the per-stage factor0 flag is dropped from the key.",
    ),
    _row(
        "control-key-factor1-each-bit",
        "#define CONTROL_KEY_MASK (0xFFu | CONTROL_MUX_MSB | CONTROL_FACTOR0_EACH | CONTROL_FACTOR1_EACH)",
        "#define CONTROL_KEY_MASK (0xFFu | CONTROL_MUX_MSB | CONTROL_FACTOR0_EACH)",
        "the per-stage factor1 flag is dropped from the key.",
    ),
    _row(
        "output-key-mask-top",
        "#define OUTPUT_KEY_MASK 0x000FFFFFu",
        "#define OUTPUT_KEY_MASK 0x0007FFFFu",
        "the blue-to-alpha bit (19) of every output word is dropped from the key.",
    ),
    _row(
        "output-key-mask-wide",
        "#define OUTPUT_KEY_MASK 0x000FFFFFu",
        "#define OUTPUT_KEY_MASK 0x00FFFFFFu",
        "bits above 19 of an output word leak into the key, so ignored bits split one "
        "configuration into several module names.",
    ),
    _row(
        "final1-key-mask",
        "#define FINAL1_KEY_MASK 0xFFFFFFE0u",
        "#define FINAL1_KEY_MASK 0xFFFFFFFFu",
        "the low five bits of final word 1, which do not change the translation, leak into the key.",
    ),
    _row(
        "output-ab-dot-position",
        "#define OUT_AB_DOT (1u << 13)",
        "#define OUT_AB_DOT (1u << 11)",
        "the AB dot-product flag is read from the wrong bit.",
    ),
    _row(
        "output-cd-dot-position",
        "#define OUT_CD_DOT (1u << 12)",
        "#define OUT_CD_DOT (1u << 11)",
        "the CD dot-product flag is read from the wrong bit.",
    ),
    _row(
        "output-mux-position",
        "#define OUT_MUX (1u << 14)",
        "#define OUT_MUX (1u << 11)",
        "the mux flag is read from the wrong bit, so the spare0 alpha select is not noted as read.",
    ),
    _row(
        "output-blue-to-alpha-cd-position",
        "#define OUT_BLUE_TO_ALPHA_CD (1u << 18)",
        "#define OUT_BLUE_TO_ALPHA_CD (1u << 11)",
        "the CD blue-to-alpha flag is read from the wrong bit.",
    ),
    _row(
        "output-blue-to-alpha-ab-position",
        "#define OUT_BLUE_TO_ALPHA_AB (1u << 19)",
        "#define OUT_BLUE_TO_ALPHA_AB (1u << 11)",
        "the AB blue-to-alpha flag is read from the wrong bit.",
    ),
    _row(
        "key-indices-alpha-input",
        "    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,                                /* alpha input, final 0 and 1 */",
        "    0, 1, 2, 3, 4, 5, 6, 7, 8, 8,                                /* alpha input, final 0 and 1 */",
        "final combiner word 1 is replaced by word 0 in the key.",
    ),
    _row(
        "key-indices-alpha-output",
        "    26, 27, 28, 29, 30, 31, 32, 33,                              /* alpha output */",
        "    26, 27, 28, 29, 30, 31, 32, 32,                              /* alpha output */",
        "the last alpha output word is replaced by its neighbour in the key.",
    ),
    _row(
        "key-indices-colour-input",
        "    34, 35, 36, 37, 38, 39, 40, 41,                              /* colour input */",
        "    34, 35, 36, 37, 38, 39, 40, 40,                              /* colour input */",
        "the last colour input word is replaced by its neighbour in the key.",
    ),
    _row(
        "key-indices-control",
        "    45, 46, 47, 48, 49, 50, 51, 52, 53, 54,                      /* colour output, control, program */",
        "    45, 46, 47, 48, 49, 50, 51, 52, 53, 53,                      /* colour output, control, program */",
        "the stage program word is replaced by the control word in the key.",
    ),
    _row(
        "name-top-byte",
        "bytes[i * 4u + 3u] = (uint8_t)(key[i] >> 24);",
        "bytes[i * 4u + 3u] = (uint8_t)(key[i] >> 23);",
        "the name hashes a corrupted top byte, so no generated module is ever found.",
    ),
    # ------------------------------------------------------------------ note_read and operands
    _row(
        "note-read-spare1-untracked",
        "if (reg != REG_SPARE0 && reg != REG_SPARE1) {\n        return;",
        "if (reg != REG_SPARE0) {\n        return;",
        "spare1 is never tracked, so reading it before any write is not flagged INFERRED.",
    ),
    _row(
        "operand-register-mask",
        "return (word >> (24u - 8u * slot)) & 0xFu;",
        "return (word >> (24u - 8u * slot)) & 0x7u;",
        "registers 8 to 15 (spares, sum, product) alias registers 0 to 7.",
    ),
    _row(
        "operand-alpha-bit",
        "return ((word >> (24u - 8u * slot)) & 0x10u) != 0u ? COMPONENT_ALPHA : COMPONENT_RGB;",
        "return ((word >> (24u - 8u * slot)) & 0x20u) != 0u ? COMPONENT_ALPHA : COMPONENT_RGB;",
        "every operand is read as RGB, so an alpha read of spare0 is never seen.",
    ),
    # ------------------------------------------------------------------ half stages
    _row(
        "half-factor0-each",
        "an->factor0_reads |= 1u << ((control & CONTROL_FACTOR0_EACH) != 0u ? stage : 0u);",
        "an->factor0_reads |= 1u << 0u;",
        "with per-stage constants, every read of constant 0 is attributed to factor 0, so a "
        "stage's own factor word is never required.",
    ),
    _row(
        "half-factor0-shared",
        "an->factor0_reads |= 1u << ((control & CONTROL_FACTOR0_EACH) != 0u ? stage : 0u);",
        "an->factor0_reads |= 1u << stage;",
        "with ONE shared constant, each stage requires its own word.",
    ),
    _row(
        "half-factor1-each",
        "an->factor1_reads |= 1u << ((control & CONTROL_FACTOR1_EACH) != 0u ? stage : 0u);",
        "an->factor1_reads |= 1u << 0u;",
        "with per-stage constants, every read of constant 1 is attributed to factor 1.",
    ),
    _row(
        "half-factor1-shared",
        "an->factor1_reads |= 1u << ((control & CONTROL_FACTOR1_EACH) != 0u ? stage : 0u);",
        "an->factor1_reads |= 1u << stage;",
        "with ONE shared constant, each stage requires its own word.",
    ),
    _row(
        "half-operation-7",
        "if (operation == 5u || operation == 7u) {",
        "if (operation == 5u) {",
        "x1/2 with a bias, which the hardware treats as invalid, is accepted.",
    ),
    _row(
        "half-operation-5",
        "if (operation == 5u || operation == 7u) {",
        "if (operation == 7u) {",
        "x4 with a bias, which the hardware treats as invalid, is accepted.",
    ),
    _row(
        "half-operation-shift",
        "const uint32_t operation = (output_word >> 15) & 7u;",
        "const uint32_t operation = (output_word >> 14) & 7u;",
        "the output operation is read from the wrong bits.",
    ),
    _row(
        "half-alpha-dot-ab",
        "(output_word & (OUT_AB_DOT | OUT_CD_DOT | OUT_BLUE_TO_ALPHA_AB | OUT_BLUE_TO_ALPHA_CD)) != 0u) {",
        "(output_word & (OUT_CD_DOT | OUT_BLUE_TO_ALPHA_AB | OUT_BLUE_TO_ALPHA_CD)) != 0u) {",
        "an alpha half with the AB dot flag is accepted.",
    ),
    _row(
        "half-alpha-blue-to-alpha-cd",
        "(output_word & (OUT_AB_DOT | OUT_CD_DOT | OUT_BLUE_TO_ALPHA_AB | OUT_BLUE_TO_ALPHA_CD)) != 0u) {",
        "(output_word & (OUT_AB_DOT | OUT_CD_DOT | OUT_BLUE_TO_ALPHA_AB)) != 0u) {",
        "an alpha half with blue-to-alpha CD is accepted.",
    ),
    _row(
        "half-cd-mask",
        "const uint32_t cd = output_word & 0xFu;",
        "const uint32_t cd = output_word & 0x7u;",
        "the CD destination aliases the low registers.",
    ),
    _row(
        "half-ab-shift",
        "const uint32_t ab = (output_word >> 4) & 0xFu;",
        "const uint32_t ab = (output_word >> 5) & 0xFu;",
        "the AB destination is read from the wrong bits.",
    ),
    _row(
        "half-sum-shift",
        "const uint32_t sum = (output_word >> 8) & 0xFu;",
        "const uint32_t sum = (output_word >> 9) & 0xFu;",
        "the sum destination is read from the wrong bits.",
    ),
    _row(
        "half-destination-sum-unchecked",
        "        if (((DESTINATIONS_GENERAL >> destinations[i]) & 1u) == 0u) {",
        "        if (i < 2u && ((DESTINATIONS_GENERAL >> destinations[i]) & 1u) == 0u) {",
        "the sum destination is never checked, so a stage may write the fog register or a texture.",
    ),
    _row(
        "half-mux-reads-colour",
        "note_read(an, REG_SPARE0, COMPONENT_ALPHA); /* the mux select is spare0 alpha */",
        "note_read(an, REG_SPARE0, COMPONENT_RGB); /* the mux select is spare0 alpha */",
        "the mux select is noted as a spare0 colour read, not its alpha, so the t0.a start is "
        "never inferred.",
    ),
    _row(
        "half-cd-blue-to-alpha",
        "writes[cd] |= COMPONENT_RGB | ((output_word & OUT_BLUE_TO_ALPHA_CD) != 0u ? COMPONENT_ALPHA : 0u);",
        "writes[cd] |= COMPONENT_RGB;",
        "the blue-to-alpha CD write is not seen, so a later alpha read of that register is "
        "flagged as a read before write.",
    ),
    _row(
        "half-ab-blue-to-alpha",
        "writes[ab] |= COMPONENT_RGB | ((output_word & OUT_BLUE_TO_ALPHA_AB) != 0u ? COMPONENT_ALPHA : 0u);",
        "writes[ab] |= COMPONENT_RGB;",
        "the blue-to-alpha AB write is not seen.",
    ),
    _row(
        "half-sum-writes-alpha",
        "writes[sum] |= COMPONENT_RGB;",
        "writes[sum] |= COMPONENT_RGB | COMPONENT_ALPHA;",
        "the sum output is taken to define alpha too, hiding a read of an undefined alpha.",
    ),
    _row(
        "half-alpha-writes-rgb",
        "writes[destinations[i]] |= COMPONENT_ALPHA;",
        "writes[destinations[i]] |= COMPONENT_RGB;",
        "an alpha half is taken to define colour instead of alpha.",
    ),
    # ------------------------------------------------------------------ final combiner reads
    _row(
        "final-read-sum-secondary",
        "        note_read(an, REG_SPARE0, COMPONENT_RGB);\n        note_read(an, REG_V1, COMPONENT_RGB);\n    } else if (reg == REG_PROD) {",
        "        note_read(an, REG_SPARE0, COMPONENT_RGB);\n    } else if (reg == REG_PROD) {",
        "a read through the sum register does not note the secondary colour, so the colour "
        "range inference is not raised.",
    ),
    _row(
        "final-read-product-sources",
        "        for (unsigned slot = 0u; slot < 2u; slot++) {\n            const uint32_t source = operand_register(word1, slot);",
        "        for (unsigned slot = 0u; slot < 1u; slot++) {\n            const uint32_t source = operand_register(word1, slot);",
        "a read of the product notes only E, not F.",
    ),
    _row(
        "final-read-product-c0",
        "            if (source == REG_C0) {\n                an->final_c0_read = true;",
        "            if (source == REG_C1) {\n                an->final_c0_read = true;",
        "a product operand of constant 0 is not noticed, so the constant is not required.",
    ),
    _row(
        "final-read-product-sum",
        "            } else if (source == REG_SUM) {\n                note_read(an, REG_SPARE0, COMPONENT_RGB);\n                note_read(an, REG_V1, COMPONENT_RGB);",
        "            } else if (source == REG_SUM) {\n                note_read(an, REG_V1, COMPONENT_RGB);",
        "a product operand through the sum does not note spare0.",
    ),
    _row(
        "analyse-final0-slots",
        "    for (unsigned slot = 0u; slot < 4u; slot++) {\n        final_read(an, operand_register(word0, slot), operand_components(word0, slot), word1);",
        "    for (unsigned slot = 0u; slot < 3u; slot++) {\n        final_read(an, operand_register(word0, slot), operand_components(word0, slot), word1);",
        "the D operand of the final combiner is never read.",
    ),
    _row(
        "analyse-final1-g",
        "    final_read(an, operand_register(word1, 2), operand_components(word1, 2), word1);",
        "    (void)word1;",
        "the G operand of the final combiner is never read.",
    ),
    _row(
        "analyse-final0-check",
        "    for (unsigned slot = 0u; slot < 4u; slot++) {\n        const uint32_t reg = operand_register(word0, slot);",
        "    for (unsigned slot = 0u; slot < 3u; slot++) {\n        const uint32_t reg = operand_register(word0, slot);",
        "the D operand of the final combiner is not checked against the legal sources.",
    ),
    _row(
        "analyse-final1-check",
        "    for (unsigned slot = 0u; slot < 3u; slot++) {\n        const uint32_t reg = operand_register(word1, slot);",
        "    for (unsigned slot = 0u; slot < 2u; slot++) {\n        const uint32_t reg = operand_register(word1, slot);",
        "the G operand of the final combiner is not checked against the legal sources.",
    ),
    _row(
        "analyse-ef-f-operand",
        "if (operand_register(word1, 0) == REG_PROD || operand_register(word1, 1) == REG_PROD) {",
        "if (operand_register(word1, 0) == REG_PROD) {",
        "F reading the E times F product is accepted.",
    ),
    _row(
        "analyse-defined-not-carried",
        "            an->defined[reg] |= writes[reg];",
        "            (void)writes[reg];",
        "a register written by stage 0 and read by stage 1 is treated as undefined, so every "
        "multi-stage combiner is flagged INFERRED (initial state).",
    ),
    _row(
        "analyse-colour-input-stage",
        "half_reads(an, stage, true, w[IDX_COLOR_ICW + stage],",
        "half_reads(an, stage, true, w[IDX_COLOR_ICW],",
        "every stage reads stage 0's colour inputs.",
    ),
    _row(
        "analyse-alpha-output-stage",
        "result = half_reads(an, stage, false, w[stage], w[IDX_ALPHA_OCW + stage], control, writes);",
        "result = half_reads(an, stage, false, w[stage], w[IDX_ALPHA_OCW], control, writes);",
        "every stage's alpha half uses stage 0's output word.",
    ),
    # ------------------------------------------------------------------ constants
    _row(
        "constant-green",
        "out[1] = factor_byte(word, 8u);  /* G */",
        "out[1] = factor_byte(word, 0u);  /* G */",
        "the green factor is read from the blue byte.",
    ),
    _row(
        "constant-blue",
        "out[2] = factor_byte(word, 0u);  /* B */",
        "out[2] = factor_byte(word, 8u);  /* B */",
        "the blue factor is read from the green byte.",
    ),
    _row(
        "constant-scale",
        "return (float)((word >> shift) & 0xFFu) / 255.0f;",
        "return (float)((word >> shift) & 0xFFu) / 256.0f;",
        "a full byte maps to just under 1.0.",
    ),
    # ------------------------------------------------------------------ plan_build entry and NEED
    _row(
        "plan-null-textures-allowed",
        "if (state == NULL || plan == NULL || textures == NULL) {",
        "if (state == NULL || plan == NULL) {",
        "a NULL texture array dereferences NULL instead of being refused.",
    ),
    _row(
        "plan-captured-required-off",
        "if (!state->combiner_captured) {",
        "if (!state->combiner_captured && 0) {",
        "a model that never decoded combiner methods is planned from an all-zero shadow instead "
        "of refused.",
    ),
    _row(
        "plan-stage-count-eight",
        "if (count < 1u || count > 8u) {",
        "if (count < 1u || count >= 8u) {",
        "an eight stage combiner, the maximum, is refused.",
    ),
    _row(
        "plan-stage-count-zero",
        "if (count < 1u || count > 8u) {",
        "if (count > 8u) {",
        "a combiner with zero stages is planned.",
    ),
    _row(
        "plan-need-alpha-input",
        '        NEED(stage, "an alpha input word");',
        "        (void)0;",
        "an unwritten alpha input word is not inferred, so no INFERRED bit is raised for it.",
    ),
    _row(
        "plan-need-alpha-output",
        '        NEED(IDX_ALPHA_OCW + stage, "an alpha output word");',
        "        (void)0;",
        "an unwritten alpha output word is not inferred.",
    ),
    _row(
        "plan-need-colour-input",
        '        NEED(IDX_COLOR_ICW + stage, "a colour input word");',
        "        (void)0;",
        "an unwritten colour input word is not inferred.",
    ),
    _row(
        "plan-need-colour-output",
        '        NEED(IDX_COLOR_OCW + stage, "a colour output word");',
        "        (void)0;",
        "an unwritten colour output word is not inferred.",
    ),
    _row(
        "plan-need-final0",
        '    NEED(IDX_FINAL0, "final combiner word 0");',
        "    (void)0;",
        "an unwritten final combiner word 0 is not inferred.",
    ),
    _row(
        "plan-need-final1",
        '    NEED(IDX_FINAL1, "final combiner word 1");',
        "    (void)0;",
        "an unwritten final combiner word 1 is not inferred.",
    ),
    _row(
        "plan-need-stage-last",
        "    for (uint32_t stage = 0u; stage < count; stage++) {\n        NEED(stage, ",
        "    for (uint32_t stage = 0u; stage + 1u < count; stage++) {\n        NEED(stage, ",
        "the last stage's words are never required to have been written.",
    ),
    _row(
        "plan-factor-bank",
        "const uint32_t reads = bank == 0u ? an.factor0_reads : an.factor1_reads;",
        "const uint32_t reads = bank == 0u ? an.factor1_reads : an.factor0_reads;",
        "the factor0 and factor1 reads are swapped, so the wrong constants are required.",
    ),
    _row(
        "plan-factor-last",
        "        for (uint32_t k = 0u; k < 8u; k++) {\n            if ((reads >> k) & 1u) {",
        "        for (uint32_t k = 0u; k < 7u; k++) {\n            if ((reads >> k) & 1u) {",
        "the eighth stage's constant is never required.",
    ),
    _row(
        "plan-factor-index",
        "NEED((bank == 0u ? IDX_FACTOR0 : IDX_FACTOR1) + k,",
        "NEED((bank == 0u ? IDX_FACTOR1 : IDX_FACTOR0) + k,",
        "the required factor word is looked up in the other bank.",
    ),
    _row(
        "plan-final-c0",
        "    if (an.final_c0_read) {\n        NEED(IDX_FINAL_C0,",
        "    if (an.final_c0_read && 0) {\n        NEED(IDX_FINAL_C0,",
        "an unwritten final constant 0 is not inferred.",
    ),
    _row(
        "plan-final-c1",
        "    if (an.final_c1_read) {\n        NEED(IDX_FINAL_C1,",
        "    if (an.final_c1_read && 0) {\n        NEED(IDX_FINAL_C1,",
        "an unwritten final constant 1 is not inferred.",
    ),
    # ------------------------------------------------------------------ textures
    _row(
        "texture-reads-shift",
        "uint32_t reads_texture = (an.reads >> 8) & 0xFu;",
        "uint32_t reads_texture = (an.reads >> 7) & 0xFu;",
        "texture register reads are taken from the wrong bits.",
    ),
    _row(
        "texture-mode-field",
        "program_written ? (w[IDX_STAGE_PROGRAM] >> (5u * stage)) & 0x1Fu :",
        "program_written ? (w[IDX_STAGE_PROGRAM] >> (4u * stage)) & 0x1Fu :",
        "stage programs are read at the wrong field stride.",
    ),
    _row(
        "texture-mode-bound",
        "if (kept[stage] > 1u && kept[stage] != TEX_CUBE &&",
        "if (kept[stage] > 2u && kept[stage] != TEX_CUBE &&",
        "program mode 2 (a cube map or a projective variant) is accepted and then planned as "
        "if it were 2D.",
    ),
    _row(
        "texture-mode-from-texture",
        ": textures[stage].rgba != NULL ? 1u : 0u;",
        ": 0u;",
        "with no stage program written, a supplied test texture is not used.",
    ),
    _row(
        "texture-r0-alpha-mode",
        "        if (modes[0] != 0u) {\n            reads_texture |= 1u; /* spare0 alpha starts as t0.a */",
        "        if (modes[0] == 0u) {\n            reads_texture |= 1u; /* spare0 alpha starts as t0.a */",
        "the start value of spare0 alpha is t0.a only when stage 0 has NO texture.",
    ),
    _row(
        "texture-r0-alpha-register",
        "reads_texture |= 1u; /* spare0 alpha starts as t0.a */",
        "reads_texture |= 2u; /* spare0 alpha starts as t0.a */",
        "the start value of spare0 alpha is taken from t1.",
    ),
    _row(
        "texture-r0-alpha-inferred",
        "            an.initial_state_read = true; /* and starts as 1.0, INFERRED */",
        "            (void)0; /* and starts as 1.0, INFERRED */",
        "the 1.0 start value of spare0 alpha with no texture is used with no inference named.",
    ),
    _row(
        "texture-stage-program-inference",
        "if (reads_texture != 0u && !program_written) {\n        inferences |= GPU_COMBINER_INFER_STAGE_PROGRAM;",
        "if (reads_texture != 0u && !program_written && 0) {\n        inferences |= GPU_COMBINER_INFER_STAGE_PROGRAM;",
        "taking a never-written stage program from the test textures is not flagged INFERRED.",
    ),
    _row(
        "texture-unprogrammed-with-program",
        "stage keeps its bit clear. xemu-level. */\n            continue;",
        "stage keeps its bit clear. xemu-level. */\n            (void)0;",
        "a stage whose written program is 'none' is sampled (or refused for want of a texture) instead of reading (0,0,0,1) (T1207).",
    ),
    _row(
        "texture-unprogrammed-no-texture",
        "        if (kept[stage] == 0u && !program_written) {",
        "        if (kept[stage] == 0u && !program_written && 0) {",
        "the 'no test texture' refusal is replaced by the later, differently worded one.",
    ),
    _row(
        "texture-width-bound",
        "if (texture->width == 0u || texture->height == 0u || texture->width > 4096u ||",
        "if (texture->width == 0u || texture->height == 0u || texture->width >= 4096u ||",
        "a 4096 wide test texture, the largest legal, is refused.",
    ),
    _row(
        "texture-height-bound",
        'texture->height > 4096u) {\n            return refuse(&an, "the test texture of stage',
        'texture->height >= 4096u) {\n            return refuse(&an, "the test texture of stage',
        "a 4096 high test texture, the largest legal, is refused.",
    ),
    _row(
        "texture-zero-size",
        "if (texture->width == 0u || texture->height == 0u || texture->width > 4096u ||",
        "if (texture->height == 0u || texture->width > 4096u ||",
        "a zero-width test texture is accepted.",
    ),
    _row(
        "texture-effective-program",
        "        effective_program |= kept[stage] << (5u * stage);\n        plan->texture_stages |= 1u << stage;",
        "        effective_program |= kept[stage] << (4u * stage);\n        plan->texture_stages |= 1u << stage;",
        "the program that enters the key is packed at the wrong stride.",
    ),
    _row(
        "texture-stage-mask",
        "plan->texture_stages |= 1u << stage;",
        "plan->texture_stages |= 1u;",
        "only stage 0's texture is ever bound.",
    ),
    _row(
        "texture-sampling-inference",
        "        inferences |= GPU_COMBINER_INFER_TEXTURE_SAMPLING;",
        "        (void)0;",
        "sampling a test texture (nearest, clamped, row 0 on top) is used with no inference named.",
    ),
    _row(
        "texture-effective-program-kept",
        "    w[IDX_STAGE_PROGRAM] = effective_program;",
        "    (void)effective_program;",
        "the written stage program enters the key instead of the effective one, so stages that "
        "read no texture split the module name.",
    ),
    # ------------------------------------------------------------------ inferences
    _row(
        "inference-constant-bytes-c1",
        "if (an.factor0_reads != 0u || an.factor1_reads != 0u || an.final_c0_read || an.final_c1_read) {",
        "if (an.factor0_reads != 0u || an.factor1_reads != 0u || an.final_c0_read) {",
        "final constant 1 read as ARGB bytes is not flagged INFERRED.",
    ),
    _row(
        "inference-constant-bytes-factor1",
        "if (an.factor0_reads != 0u || an.factor1_reads != 0u || an.final_c0_read || an.final_c1_read) {",
        "if (an.factor0_reads != 0u || an.final_c0_read || an.final_c1_read) {",
        "factor1 read as ARGB bytes is not flagged INFERRED.",
    ),
    _row(
        "inference-colour-range-v1",
        "if ((an.reads & ((1u << REG_V0) | (1u << REG_V1))) != 0u) {",
        "if ((an.reads & (1u << REG_V0)) != 0u) {",
        "reading oD1 unclamped is not flagged INFERRED.",
    ),
    _row(
        "inference-colour-range-v0",
        "if ((an.reads & ((1u << REG_V0) | (1u << REG_V1))) != 0u) {",
        "if ((an.reads & (1u << REG_V1)) != 0u) {",
        "reading oD0 unclamped is not flagged INFERRED.",
    ),
    _row(
        "inference-one-always-allowed",
        "const uint32_t refused = inferences & ~allowed;",
        "const uint32_t refused = inferences & ~(allowed | GPU_COMBINER_INFER_COLOUR_RANGE);",
        "one INFERRED rule is applied whether or not the caller allowed it.",
    ),
    _row(
        "inference-unwritten-detail",
        "if (names[i].bit == GPU_COMBINER_INFER_UNWRITTEN && unwritten_first != NULL) {",
        "if (names[i].bit == GPU_COMBINER_INFER_UNWRITTEN && unwritten_first != NULL && 0) {",
        "the refusal for an unwritten word stops naming WHICH word was never written.",
    ),
    # ------------------------------------------------------------------ the key
    _row(
        "key-alpha-output-mask",
        "        canonical[IDX_ALPHA_OCW + stage] &= OUTPUT_KEY_MASK;",
        "        (void)0;",
        "ignored bits of an alpha output word leak into the module name.",
    ),
    _row(
        "key-colour-output-mask",
        "        canonical[IDX_COLOR_OCW + stage] &= OUTPUT_KEY_MASK;",
        "        (void)0;",
        "ignored bits of a colour output word leak into the module name.",
    ),
    _row(
        "key-final1-mask",
        "    canonical[IDX_FINAL1] &= FINAL1_KEY_MASK;",
        "    (void)0;",
        "ignored bits of final word 1 leak into the module name.",
    ),
    _row(
        "key-control-mask",
        "    canonical[IDX_CONTROL] &= CONTROL_KEY_MASK;",
        "    (void)0;",
        "ignored bits of the control word leak into the module name.",
    ),
    _row(
        "key-unused-alpha-input",
        "        canonical[stage] = 0u;\n        canonical[IDX_ALPHA_OCW + stage] = 0u;",
        "        canonical[IDX_ALPHA_OCW + stage] = 0u;",
        "an unused stage's alpha input word stays in the key.",
    ),
    _row(
        "key-unused-colour-input",
        "        canonical[IDX_COLOR_ICW + stage] = 0u;",
        "        (void)0;",
        "an unused stage's colour input word stays in the key.",
    ),
    _row(
        "key-lookup",
        "plan->key[i] = canonical[key_indices[i]];",
        "plan->key[i] = canonical[i];",
        "the key is the first 36 words, not the 36 named ones.",
    ),
    # ------------------------------------------------------------------ plan output
    _row(
        "plan-factor1-slot",
        "unpack_constant(w[IDX_FACTOR1 + k], plan->constants + (8u + k) * 4u);",
        "unpack_constant(w[IDX_FACTOR1 + k], plan->constants + (7u + k) * 4u);",
        "factor1 constants land one slot early, on top of the previous bank's last constant.",
    ),
    _row(
        "plan-final-c0-slot",
        "unpack_constant(w[IDX_FINAL_C0], plan->constants + 16u * 4u);",
        "unpack_constant(w[IDX_FINAL_C0], plan->constants + 15u * 4u);",
        "final constant 0 lands on factor1's last slot.",
    ),
    _row(
        "plan-final-c1-slot",
        "unpack_constant(w[IDX_FINAL_C1], plan->constants + 17u * 4u);",
        "unpack_constant(w[IDX_FINAL_C1], plan->constants + 16u * 4u);",
        "final constant 1 lands on final constant 0.",
    ),
    _row(
        "plan-stage-count",
        "    plan->stage_count = count;",
        "    plan->stage_count = count + 1u;",
        "the reported stage count is off by one.",
    ),
    _row(
        "plan-reads",
        "    plan->reads = an.reads;",
        "    plan->reads = 0u;",
        "the register read mask is not reported, so the vertex-output check has nothing to "
        "compare.",
    ),
    _row(
        "plan-used-inferences",
        "    plan->used_inferences = inferences & ~GPU_COMBINER_INFER_TEXTURE_MODES;",
        "    plan->used_inferences = 0u;",
        "a plan that rested on inferences reports none.",
    ),
    # ------------------------------------------------------------------ SPIR-V interface scan
    _row(
        "spirv-magic",
        "word_count < 5u || words[0] != SPV_MAGIC) {",
        "word_count < 5u) {",
        "a buffer without the SPIR-V magic is scanned.",
    ),
    _row(
        "spirv-bound",
        "if (bound == 0u || bound > SPV_MAX_IDS) {",
        "if (bound > SPV_MAX_IDS) {",
        "a module with an id bound of 0 is scanned.",
    ),
    _row(
        "spirv-instruction-overrun",
        "if (length == 0u || at + length > word_count) {",
        "if (length == 0u || at + length > word_count + 1u) {",
        "an instruction that runs one word past the end of the module is accepted.",
    ),
    _row(
        "spirv-zero-length",
        "if (length == 0u || at + length > word_count) {",
        "if (at + length > word_count) {",
        "a zero-length instruction loops forever.",
    ),
    _row(
        "spirv-first-entry-point",
        "if (opcode == SPV_OP_ENTRY_POINT && interface_ids == NULL) {",
        "if (opcode == SPV_OP_ENTRY_POINT) {",
        "the LAST entry point's interface is used instead of the first.",
    ),
    _row(
        "spirv-entry-name-offset",
        "size_t name_at = at + 3u;",
        "size_t name_at = at + 2u;",
        "the entry point name is read from the wrong word, so the interface list is misplaced.",
    ),
    _row(
        "spirv-variable-class",
        "is_class = words[at + 3u] == storage_class;",
        "is_class = words[at + 3u] != storage_class;",
        "Input and Output variables are swapped.",
    ),
    _row(
        "spirv-decoration-target",
        "if (opcode == SPV_OP_DECORATE && words[at + 1u] == id &&\n                words[at + 2u] == SPV_DECORATION_LOCATION) {",
        "if (opcode == SPV_OP_DECORATE &&\n                words[at + 2u] == SPV_DECORATION_LOCATION) {",
        "every Location decoration counts for every variable.",
    ),
    _row(
        "spirv-mask-accumulates",
        "*out_mask |= 1u << words[at + 3u];",
        "*out_mask = 1u << words[at + 3u];",
        "only the last location of the interface is reported.",
    ),
    _row(
        "spirv-location-bound",
        "if (words[at + 3u] >= 32u) {",
        "if (words[at + 3u] >= 31u) {",
        "location 31, the last representable in the mask, is refused.",
    ),
]


# RECORD of a run with no Vulkan device (`VK_DRIVER_FILES=/nonexistent`, 2026-10-03): the mutations whose
# every target exits 77 there, so the harness reports them SKIPPED and never counts them killed. It is
# a record, not a mechanism: the harness decides from the exit code and prints a NOTE when a skip is
# not listed here. Regenerate it with that command if a test starts or stops needing a device.
_NEEDS_DEVICE = frozenset({})
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
