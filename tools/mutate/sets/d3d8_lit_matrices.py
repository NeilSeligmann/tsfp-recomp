# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for T533 (b), the lit and blended matrices of 0x003DEB80 (src/gpu/d3d8_dirty.c) and the 4x4
inverse 0x003D9DB0 under them (src/gpu/d3d8_matrix_inverse.c).

Every mutation is covered by `tests/test_d3d8_lit_matrices_oracle.py`, which replays the ORIGINAL bytes under
the oracle, the ctest binary beside it is the compile guard (a mutation that does not compile would make the
pytest runner build fail, which reads like a kill).

EQUIVALENT MUTANTS DROPPED (a survivor that cannot be killed is not evidence): the arithmetic shift of the
reciprocal square root seed (`(int32_t)(0xBE800000u - x) >> 1`: x is the float of a determinant squared, never
above 0x7F800000, so the difference is never negative and the logical shift gives the same), the magnitude mask
of the refined estimate (the refinement is positive for every seed the integer trick produces), the byte count
of the LAST planned site (the blend loop's three packets: nothing is planned after it, so it only feeds a
mapping check the oracle's ring always satisfies), the NaN determinant test and the NaN scan of the input
words (a NaN word always reaches the determinant and nine cofactors, which the determinant and output scans
refuse with the same address, so the input scan only refuses earlier). The one-bit change of the estimate's
seed is equivalent too: two Newton steps wash out a seed error of 1e-7, so the mutant uses a seed that is
wrong by 1e-3.

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror` turns
the latter into NOT-A-MUTANT, which reads like evidence.
"""

INVERSE = "src/gpu/d3d8_matrix_inverse.c"
DIRTY = "src/gpu/d3d8_dirty.c"
ORACLE = "pytest:tests/test_d3d8_lit_matrices_oracle.py"
INVERSE_TESTS = f'{ORACLE} -k "inverse or special or nan or overflowing or singular_test"'
MATRIX_TESTS = f'{ORACLE} -k "test_the_matrices_match or singular or cascade"'
ROLL_TESTS = f'{ORACLE} -k "rolls_over or flag_4"'


def mutation(
    identifier: str, file: str, old: str, new: str, why: str, tests: list[str] | None = None
) -> dict:
    return {
        "id": f"t533-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": ["test_d3d8_dirty", *(tests or [ORACLE])],
        "why": why,
    }


def inverse(identifier: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, INVERSE, old, new, why, [INVERSE_TESTS])


def matrices(identifier: str, old: str, new: str, why: str, tests: list[str] | None = None) -> dict:
    return mutation(identifier, DIRTY, old, new, why, tests or [MATRIX_TESTS])


MUTATIONS: list[dict] = [
    # --- the cofactor sequence, the original's instructions ---------------------------------------------------
    inverse(
        "inv-asm-input-load",
        '"fld dword ptr [rax + 4]\\n\\t" /* 3D9DBC */',
        '"fld dword ptr [rax + 8]\\n\\t" /* 3D9DBC */',
        "the second row-0 word is the second input.",
    ),
    inverse(
        "inv-asm-first-subtraction",
        '".byte 0xde,0xe9\\n\\t" /* 3D9DE7 fsubp st(1) */',
        '".byte 0xde,0xe1\\n\\t" /* 3D9DE7 fsubp st(1) */',
        "the first cofactor subtracts st(0) from st(1), not the reverse.",
    ),
    inverse(
        "inv-asm-operand",
        '"fmul dword ptr [rbx + 0x24]\\n\\t" /* 3D9E42 */',
        '"fmul dword ptr [rbx + 0x20]\\n\\t" /* 3D9E42 */',
        "a product of the second cofactor group reads the right frame word.",
    ),
    inverse(
        "inv-asm-late-operand",
        '"fmul dword ptr [rbx]\\n\\t" /* 3D9FC4 */',
        '"fmul dword ptr [rbx + 4]\\n\\t" /* 3D9FC4 */',
        "the last cofactor group reads the right frame word.",
    ),
    inverse(
        "inv-asm-determinant-subtraction",
        '".byte 0xde,0xe9\\n\\t" /* 3DA005 fsubp st(1) */',
        '".byte 0xde,0xc1\\n\\t" /* 3DA005 fsubp st(1) */',
        "the cofactors of the last group subtract.",
    ),
    inverse(
        "inv-asm-determinant-term",
        '"fmul dword ptr [rbx + 0x30]\\n\\t" /* 3DA0F5 */',
        '"fmul dword ptr [rbx + 0x34]\\n\\t" /* 3DA0F5 */',
        "the determinant sums the four row 0 words with their cofactors.",
    ),
    inverse(
        "inv-asm-determinant-store",
        '"fstp dword ptr [rbx + 0x5c]\\n\\t" /* 3DA0FB */',
        '"fstp dword ptr [rbx + 0x58]\\n\\t" /* 3DA0FB */',
        "the determinant is the float at frame word 0x5C.",
    ),
    # --- the stores and the scale --------------------------------------------------------------------------------
    inverse(
        "inv-store-order",
        "    {0x1Cu, 0u},  {0x18u, 4u},",
        "    {0x1Cu, 4u},  {0x18u, 0u},",
        "the first two cofactors land at output words 0 and 4.",
    ),
    inverse(
        "inv-store-order-last",
        "{0x4Cu, 11u}, {0x50u, 15u},",
        "{0x4Cu, 15u}, {0x50u, 11u},",
        "the last two cofactors land at output words 11 and 15.",
    ),
    inverse(
        "inv-singular-test",
        "    if (word_float(determinant) == word_float(zero)) {",
        "    if (word_float(determinant) != word_float(zero)) {",
        "a zero determinant returns -1 before any store.",
    ),
    inverse(
        "inv-singular-constant",
        "    if (word_float(determinant) == word_float(zero)) {",
        "    if (word_float(determinant) == 0.0f + 0.0f * word_float(zero)) {",
        "the zero is the float constant at 0x475CAC (read from guest memory, a changed constant moves the test).",
    ),
    inverse(
        "inv-normalize-branch",
        "    if (normalize) {\n        /* 0x003DA129",
        "    if (!normalize) {\n        /* 0x003DA129",
        "a non-zero flag scales by the reciprocal estimate, zero only corrects the sign.",
    ),
    inverse(
        "inv-scale-sign",
        "        const uint32_t scale = (determinant & 0x80000000u) | float_word((float)root);",
        "        const uint32_t scale = float_word((float)root);",
        "the scale carries the sign of the determinant.",
    ),
    inverse(
        "inv-unscaled-sign",
        "                frame[COFACTOR_STORES[index].frame / 4u] ^ (determinant & 0x80000000u);",
        "                frame[COFACTOR_STORES[index].frame / 4u];",
        "the unscaled path flips the sign of every cofactor when the determinant is negative.",
    ),
    inverse(
        "inv-determinant-square",
        "        const float squared = (float)((long double)word_float(determinant) * word_float(determinant));",
        "        const float squared = (float)((long double)word_float(determinant));",
        "the estimate is of the determinant squared.",
    ),
    inverse(
        "inv-scale-rounded",
        "                float_word((float)((long double)word_float(scale) * word_float(frame[COFACTOR_STORES[index].frame / 4u])));",
        "                float_word(word_float(scale)) * 0u + frame[COFACTOR_STORES[index].frame / 4u];",
        "every scaled output is the product of the scale and the cofactor.",
    ),
    # --- the reciprocal square root 0x003D9D00 ---------------------------------------------------------------------
    inverse(
        "rsqrt-seed",
        "    const uint32_t seed = (uint32_t)((int32_t)(0xBE800000u - x_word) >> 1);",
        "    const uint32_t seed = (uint32_t)((int32_t)(0xBE880000u - x_word) >> 1);",
        "the integer seed is (0xBE800000 - bits) / 2.",
    ),
    inverse(
        "rsqrt-seed-shift",
        "    const uint32_t seed = (uint32_t)((int32_t)(0xBE800000u - x_word) >> 1);",
        "    const uint32_t seed = (uint32_t)((int32_t)(0xBE800000u - x_word) >> 2);",
        "the seed is halved.",
    ),
    inverse(
        "rsqrt-newton-sign",
        "    const long double refined = ((long double)word_float(offset) - squared * scaled) * y0;",
        "    const long double refined = ((long double)word_float(offset) + squared * scaled) * y0;",
        "the first step subtracts.",
    ),
    inverse(
        "rsqrt-newton-scale",
        "    const long double scaled = (long double)word_float(scale) * x;",
        "    const long double scaled = (long double)word_float(offset) * x + 0.0L * word_float(scale);",
        "the first step scales by the constant at 0x549720.",
    ),
    inverse(
        "rsqrt-refine-three",
        "    return (((long double)word_float(three) - product) * magnitude) * word_float(half);",
        "    return (((long double)word_float(three) + product) * magnitude) * word_float(half);",
        "the second step subtracts from the constant 3.0.",
    ),
    inverse(
        "rsqrt-refine-half",
        "    return (((long double)word_float(three) - product) * magnitude) * word_float(half);",
        "    return (((long double)word_float(three) - product) * magnitude) + 0.0L * word_float(half);",
        "the second step halves.",
    ),
    # --- the refusals ----------------------------------------------------------------------------------------------
    inverse(
        "inv-nan-output",
        "        if (nan_class(result[index])) {",
        "        if (nan_class(result[index]) && false) {",
        "a NaN output is refused before it is stored.",
    ),
    inverse(
        "inv-nan-constant",
        '    if (nan_class(word)) {\n        d3d8_hle_fatal(ENTRY, "float constant',
        '    if (nan_class(word) && false) {\n        d3d8_hle_fatal(ENTRY, "float constant',
        "a NaN float constant of the original's data is refused.",
    ),
    inverse(
        "inv-writes-on-singular",
        "    if (!d3d8_matrix_inverse_compute(matrix, flag != 0u, result)) {\n        return 0xFFFFFFFFu;",
        "    if (!d3d8_matrix_inverse_compute(matrix, flag != 0u, result)) {\n        (void)kernel_guest_write_bytes(out, result, sizeof(result));\n        return 0xFFFFFFFFu;",
        "a singular matrix leaves the output untouched.",
    ),
    inverse(
        "inv-flag",
        "    if (!d3d8_matrix_inverse_compute(matrix, flag != 0u, result)) {",
        "    if (!d3d8_matrix_inverse_compute(matrix, flag == 0u, result)) {",
        "the flag argument selects the scaled form.",
    ),
    # --- 0x003DEB80 --------------------------------------------------------------------------------------------------
    matrices(
        "lit-condition-wrap",
        "    plan->lit = d3d8_device_load32(0x950u) != 0u || d3d8_guest_load32(GLOBAL_LIGHTING_STATE) != 0u;",
        "    plan->lit = d3d8_device_load32(0x950u) != 0u && d3d8_guest_load32(GLOBAL_LIGHTING_STATE) != 0u;",
        "either render state 0x66 or a wrapped texture stage makes the matrices lit.",
    ),
    matrices(
        "lit-condition-state",
        "    plan->lit = d3d8_device_load32(0x950u) != 0u || d3d8_guest_load32(GLOBAL_LIGHTING_STATE) != 0u;",
        "    plan->lit = d3d8_device_load32(0x950u) != 0u;",
        "render state 0x66 (0x003E3E58) makes the matrices lit.",
    ),
    matrices(
        "blend-condition",
        "    plan->blend = d3d8_guest_load32(GLOBAL_BLENDING) != 0u;",
        "    plan->blend = d3d8_guest_load32(GLOBAL_BLENDING) == 0u;",
        "vertex blending (0x003E3EE4) loops over the blend matrices.",
    ),
    matrices(
        "normalize-flag",
        "    if (!d3d8_matrix_inverse_compute(words, d3d8_guest_load32(GLOBAL_NORMALIZE) == 0u, output)) {",
        "    if (!d3d8_matrix_inverse_compute(words, d3d8_guest_load32(GLOBAL_NORMALIZE) != 0u, output)) {",
        "the inverse's flag is [0x003E3EF8] == 0.",
    ),
    matrices(
        "singular-refusal",
        "    if (!d3d8_matrix_inverse_compute(words, d3d8_guest_load32(GLOBAL_NORMALIZE) == 0u, output)) {\n        d3d8_hle_fatal",
        "    if (!d3d8_matrix_inverse_compute(words, d3d8_guest_load32(GLOBAL_NORMALIZE) == 0u, output) && false) {\n        d3d8_hle_fatal",
        "a singular matrix in the lit path is refused (the original copies uninitialised stack).",
    ),
    matrices(
        "lit-packet-header",
        "#define PACKET_LIT 0x00300580u     /* 12 dwords */",
        "#define PACKET_LIT 0x00300581u     /* 12 dwords */",
        "the lit packet is method 0x580 with 12 dwords.",
    ),
    matrices(
        "lit-blend-header",
        "#define PACKET_LIT_BLEND 0x003005C0u",
        "#define PACKET_LIT_BLEND 0x003005C1u",
        "the blended lit packets start at method 0x5C0.",
    ),
    matrices(
        "blend-header",
        "#define PACKET_BLEND 0x004004C0u",
        "#define PACKET_BLEND 0x004004C4u",
        "the blended packets start at method 0x4C0.",
    ),
    matrices(
        "blend-count",
        "#define BLEND_MATRICES 3u ",
        "#define BLEND_MATRICES 4u ",
        "the blend loop runs three times (edi from 0x4004C0 in steps of 0x40 while below 0x400580).",
    ),
    matrices(
        "blend-source",
        "#define MATRIX_BLEND 0xE20u ",
        "#define MATRIX_BLEND 0xE60u ",
        "the blend matrices start at device+0xE20.",
    ),
    matrices(
        "blend-stride",
        "        cursor = store_packet_at(cursor, PACKET_BLEND + index * 0x40u, plan.blended[index]);",
        "        cursor = store_packet_at(cursor, PACKET_BLEND + index * 0x44u, plan.blended[index]);",
        "each blend packet is 0x40 methods after the last.",
    ),
    matrices(
        "lit-blend-stride",
        "        if (plan.lit) cursor = store_lit_packet(cursor, PACKET_LIT_BLEND + index * 0x40u, plan.lit_blended[index]);",
        "        if (plan.lit) cursor = store_lit_packet(cursor, PACKET_LIT_BLEND + index * 0x44u, plan.lit_blended[index]);",
        "each lit blend packet is 0x40 methods after the last.",
    ),
    matrices(
        "lit-words",
        "    for (uint32_t index = 0u; index < 12u; index++) d3d8_guest_store32(cursor + 4u + index * 4u, words[index]);",
        "    for (uint32_t index = 0u; index < 11u; index++) d3d8_guest_store32(cursor + 4u + index * 4u, words[index]);",
        "the lit packet carries 12 dwords.",
    ),
    matrices(
        "lit-packet-bytes",
        "#define LIT_PACKET_BYTES 52u",
        "#define LIT_PACKET_BYTES 48u",
        "the lit packet is 52 bytes.",
    ),
    matrices(
        "blend-viewport-packet",
        "    d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.viewport));",
        "    d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.second));",
        "with blending the viewport packet is the viewport matrix itself.",
    ),
    matrices(
        "plain-viewport-packet",
        "        d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.second));",
        "        d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.viewport));",
        "without blending the viewport packet is the product with the viewport matrix.",
    ),
    matrices(
        "blend-second-preamble",
        "    d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.viewport));\n    cursor = d3d8_pushbuffer_begin();",
        "    d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.viewport));\n    cursor += MATRIX_PACKET_BYTES;",
        "the blend loop opens with its own reservation preamble.",
        [ROLL_TESTS],
    ),
    matrices(
        "first-site-lit-bytes",
        "    d3d8_pushbuffer_sim_site(sim, 0x003DEB80u, 2u * MATRIX_PACKET_BYTES + lit_bytes);",
        "    d3d8_pushbuffer_sim_site(sim, 0x003DEB80u, 2u * MATRIX_PACKET_BYTES);",
        "the entry reservation covers the lit packet.",
        [ROLL_TESTS],
    ),
    matrices(
        "plan-blend-site",
        "    if (plan->blend) {\n        d3d8_pushbuffer_sim_site(sim, 0x003DEB80u, BLEND_MATRICES",
        "    if (plan->blend && false) {\n        d3d8_pushbuffer_sim_site(sim, 0x003DEB80u, BLEND_MATRICES",
        "the second preamble of the blended path is a site of the plan.",
        [ROLL_TESTS],
    ),
    matrices(
        "emit-plans-first",
        "    plan_matrix_sites(&plan, &sim);\n    uint32_t cursor = d3d8_pushbuffer_begin();",
        "    (void)sim;\n    uint32_t cursor = d3d8_pushbuffer_begin();",
        "a roll-over device flag 4 cannot take refuses before the first packet of the blended path.",
        [ROLL_TESTS],
    ),
    matrices(
        "blend-lit-dropped",
        "        if (plan->lit) lit_words(plan->blended[index], plan->lit_blended[index]);",
        "        if (plan->lit && false) lit_words(plan->blended[index], plan->lit_blended[index]);",
        "every blended matrix has its inverse packet when lit.",
    ),
    matrices(
        "blend-uses-projection",
        "        matrix_multiply(plan->blended[index], blend, projection);",
        "        matrix_multiply(plan->blended[index], projection, blend);",
        "each blended packet is the blend matrix times the projection.",
    ),
    matrices(
        "lit-first-source",
        "    if (plan->lit) lit_words(plan->first, plan->lit_first);",
        "    if (plan->lit) lit_words(plan->second, plan->lit_first);",
        "the lit packet inverts the world-view-projection product.",
    ),
    matrices(
        "lit-before-viewport",
        "    if (plan.lit) cursor = store_lit_packet(cursor, PACKET_LIT, plan.lit_first);\n    if (!plan.blend) {",
        "    if (!plan.blend) {",
        "the lit packet follows the first product.",
    ),
]
