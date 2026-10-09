# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for T461, the named refusals T443 left: SetPixelShader's roll-over sites
(src/gpu/d3d8_pixel_bind.c, T525 and T526 landed that port first and T461's tests pin it again),
the emitting library variants of render states 0x89 to 0xA5 (src/gpu/d3d8_state.c,
d3d8_set_state.c), the BumpEnv, BorderColor and ColorKeyColor stage states,
the texture transform shapes of 0x003DE080 (src/gpu/d3d8_dirty.c) and the read-only twin of the
viewport matrix rebuild (src/gpu/d3d8_viewport_matrix.c).

Every mutation is covered by a pytest target that replays the ORIGINAL bytes under the oracle, the
ctest binary beside it is the compile guard (a mutation that does not compile would make the pytest
runner build fail, which reads like a kill).

EQUIVALENT MUTANTS DROPPED (a survivor that cannot be killed is not evidence): the first site of state 0x8F
planned as `sim.cursor += 16` (the first `begin()` is the first thing the apply does, so a roll-over it
cannot take refuses before any write either way, and the plan's cursor only feeds the second site's
mapping check), the second site's byte count of state 0x8F (plan only, it matters when the span runs off
mapped memory, which the oracle's ring never does) and the helper plan of state 0x98 (its sized reservation
rolls only when the cursor is 200 bytes past the limit, where the plain `begin()` preamble the plan also
has already rolled and refuses). The pixel binder's probe of every planned span (it fails only on a protected
mapping, which the oracle's RAM cannot model) is equivalent too. The same argument drops the helper plan's own `sim_reserve` call, and the helper's flag test in
`vertex_program_helper_dwords` (the emitter re-tests flag 2 itself, so a wrong test only adds a no-op reservation). The NaN
test of `truncate_to_integer` is equivalent on x86, where the compiler's cast IS `cvttss2si`.

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror` turns
the latter into NOT-A-MUTANT, which reads like evidence.
"""

PIXEL_BIND = "src/gpu/d3d8_pixel_bind.c"
STATE = "src/gpu/d3d8_state.c"
SET_STATE = "src/gpu/d3d8_set_state.c"
DIRTY = "src/gpu/d3d8_dirty.c"
VIEWPORT_MATRIX = "src/gpu/d3d8_viewport_matrix.c"
ORACLE_PIXEL = "pytest:tests/test_d3d8_pixel_shader_refill_oracle.py"
ORACLE_HELPERS = "pytest:tests/test_d3d8_library_helpers_oracle.py"
ORACLE_SET_STATE = "pytest:tests/test_d3d8_set_state_oracle.py"
ORACLE_TRANSFORMS = "pytest:tests/test_d3d8_texture_transform_oracle.py"


def mutation(identifier: str, file: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"t461-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets,
        "why": why,
    }


# One selection per test family keeps a survivor from costing the whole file: the cases that can see
# the mutation, ordered fast to slow.
PIXEL_PLAN = f'{ORACLE_PIXEL} -k "ring_end or ring_base or under_the_cursor or second_unbound or cannot_roll or before_the_roll"'
PIXEL_ROLLS = f"{ORACLE_PIXEL} -k rolls_over"
HELPERS_8F = f'{ORACLE_HELPERS} -k "state_8f or plans_the"'
HELPERS_9A = f"{ORACLE_HELPERS} -k state_9a"
HELPERS_CONTROL = f"{ORACLE_HELPERS} -k control_word"
HELPERS_REST = f'{ORACLE_HELPERS} -k "remaining or state_9f or state_98 or plans_the"'
HELPERS_PROGRAMMABLE = f'{ORACLE_HELPERS} -k "state_8f or programmable or plans_the"'
SET_STATE_EACH = f"{ORACLE_SET_STATE} -k each_helper"
SET_STATE_DISPATCH = f'{ORACLE_SET_STATE} -k "each_helper or dispatcher"'
SET_STATE_STAGE = f'{ORACLE_SET_STATE} -k "bump_border or stage"'


def pixel(identifier: str, old: str, new: str, why: str, rolls: bool = False) -> dict:
    selection = [PIXEL_PLAN, PIXEL_ROLLS] if rolls else [PIXEL_PLAN]
    return mutation(identifier, PIXEL_BIND, old, new, why, ["test_d3d8_pixel_bind", *selection])


def helper(
    identifier: str, old: str, new: str, why: str, file: str = STATE, tests: list[str] | None = None
) -> dict:
    return mutation(
        identifier, file, old, new, why, ["test_d3d8_state", *(tests or [HELPERS_REST])]
    )


def transform(identifier: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, DIRTY, old, new, why, ["test_d3d8_dirty", ORACLE_TRANSFORMS])


MUTATIONS: list[dict] = [
    # --- SetPixelShader at the pushbuffer limit (the port is T525/T526's, these pin T461's oracle tests) ---
    pixel(
        "pixel-bound-program-bytes",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+(rows_needed ? 84u : 0u);",
        "if (argument) site_bytes[sites++]=244u+(inputs ? 12u : 0u)+(rows_needed ? 84u : 0u);",
        "the bound site is 240 program bytes, a wrong size moves where a ring end or alias refusal falls.",
    ),
    pixel(
        "pixel-bound-inputs-bytes",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+(rows_needed ? 84u : 0u);",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 8u : 0u)+(rows_needed ? 84u : 0u);",
        "the inputs packet is 12 bytes and only written when the inputs word is not zero.",
    ),
    pixel(
        "pixel-bound-rows-bytes",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+(rows_needed ? 84u : 0u);",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+(rows_needed ? 80u : 0u);",
        "enabling from the null binding writes three 28 byte texture state rows first.",
    ),
    pixel(
        "pixel-bound-rows-always",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+(rows_needed ? 84u : 0u);",
        "if (argument) site_bytes[sites++]=240u+(inputs ? 12u : 0u)+84u;",
        "a rebind from a bound shader writes no rows, the site is 84 bytes shorter.",
    ),
    pixel(
        "pixel-rows-needed-inverted",
        "const bool rows_needed=!argument || plan->previous==0u;",
        "const bool rows_needed=!argument || plan->previous!=0u;",
        "the rows are written when the previous binding is null (and always unbound), not when it is bound.",
        rolls=True,
    ),
    pixel(
        "pixel-unbound-first-site-bytes",
        "else { site_bytes[sites++]=68u; site_bytes[sites++]=92u; }",
        "else { site_bytes[sites++]=64u; site_bytes[sites++]=92u; }",
        "the texture factor packet is 68 bytes: 16 factors and a header.",
    ),
    pixel(
        "pixel-unbound-second-site-bytes",
        "else { site_bytes[sites++]=68u; site_bytes[sites++]=92u; }",
        "else { site_bytes[sites++]=68u; site_bytes[sites++]=88u; }",
        "the second site writes the 84 byte rows and the 8 byte stage 0 pair.",
    ),
    pixel(
        "pixel-unbound-second-site-dropped",
        "else { site_bytes[sites++]=68u; site_bytes[sites++]=92u; }",
        "else { site_bytes[sites++]=68u; }",
        "the unbound path has a SECOND reservation preamble, a limit reached between the two sites rolls there.",
    ),
    pixel(
        "pixel-branch-swapped",
        "if (argument) site_bytes[sites++]=240u",
        "if (!argument) site_bytes[sites++]=240u",
        "binding and unbinding have different site layouts.",
        rolls=True,
    ),
    pixel(
        "pixel-ring-end-check-dropped",
        "if (ring_end && ((uint64_t)start+site_bytes[site]>ring_end || start<ring_base || ring_end<=ring_base))",
        "if (ring_end && (((uint64_t)start+site_bytes[site]>ring_end && ring_end==0u) || start<ring_base || ring_end<=ring_base))",
        "a span past the ring end is refused (the original writes on, the port does not model it).",
    ),
    pixel(
        "pixel-ring-base-check-dropped",
        "if (ring_end && ((uint64_t)start+site_bytes[site]>ring_end || start<ring_base || ring_end<=ring_base))",
        "if (ring_end && ((uint64_t)start+site_bytes[site]>ring_end || (start<ring_base && ring_end<ring_base) || ring_end<=ring_base))",
        "a cursor below the ring base is an inconsistent state, refused before any write.",
    ),
    pixel(
        "pixel-definition-alias-dropped",
        "(argument && (overlaps(spans[span][0],spans[span][1],plan->definition,240u) ||",
        "(argument && (overlaps(spans[span][0],spans[span][1],plan->definition,0u) ||",
        "the original reads the program while writing the ring over it, which is not modelled.",
    ),
    pixel(
        "pixel-wrapper-alias-dropped",
        "overlaps(spans[span][0],spans[span][1],plan->wrapper,12u))))",
        "overlaps(spans[span][0],spans[span][1],plan->wrapper,0u))))",
        "the wrapper under the cursor is read mid-write, which is not modelled.",
    ),
    # --- the emitting library helpers ----------------------------------------------------------
    helper(
        "h91-value-dropped",
        "    d3d8_guest_store32(cursor + 12u, value);\n    d3d8_pushbuffer_end(cursor + 16u);\n}\n\nvoid d3d8_state_library_set_a1",
        "    d3d8_guest_store32(cursor + 12u, 0u);\n    d3d8_pushbuffer_end(cursor + 16u);\n}\n\nvoid d3d8_state_library_set_a1",
        "state 0x91 writes the value it was given at method 0x370.",
    ),
    helper(
        "h91-header",
        "    d3d8_guest_store32(cursor + 4u, cull_word());\n"
        "    d3d8_guest_store32(cursor + 8u, 0x00040370u);",
        "    d3d8_guest_store32(cursor + 4u, cull_word());\n"
        "    d3d8_guest_store32(cursor + 8u, 0x00040374u);",
        "the register the stencil function writes.",
    ),
    helper(
        "cull-a2-bit",
        "    uint32_t word = d3d8_guest_load32(D3D8_STATE_A2) != 0u ? 2u : 0u;\n    if (d3d8_guest_load32(D3D8_STATE_A1)",
        "    uint32_t word = d3d8_guest_load32(D3D8_STATE_A2) != 0u ? 1u : 0u;\n    if (d3d8_guest_load32(D3D8_STATE_A1)",
        "state 0xA2 is bit 1 of the cull word.",
    ),
    helper(
        "cull-91-compare",
        "d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {\n        word |= 1u;\n    }\n    return word;",
        "d3d8_guest_load32(D3D8_STATE_91) != 0x1E00u)) {\n        word |= 1u;\n    }\n    return word;",
        "bit 0 follows state 0x91 being 0x1E00 (KEEP) while stencil is on.",
    ),
    helper(
        "cull-90-compare",
        "    if (d3d8_guest_load32(D3D8_STATE_A1) != 0u &&\n        (d3d8_guest_load32(D3D8_STATE_90) == 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {\n        word |= 1u;",
        "    if (d3d8_guest_load32(D3D8_STATE_A1) != 0u &&\n        (d3d8_guest_load32(D3D8_STATE_90) != 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {\n        word |= 1u;",
        "stencil OFF (state 0x90 zero) enables the occlusion bit (the twin of t443-stencil-enable-test, T590).",
    ),
    helper(
        "cull-90-or-91",
        "(d3d8_guest_load32(D3D8_STATE_90) == 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {\n        word |= 1u;\n    }\n    return word;",
        "(d3d8_guest_load32(D3D8_STATE_90) == 0u && d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {\n        word |= 1u;\n    }\n    return word;",
        "stencil off OR state 0x91 at KEEP sets the bit.",
    ),
    helper(
        "a1-store-dropped",
        "    store_state(D3D8_STATE_A1, value);\n    emit_cull_word();",
        "    (void)value;\n    emit_cull_word();",
        "state 0xA1 is stored BEFORE the cull word is formed from it.",
    ),
    helper(
        "a2-store-dropped",
        "    d3d8_guest_store32(D3D8_STATE_A2, value);\n    emit_cull_word();",
        "    (void)value;\n    emit_cull_word();",
        "state 0xA2 is stored BEFORE the cull word is formed from it.",
    ),
    helper(
        "control-words-reset-header",
        "    d3d8_guest_store32(cursor, PUSH_CONTROL_RESET);\n    d3d8_guest_store32(cursor + 4u, 0u);\n    d3d8_guest_store32(cursor + 8u, PUSH_CONTROL_SELECT);",
        "    d3d8_guest_store32(cursor, PUSH_CONTROL_STROBE);\n    d3d8_guest_store32(cursor + 4u, 0u);\n    d3d8_guest_store32(cursor + 8u, PUSH_CONTROL_SELECT);",
        "the control packet opens with the 0x40110 reset.",
    ),
    helper(
        "control-words-a-register",
        "    d3d8_guest_store32(cursor + 12u, 0x00400094u);\n"
        "    d3d8_guest_store32(cursor + 16u, d3d8_device_load32(DEV_CONTROL_WORD_A));",
        "    d3d8_guest_store32(cursor + 12u, 0x00400098u);\n"
        "    d3d8_guest_store32(cursor + 16u, d3d8_device_load32(DEV_CONTROL_WORD_A));",
        "word A goes to method 0x94 of the select window.",
    ),
    helper(
        "control-words-b-register",
        "    d3d8_guest_store32(cursor + 32u, 0x00400B80u);\n"
        "    d3d8_guest_store32(cursor + 36u, d3d8_device_load32(DEV_CONTROL_WORD_B));",
        "    d3d8_guest_store32(cursor + 32u, 0x00400B84u);\n"
        "    d3d8_guest_store32(cursor + 36u, d3d8_device_load32(DEV_CONTROL_WORD_B));",
        "word B goes to method 0xB80 of the select window.",
    ),
    helper(
        "control-words-b-value",
        "    d3d8_guest_store32(cursor + 36u, d3d8_device_load32(DEV_CONTROL_WORD_B));",
        "    d3d8_guest_store32(cursor + 36u, d3d8_device_load32(DEV_CONTROL_WORD_A));",
        "the two control words are different values.",
    ),
    helper(
        "control-words-strobe",
        "    d3d8_guest_store32(cursor + 44u, 9u);\n    d3d8_pushbuffer_end(cursor + 48u);\n}",
        "    d3d8_guest_store32(cursor + 44u, 8u);\n    d3d8_pushbuffer_end(cursor + 48u);\n}",
        "both words are strobed with 9.",
    ),
    helper(
        "control-words-size",
        "    d3d8_pushbuffer_end(cursor + 48u);\n}",
        "    d3d8_pushbuffer_end(cursor + 44u);\n}",
        "the packet is 12 dwords.",
    ),
    helper(
        "control-words-not-updated",
        "static void emit_control_words(void)\n{\n    d3d8_state_update_control_words();",
        "static void emit_control_words(void)\n{",
        "the words are recomputed from the three sources before they are written.",
    ),
    helper(
        "a5-source",
        "    d3d8_guest_store32(GLOBAL_CONTROL_SOURCE_A, value);\n    emit_control_words();",
        "    d3d8_guest_store32(D3D8_STATE_A3, value);\n    emit_control_words();",
        "state 0xA5 is the first control source.",
    ),
    helper(
        "h8f-transition-old",
        "    const bool transition = d3d8_guest_load32(D3D8_STATE_8F) == 2u || value == 2u;\n    const uint32_t declaration_flags =",
        "    const bool transition = d3d8_guest_load32(D3D8_STATE_8F) == 2u;\n    const uint32_t declaration_flags =",
        "entering value 2 runs the viewport cascade too.",
    ),
    helper(
        "h8f-transition-new",
        "    const bool transition = d3d8_guest_load32(D3D8_STATE_8F) == 2u || value == 2u;\n    const uint32_t declaration_flags =",
        "    const bool transition = value == 2u;\n    const uint32_t declaration_flags =",
        "leaving value 2 runs the viewport cascade too.",
    ),
    helper(
        "h8f-depth-bit",
        "                       value != 0u && d3d8_device_load32(DEV_DEPTH_SURFACE) != 0u ? 1u : 0u);\n    d3d8_guest_store32(cursor + 8u, 0x00041D78u);",
        "                       value != 0u || d3d8_device_load32(DEV_DEPTH_SURFACE) != 0u ? 1u : 0u);\n    d3d8_guest_store32(cursor + 8u, 0x00041D78u);",
        "the first packet's flag needs both the state and a depth surface.",
    ),
    helper(
        "h8f-state-4a",
        "    d3d8_guest_store32(cursor + 12u, d3d8_guest_load32(GLOBAL_STATE_4A));",
        "    d3d8_guest_store32(cursor + 12u, 0u);",
        "the second pair carries render state 0x4A.",
    ),
    helper(
        "h8f-first-packet-size",
        "    d3d8_pushbuffer_end(cursor + 16u);\n    store_state(D3D8_STATE_8F, value);",
        "    d3d8_pushbuffer_end(cursor + 12u);\n    store_state(D3D8_STATE_8F, value);",
        "the packet is 16 bytes.",
    ),
    helper(
        "h8f-matrix-check-dropped",
        "        d3d8_rebuild_viewport_matrix_check();\n        d3d8_plan_vertex_program_helper(&sim);\n        const bool programmable",
        "        d3d8_plan_vertex_program_helper(&sim);\n        const bool programmable",
        "the viewport matrix refusals (NaN inputs) precede the first packet.",
    ),
    helper(
        "h8f-second-site-dropped",
        "        d3d8_pushbuffer_sim_site(&sim, 0x003D7EE0u, 8u + viewport + 12u);\n    }\n    uint32_t cursor",
        "        (void)viewport;\n    }\n    uint32_t cursor",
        "the viewport packets sit behind a second reservation preamble that may roll over.",
    ),
    helper(
        "h8f-matrix-apply-dropped",
        "    if (!transition) {\n        return;\n    }\n    (void)d3d8_rebuild_viewport_matrix();\n    d3d8_run_vertex_program_helper();\n    cursor = d3d8_pushbuffer_begin();",
        "    if (!transition) {\n        return;\n    }\n    d3d8_run_vertex_program_helper();\n    cursor = d3d8_pushbuffer_begin();",
        "the transition rebuilds the viewport matrix at device+0x980 and ORs 0x200 into the dirty mask.",
    ),
    helper(
        "h8f-surface-control-dropped",
        "    cursor = d3d8_state_emit_surface_control(cursor);\n    cursor = d3d8_viewport_emit(cursor, declaration_flags);\n    d3d8_pushbuffer_end(cursor);\n}\n\n",
        "    cursor = d3d8_viewport_emit(cursor, declaration_flags);\n    d3d8_pushbuffer_end(cursor);\n}\n\n",
        "the surface control packet precedes the viewport packets.",
    ),
    helper(
        "h8f-viewport-dropped",
        "    cursor = d3d8_state_emit_surface_control(cursor);\n    cursor = d3d8_viewport_emit(cursor, declaration_flags);\n    d3d8_pushbuffer_end(cursor);\n}\n\n",
        "    cursor = d3d8_state_emit_surface_control(cursor);\n    d3d8_pushbuffer_end(cursor);\n}\n\n",
        "the viewport packets follow the surface control packet.",
    ),
    helper(
        "h8f-flags-for-viewport",
        "    cursor = d3d8_viewport_emit(cursor, declaration_flags);\n    d3d8_pushbuffer_end(cursor);\n}\n\n",
        "    cursor = d3d8_viewport_emit(cursor, 0u);\n    d3d8_pushbuffer_end(cursor);\n}\n\n",
        "the viewport packets depend on the declaration flags.",
    ),
    helper(
        "h8f-helper-plan-dropped",
        "        d3d8_rebuild_viewport_matrix_check();\n        d3d8_plan_vertex_program_helper(&sim);\n",
        "        d3d8_rebuild_viewport_matrix_check();\n",
        "the vertex program helper moves the cursor, so the second site rolls earlier, and its own reservation may roll.",
    ),
    helper(
        "h8f-helper-run-dropped",
        "    if (!transition) {\n        return;\n    }\n    (void)d3d8_rebuild_viewport_matrix();\n    d3d8_run_vertex_program_helper();\n",
        "    if (!transition) {\n        return;\n    }\n    (void)d3d8_rebuild_viewport_matrix();\n",
        "a programmable declaration loads its constant table between the viewport matrix and the surface control packet.",
    ),
    helper(
        "h98-helper-run-dropped",
        "    d3d8_guest_store32(D3D8_STATE_98, value);\n    d3d8_run_vertex_program_helper();\n",
        "    d3d8_guest_store32(D3D8_STATE_98, value);\n",
        "0x98 runs the helper before its own word.",
    ),
    helper(
        "h98-store-after-helper",
        "    d3d8_guest_store32(D3D8_STATE_98, value);\n    d3d8_run_vertex_program_helper();\n",
        "    d3d8_run_vertex_program_helper();\n    d3d8_guest_store32(D3D8_STATE_98, value);\n",
        "the helper reads state 0x98 to pick its half texel bias, so the store comes first.",
    ),
    mutation(
        "vph-plan-dropped",
        "src/gpu/d3d8_texture_dirty.c",
        "    fog_sites sites;\n    plan_fog_sites(sim, count, reserve, 0u, &sites);\n}\nvoid d3d8_run_vertex_program_helper(void)",
        "    fog_sites sites;\n    (void)sites;\n    (void)reserve;\n}\nvoid d3d8_run_vertex_program_helper(void)",
        "the helper moves the cursor, so the plan advances the simulated writer by it (T461 wrapper over T546's fog sites).",
        ["test_d3d8_texture_dirty", HELPERS_PROGRAMMABLE],
    ),
    mutation(
        "vph-run-dropped",
        "src/gpu/d3d8_texture_dirty.c",
        "    (void)d3d8_emit_fog_vertex_program();\n}\n/* The words of the whole emitter",
        "}\n/* The words of the whole emitter",
        "the run writes the helper's constant table.",
        ["test_d3d8_texture_dirty", HELPERS_PROGRAMMABLE],
    ),
    # --- the other twelve helpers --------------------------------------------------------------
    helper(
        "h89-dirty",
        "    dirty_or(0x200u);\n    emit_pair_at_preamble(0x00040328u, value);",
        "    dirty_or(0x100u);\n    emit_pair_at_preamble(0x00040328u, value);",
        "state 0x89 ORs 0x200 into the dirty mask before it emits.",
    ),
    helper(
        "h89-header",
        "    emit_pair_at_preamble(0x00040328u, value);",
        "    emit_pair_at_preamble(0x0004032Cu, value);",
        "method 0x328.",
    ),
    helper(
        "h8a-swap",
        "        ((value >> 16) & 0xFFu) | ((value & 0xFFu) << 16) | (value & 0xFF00FF00u);",
        "        ((value >> 16) & 0xFFu) | ((value & 0xFFu) << 8) | (value & 0xFF00FF00u);",
        "the 0x8A word exchanges the red and blue bytes of the value.",
    ),
    helper(
        "h8d-dirty",
        "    emit_pair_at_preamble(0x000417C4u, value);\n"
        "    if (value != 0u) {\n        dirty_or(0x9000u);\n    }",
        "    emit_pair_at_preamble(0x000417C4u, value);\n"
        "    if (value == 0u) {\n        dirty_or(0x9000u);\n    }",
        "state 0x8D on dirties the lighting state.",
    ),
    helper(
        "h8d-second-pair-back",
        "    d3d8_guest_store32(cursor + 8u, two_sided != 0u ? back : front);\n"
        "    d3d8_pushbuffer_end(cursor + 12u);",
        "    d3d8_guest_store32(cursor + 8u, two_sided == 0u ? back : front);\n"
        "    d3d8_pushbuffer_end(cursor + 12u);",
        "two sided lighting selects the back fill mode for the second word.",
    ),
    helper(
        "h8e-dirty",
        "    emit_pair_at_preamble(0x000403A4u, value);\n    dirty_or(0x200u);",
        "    emit_pair_at_preamble(0x000403A4u, value);\n    dirty_or(0x400u);",
        "state 0x8E ORs 0x200.",
    ),
    helper(
        "h92-tail-call",
        "    d3d8_state_library_set_93(current_93);",
        "    (void)current_93;\n    d3d8_state_library_set_93(value);",
        "0x92 tail calls 0x93 with the CURRENT state 0x93, not its own argument.",
    ),
    helper(
        "h92-store-92",
        "    d3d8_guest_store32(D3D8_STATE_92, value);\n    /* A tail call",
        "    /* A tail call",
        "0x93 compares its value with state 0x92, which this helper has just stored.",
    ),
    helper(
        "h98-viewport-dropped",
        "    d3d8_pushbuffer_end(d3d8_viewport_emit(cursor, declaration_flags));\n}",
        "    d3d8_pushbuffer_end(cursor);\n}",
        "0x98 rewrites the viewport packets after its own word.",
    ),
    helper(
        "h98-word-antialias",
        "    d3d8_guest_store32(cursor + 4u, multisample_word(d3d8_guest_load32(D3D8_STATE_9E), value));",
        "    d3d8_guest_store32(cursor + 4u, multisample_word(d3d8_guest_load32(D3D8_STATE_9E), 0u));",
        "bit 0 of the multisample word follows the NEW antialias state.",
    ),
    helper(
        "h98-second-site-dropped",
        "    d3d8_pushbuffer_sim_site(&sim, 0x003D8250u, viewport + 12u);\n",
        "    (void)viewport;\n",
        "the viewport packets sit behind a second reservation preamble.",
    ),
    helper(
        "h9c-offset",
        "    emit_pair_at_preamble(0x00041E6Cu, value - 0x200u);",
        "    emit_pair_at_preamble(0x00041E6Cu, value);",
        "state 0x9C writes the value minus 0x200.",
    ),
    helper(
        "h9d-clamp",
        "    if (level > 0x1FFu) {\n        level = 0x1FFu;\n    }",
        "    if (level > 0x200u) {\n        level = 0x200u;\n    }",
        "the point size level is clamped to 0x1FF.",
    ),
    helper(
        "h9d-scale-constant",
        "    scaled = scaled * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_EIGHT));",
        "    scaled = scaled * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_HALF));",
        "the level is scaled by 8.0.",
    ),
    helper(
        "h9d-rounding",
        "    scaled = scaled + (long double)float_of_bits(d3d8_guest_load32(CONSTANT_HALF));",
        "    scaled = scaled + 0.0L;",
        "0.5 is added before the truncation.",
    ),
    helper(
        "h9e-word",
        "    uint32_t word = (d3d8_guest_load32(D3D8_STATE_99) << 16) | sample_alpha;",
        "    uint32_t word = (d3d8_guest_load32(D3D8_STATE_99) << 15) | sample_alpha;",
        "the multisample mask is the high half of the word.",
    ),
    helper(
        "h9e-flag",
        "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x8000u) != 0u && antialias != 0u) {",
        "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x8000u) != 0u || antialias != 0u) {",
        "bit 0 needs device flag 0x8000 AND the antialias state.",
    ),
    helper(
        "h9e-flag-mask",
        "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x8000u) != 0u && antialias != 0u) {",
        "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x4000u) != 0u && antialias != 0u) {",
        "device flag 0x8000 selects the multisampled render target (the twin of t443-multisample-mask-flag, T590).",
    ),
    helper(
        "h9f-compare",
        "    if (wanted != (flags & 1u)) {",
        "    if (wanted == (flags & 1u)) {",
        "the pair is written when the wanted bit differs from the device flag.",
    ),
    helper(
        "h9f-format-table",
        "        (d3d8_guest_load8(GLOBAL_FORMAT_INFO + format) & 0x3Cu) == 0x20u ? value : 0u;",
        "        (d3d8_guest_load8(GLOBAL_FORMAT_INFO + format) & 0x3Cu) == 0x10u ? value : 0u;",
        "only a 32 bit per pixel format (table bits 0x20) takes the supersample value.",
    ),
    helper(
        "h9f-pair",
        "        d3d8_guest_store32(cursor + 12u, (wanted << 5) | 8u);",
        "        d3d8_guest_store32(cursor + 12u, (wanted << 4) | 8u);",
        "the strobe word is (wanted << 5) | 8.",
    ),
    # --- the dispatcher ----------------------------------------------------------------------
    helper(
        "dispatch-8f",
        "        {0x8Fu, 0x003D7EE0u, d3d8_state_library_set_8f},",
        "        {0x8Fu, 0x003D7EE0u, d3d8_state_library_set_91},",
        "each state calls its own helper.",
        SET_STATE,
    ),
    helper(
        "dispatch-a4",
        "        {0xA4u, 0x003D81B0u, d3d8_state_library_set_a4},",
        "        {0xA4u, 0x003D81B0u, d3d8_state_library_set_a3},",
        "states 0xA3 and 0xA4 are different control sources.",
        SET_STATE,
    ),
    helper(
        "dispatch-9a",
        "        {0x9Au, 0x003D81F0u, d3d8_state_library_set_9a},",
        "        {0x9Au, 0x003D81F0u, d3d8_state_set_9b},",
        "0x9A and 0x9B refuse opposite render target cases.",
        SET_STATE,
    ),
    # --- BumpEnv, BorderColor and ColorKeyColor ------------------------------------------------
    helper(
        "stage-border-header",
        "const uint32_t header = state == 0x1D ? 0x00041B24u + (stage << 6) : 0x00040AE0u + stage * 4u;",
        "const uint32_t header = state == 0x1D ? 0x00041B24u + (stage << 6) : 0x00040AE4u + stage * 4u;",
        "BorderColor writes method 0xAE0 + 4 * stage.",
        SET_STATE,
    ),
    helper(
        "stage-colour-key-header",
        "const uint32_t header = state == 0x1D ? 0x00041B24u + (stage << 6) : 0x00040AE0u + stage * 4u;",
        "const uint32_t header = state == 0x1D ? 0x00041B24u + (stage << 5) : 0x00040AE0u + stage * 4u;",
        "ColorKeyColor writes method 0x1B24 + 0x40 * stage.",
        SET_STATE,
    ),
    helper(
        "stage-colour-key-border-swapped",
        "if (state == 0x1D || state == 0x1E) {",
        "if (state == 0x1D || state == 0x1F) {",
        "state 0x1E is BorderColor and takes the same single pair path.",
        SET_STATE,
    ),
    helper(
        "stage-bump-hardware-stage",
        "const uint32_t hardware_stage = d3d8_device_load32(0x784u) == 0u ? stage + 1u : stage;",
        "const uint32_t hardware_stage = d3d8_device_load32(0x784u) != 0u ? stage + 1u : stage;",
        "with no pixel shader the hardware stage is stage + 1.",
        SET_STATE,
    ),
    helper(
        "stage-bump-silent-test",
        "    if ((hardware_stage & 3u) != 0u) {",
        "    if ((hardware_stage & 1u) != 0u) {",
        "BumpEnv is written only when the hardware stage is not a multiple of 4.",
        SET_STATE,
    ),
    helper(
        "stage-bump-header",
        "const uint32_t header = 0x00041AD0u + ((hardware_stage << 4) + (uint32_t)state) * 4u;",
        "const uint32_t header = 0x00041AD0u + ((hardware_stage << 3) + (uint32_t)state) * 4u;",
        "BumpEnv writes method 0x1AD0 + 0x40 * stage + 4 * state.",
        SET_STATE,
    ),
    helper(
        "stage-shadow-dropped",
        "    d3d8_guest_store32(slot, value);\n}\n",
        "}\n",
        "the BumpEnv shadow is stored after the pair.",
        SET_STATE,
    ),
    helper(
        "stage-border-shadow-dropped",
        "        emit_pair_at_preamble(header, value);\n        d3d8_guest_store32(slot, value);\n        return;",
        "        emit_pair_at_preamble(header, value);\n        return;",
        "the BorderColor and ColorKeyColor shadows are stored after the pair.",
        SET_STATE,
    ),
    # --- the texture transform shapes ----------------------------------------------------------
    transform(
        "tt-zero-bytes",
        "    return shape == SHAPE_ZERO ? 8u : 76u;",
        "    return shape == SHAPE_ZERO ? 8u : 72u;",
        "a stage with transform flags writes 8 + 4 + 64 bytes.",
    ),
    transform(
        "tt-enable-value",
        "            d3d8_guest_store32(cursor + 4u, 1u);\n            d3d8_guest_store32(cursor + 8u, TRANSFORM_MATRIX_HEADER",
        "            d3d8_guest_store32(cursor + 4u, 0u);\n            d3d8_guest_store32(cursor + 8u, TRANSFORM_MATRIX_HEADER",
        "the enable pair carries 1 when the stage transforms.",
    ),
    transform(
        "tt-matrix-header",
        "            d3d8_guest_store32(cursor + 8u, TRANSFORM_MATRIX_HEADER + stage * 0x40u);",
        "            d3d8_guest_store32(cursor + 8u, TRANSFORM_MATRIX_HEADER + stage * 0x20u);",
        "each stage's matrix goes to its own 0x40 byte window.",
    ),
    transform(
        "tt-source-stride",
        "        m[index] = d3d8_device_load32(TRANSFORM_SOURCE + stage * 0x40u + index * 4u);",
        "        m[index] = d3d8_device_load32(TRANSFORM_SOURCE + stage * 0x44u + index * 4u);",
        "the stage matrices are 0x40 bytes apart.",
    ),
    transform(
        "tt-dimension-from-mode",
        "    uint32_t dimension = 3u;\n    if ((index_word & 0xFFFF0000u) == 0u) {",
        "    uint32_t dimension = 2u;\n    if ((index_word & 0xFFFF0000u) == 0u) {",
        "a texture coordinate mode (texgen) makes the dimension 3.",
    ),
    transform(
        "tt-dimension-default",
        "        if (dimension == 0u) dimension = 2u;",
        "        if (dimension == 0u) dimension = 3u;",
        "a declaration that does not name the set defaults to 2.",
    ),
    transform(
        "tt-dimension-shift",
        "(((index_word & 0xFFFFu) << 3) & 31u)) & 0xFFu;",
        "(((index_word & 0xFFFFu) << 2) & 31u)) & 0xFFu;",
        "the declaration holds one byte per texture coordinate set.",
    ),
    transform(
        "tt-code-count-mask",
        "    const uint32_t code = (((dimension << 4) | (flags & 0xFFu)) << 4) | ((flags >> 8) & 1u);",
        "    const uint32_t code = (((dimension << 4) | (flags & 0xFu)) << 4) | ((flags >> 8) & 1u);",
        "the count is the low BYTE of the flags and overlaps the dimension above 15.",
    ),
    transform(
        "tt-code-projected",
        "    const uint32_t code = (((dimension << 4) | (flags & 0xFFu)) << 4) | ((flags >> 8) & 1u);",
        "    const uint32_t code = (((dimension << 4) | (flags & 0xFFu)) << 4) | ((flags >> 9) & 1u);",
        "the projected bit is bit 8 of the flags.",
    ),
    transform(
        "tt-code-high-bound",
        "    if (code > 0x320u) return code == 0x330u",
        "    if (code >= 0x320u) return code == 0x330u",
        "0x320 has its own shape, the full ones start above it.",
    ),
    transform(
        "tt-3x4-code",
        "    if (code == 0x320u) return SHAPE_3X4;",
        "    if (code == 0x321u) return SHAPE_3X4;",
        "code 0x320 (3 components, count 2, not projected) is the 3x4 shape.",
    ),
    transform(
        "tt-full-z-code",
        "return code == 0x330u ? SHAPE_FULL_Z : code == 0x331u ? SHAPE_FULL_W : SHAPE_FULL;",
        "return code == 0x330u ? SHAPE_FULL_W : code == 0x331u ? SHAPE_FULL_Z : SHAPE_FULL;",
        "codes 0x330 and 0x331 are two different cut down 4x4 shapes.",
    ),
    transform(
        "tt-garbage-allowed",
        '    default:\n        d3d8_hle_fatal(0x003DE080u,\n                       "stage %u texture transform code',
        '    default:\n        if (stage < 4u) return SHAPE_2D;\n        d3d8_hle_fatal(0x003DE080u,\n                       "stage %u texture transform code',
        "an entry past the four labels jumps through garbage in the original, a refusal here.",
    ),
    transform(
        "tt-label-2d-z",
        "    case 0x003DE1C2u: return SHAPE_2D_Z;",
        "    case 0x003DE1C2u: return SHAPE_2D_W;",
        "label 0x3DE1C2 writes the third row, label 0x3DE200 the fourth.",
    ),
    transform(
        "tt-label-2d-zw",
        "    case 0x003DE250u: return SHAPE_2D_ZW;",
        "    case 0x003DE250u: return SHAPE_2D_Z;",
        "label 0x3DE250 writes both the third and the fourth row.",
    ),
    transform(
        "tt-integer-move-row0",
        "    out[0] = m[0];\n    out[1] = F(4);",
        "    out[0] = F(0);\n    out[1] = F(4);",
        "the first element of each row is an integer move, a NaN there is copied, not refused.",
    ),
    transform(
        "tt-row0-transpose",
        "    out[0] = m[0];\n    out[1] = F(4);\n    out[4] = m[1];\n    out[5] = F(5);",
        "    out[0] = m[0];\n    out[1] = F(5);\n    out[4] = m[1];\n    out[5] = F(4);",
        "the 2D shapes are transposed: column 0 of the matrix is the first row of the packet.",
    ),
    transform(
        "tt-3x4-w",
        "        out[2] = F(8);\n        out[3] = F(12);\n        out[6] = F(9);\n        out[7] = F(13);",
        "        out[2] = F(8);\n        out[3] = F(13);\n        out[6] = F(9);\n        out[7] = F(12);",
        "the third and fourth column of the 3x4 and full shapes.",
    ),
    transform(
        "tt-2d-translation",
        "        out[3] = F(8);\n        out[7] = F(9);",
        "        out[3] = F(9);\n        out[7] = F(8);",
        "the 2D shapes carry the translation of m8 and m9 in the last column.",
    ),
    transform(
        "tt-full-row2",
        "        out[8] = m[2];\n        out[9] = F(6);\n        out[10] = F(10);\n        out[11] = F(14);\n        break;\n    case SHAPE_FULL_W:",
        "        out[8] = m[2];\n        out[9] = F(6);\n        out[10] = F(14);\n        out[11] = F(10);\n        break;\n    case SHAPE_FULL_W:",
        "the third row of the full shape.",
    ),
    transform(
        "tt-full-w-row3",
        "        out[12] = m[2];\n        out[13] = F(6);\n        out[14] = F(10);\n        out[15] = F(14);",
        "        out[12] = m[2];\n        out[13] = F(6);\n        out[14] = F(14);\n        out[15] = F(10);",
        "shape 0x331 puts matrix row 2 in the LAST packet row.",
    ),
    transform(
        "tt-2d-w-row3",
        "    case SHAPE_2D_W:\n        out[12] = m[2];\n        out[13] = F(6);\n        out[15] = F(10);",
        "    case SHAPE_2D_W:\n        out[12] = m[2];\n        out[13] = F(10);\n        out[15] = F(6);",
        "shape 0x200 puts matrix row 2 in the last packet row.",
    ),
    transform(
        "tt-2d-zw-row3",
        "        out[12] = m[3];\n        out[13] = F(7);\n        out[15] = F(11);",
        "        out[12] = m[3];\n        out[13] = F(11);\n        out[15] = F(7);",
        "shape 0x250 carries both rows 2 and 3.",
    ),
    transform(
        "tt-full-row3",
        "        out[12] = m[3];\n        out[13] = F(7);\n        out[14] = F(11);\n        out[15] = F(15);",
        "        out[12] = m[3];\n        out[13] = F(7);\n        out[14] = F(15);\n        out[15] = F(11);",
        "the last row of the full shape.",
    ),
    transform(
        "tt-default-w",
        "    out[15] = 0x3F800000u;\n    const bool full",
        "    out[15] = 0x40000000u;\n    const bool full",
        "the fourth column's last element is 1.0 unless the shape writes it.",
    ),
    transform(
        "tt-nan-refusal-dropped",
        "    if ((bits & 0x7FFFFFFFu) > 0x7F800000u) *nan = true;",
        "    if ((bits & 0x7FFFFFFFu) > 0x7F800000u && false) *nan = true;",
        "a NaN that goes through the x87 is refused (payload handling differs from the oracle).",
    ),
    transform(
        "tt-validate-in-emit",
        "    if (d3d8_dirty_programmable()) return;\n    validate_texture_transforms();\n    for (uint32_t stage = 0u; stage < 4u; stage++) {\n        /* 0x003DE0D9",
        "    if (d3d8_dirty_programmable()) return;\n    for (uint32_t stage = 0u; stage < 4u; stage++) {\n        /* 0x003DE0D9",
        "the emitter refuses before its first write even when called without the plan.",
    ),
    transform(
        "tt-plan-sizes",
        "        d3d8_pushbuffer_sim_site(sim, 0x003DE080u, shape_bytes(stage_transform_shape(stage)));",
        "        d3d8_pushbuffer_sim_site(sim, 0x003DE080u, 8u);",
        "the plan sizes each stage's site by its shape.",
    ),
    # --- the read-only twin of the viewport matrix rebuild ------------------------------------
    mutation(
        "vm-check-nan-inputs",
        VIEWPORT_MATRIX,
        "    reject_nan(0x003D6E50u,left,16u);reject_nan(0x003D6E50u,constants,4u);",
        "    reject_nan(0x003D6E50u,left,16u);",
        "a NaN viewport constant must be refused before anything is written.",
        ["test_d3d8_viewport_matrix", ORACLE_HELPERS],
    ),
    mutation(
        "vm-check-returns-before-apply",
        VIEWPORT_MATRIX,
        "    if(!apply) {\n        /* The read-only twin",
        "    if(!apply && false) {\n        /* The read-only twin",
        "the check must not run the rebuild (which writes the matrix and the dirty mask).",
        ["test_d3d8_viewport_matrix", ORACLE_HELPERS],
    ),
]

# The test selection of every helper mutation, by id prefix (first match wins).
SELECTION: list[tuple[str, list[str]]] = [
    ("t461-h91", [SET_STATE_EACH, HELPERS_REST]),
    ("t461-cull", [SET_STATE_EACH, HELPERS_REST]),
    ("t461-a1-", [SET_STATE_EACH, HELPERS_REST]),
    ("t461-a2-", [SET_STATE_EACH, HELPERS_REST]),
    ("t461-control-words", [HELPERS_CONTROL, HELPERS_REST]),
    ("t461-a5-", [HELPERS_CONTROL, HELPERS_REST]),
    ("t461-h9a", [HELPERS_9A]),
    ("t461-h8f", [HELPERS_8F]),
    ("t461-dispatch", [SET_STATE_DISPATCH, f'{ORACLE_HELPERS} -k "dispatcher_off"']),
    ("t461-stage", [SET_STATE_STAGE]),
    ("t461-vph", [HELPERS_PROGRAMMABLE]),
    ("t461-vm", [HELPERS_8F]),
]
for _entry in MUTATIONS:
    for _prefix, _tests in SELECTION:
        if _entry["id"].startswith(_prefix):
            _entry["targets"] = [_entry["targets"][0], *_tests]
            break
