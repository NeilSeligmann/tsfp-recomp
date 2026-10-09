# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the T443 copy composition of Swap: the library's own render state and texture stage
state dispatchers and helpers (src/gpu/d3d8_set_state.c, d3d8_state.c), later the fixed-function
dirty cascade, the immediate mode commands and the composition itself.

Every mutation is covered by a pytest target that replays the ORIGINAL bytes under the oracle
(`tests/test_d3d8_set_state_oracle.py`), the ctest binary named beside it is the compile guard (a
mutation that does not compile would make the pytest runner build fail, which reads like a kill).

EQUIVALENT MUTANTS DROPPED (a survivor that cannot be killed is not evidence): `s14 = edx | 0x18000u`
to `0x10000u` in ADDSIGNED2X (the fall through into ADDSIGNED ORs 0x8000 again) and the 32 dword chunk
bound of the copy's `push_program` (both callers pass 8 dwords, clang folds the bound away and the
object file is byte identical).

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror`
turns the latter into NOT-A-MUTANT, which reads like evidence.
"""

SET_STATE = "src/gpu/d3d8_set_state.c"
STATE = "src/gpu/d3d8_state.c"
COMBINER = "src/gpu/d3d8_combiner.c"
DIRTY = "src/gpu/d3d8_dirty.c"
RESOURCE = "src/gpu/d3d8_resource.c"
IMMEDIATE = "src/gpu/d3d8_immediate.c"
COPY = "src/gpu/d3d8_copy.c"
PRESENT = "src/gpu/d3d8_present.c"
BIND = "src/gpu/d3d8_bind.c"
T_STATE = ["test_d3d8_state"]
ORACLE_STATE = "pytest:tests/test_d3d8_set_state_oracle.py"
ORACLE_CASCADE = "pytest:tests/test_d3d8_fixed_function_cascade_oracle.py"
ORACLE_IMMEDIATE = "pytest:tests/test_d3d8_immediate_oracle.py"
ORACLE_COPY = "pytest:tests/test_d3d8_copy_composition_port_oracle.py"
ORACLE_SWAP = "pytest:tests/test_d3d8_swap_copy_oracle.py"
ORACLE_TITLE = "pytest:tests/test_d3d8_title_state_oracle.py"
ORACLE_SRT = "pytest:tests/test_d3d8_set_render_target_oracle.py"
ORACLE_BIND = "pytest:tests/test_d3d8_bind_oracle.py"
ORACLE_TRANSFORM_NAN = (
    'pytest:tests/test_d3d8_texture_transform_oracle.py -k "nan_word_in_the_cascade"'
)


def mutation(
    identifier: str, file: str, old: str, new: str, why: str, oracle: str = ORACLE_STATE
) -> dict:
    return {
        "id": f"t443-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": [*T_STATE, oracle],
        "why": why,
    }


def cascade(identifier: str, file: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, file, old, new, why, ORACLE_CASCADE)


MUTATIONS: list[dict] = [
    mutation(
        "rs-immediate-value-off",
        SET_STATE,
        "        d3d8_pushbuffer_emit_pair(d3d8_guest_load32(D3D8_RS_HEADER_TABLE + (uint32_t)state * 4u),\n                                  value);",
        "        d3d8_pushbuffer_emit_pair(d3d8_guest_load32(D3D8_RS_HEADER_TABLE + (uint32_t)state * 4u),\n                                  value + 1u);",
        "an immediate state pushes the value it was given, a wrong value is a wrong register write.",
    ),
    mutation(
        "rs-immediate-shadow-dropped",
        SET_STATE,
        "                                  value);\n        d3d8_guest_store32(shadow, value);\n        return;\n    }\n    if (state < 0x88) {",
        "                                  value);\n        return;\n    }\n    if (state < 0x88) {",
        "the shadow is what the copy's snapshot reads back and the restore compares against.",
    ),
    mutation(
        "rs-deferred-dirty-dropped",
        SET_STATE,
        "        dirty_or(d3d8_guest_load32(D3D8_RS_DIRTY_TABLE + (uint32_t)state * 4u));",
        "        dirty_or(0u);",
        "a deferred state that marks nothing is never flushed by the cascade.",
    ),
    mutation(
        "rs-immediate-bound-moved",
        SET_STATE,
        "    if (state < 0x5C) {",
        "    if (state < 0x5D) {",
        "state 0x5C is the first DEFERRED state, one past the bound it would emit.",
    ),
    mutation(
        "rs-deferred-bound-moved",
        SET_STATE,
        "    if (state < 0x88) {\n        dirty_or(",
        "    if (state < 0x89) {\n        dirty_or(",
        "state 0x88 is the first helper state, one past the bound it would be deferred.",
    ),
    mutation(
        "rs-helper-entry-check-dropped",
        SET_STATE,
        "            if (entry != helpers[index].address) {",
        "            if (entry != helpers[index].address && false) {",
        "a helper table entry that moved must be refused, not silently run as the wrong function.",
    ),
    mutation(
        "ts-color-op-boundary",
        SET_STATE,
        "        dirty_or(value < 0x19u ? 0x800u : 0x480Fu);",
        "        dirty_or(value <= 0x19u ? 0x800u : 0x480Fu);",
        "0x19 is the first programmable operation and selects the large dirty set.",
    ),
    mutation(
        "ts-color-op-dirty-swapped",
        SET_STATE,
        "        dirty_or(value < 0x19u ? 0x800u : 0x480Fu);",
        "        dirty_or(value < 0x19u ? 0x480Fu : 0x800u);",
        "the two dirty sets select different cascade families.",
    ),
    mutation(
        "ts-low-dirty-bit",
        SET_STATE,
        "        dirty_or(1u << stage);",
        "        dirty_or(1u);",
        "the stage's own dirty bit selects which texture stage packet the cascade rewrites.",
    ),
    mutation(
        "ts-color-op-bound",
        SET_STATE,
        "    if (state < (int32_t)D3D8_TS_COLOR_OP) {",
        "    if (state <= (int32_t)D3D8_TS_COLOR_OP) {",
        "state 0xC is the colour operation with its own dirty rule, not a plain stage bit.",
    ),
    mutation(
        "ts-table-bound",
        SET_STATE,
        "    if (state < 0x16) {",
        "    if (state < 0x17) {",
        "state 0x16 is a helper state, one past the table that holds the dirty bits.",
    ),
    mutation(
        "texcoord-wrap-cube",
        SET_STATE,
        "            code = 0x8512u;\n            wrap = 1u;",
        "            code = 0x8512u;\n            wrap = 0u;",
        "the wrap mask records which stages use a cube or sphere map, the cascade reads it.",
    ),
    mutation(
        "texcoord-kept-index",
        SET_STATE,
        "        kept = stage;",
        "        kept = value;",
        "a generated coordinate maps the stage to its own attribute, not to the caller's word.",
    ),
    mutation(
        "texcoord-map-offset",
        SET_STATE,
        "(uint8_t)(kept + 9u));",
        "(uint8_t)(kept + 8u));",
        "the attribute map byte is the vertex attribute the stage samples.",
    ),
    mutation(
        "texcoord-dirty-bits",
        SET_STATE,
        "    dirty_or(0x47Fu);",
        "    dirty_or(0x47Eu);",
        "stage 0 would never be rewritten after a coordinate index change.",
    ),
    mutation(
        "texcoord-first-wrap-dirty",
        SET_STATE,
        "    if (mask == 0u && wrap != 0u) {",
        "    if (mask != 0u && wrap != 0u) {",
        "the first stage that wraps marks the matrix family dirty.",
    ),
    mutation(
        "fill-mode-second-word",
        STATE,
        "void d3d8_state_set_8b(uint32_t value)\n{\n"
        "    /* Both shadows are read after the original refill boundary, in instruction order. */\n"
        "    uint32_t cursor = d3d8_pushbuffer_begin();\n"
        "    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);\n"
        "    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);\n"
        "    const uint32_t second = two_sided != 0u ? back : value;",
        "void d3d8_state_set_8b(uint32_t value)\n{\n"
        "    /* Both shadows are read after the original refill boundary, in instruction order. */\n"
        "    uint32_t cursor = d3d8_pushbuffer_begin();\n"
        "    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);\n"
        "    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);\n"
        "    const uint32_t second = two_sided != 0u ? value : back;",
        "two sided lighting selects the back fill mode for the second word.",
    ),
    mutation(
        "fill-mode-header",
        STATE,
        "#define PUSH_FILL_MODE 0x0008038Cu",
        "#define PUSH_FILL_MODE 0x0008038Du",
        "the fill mode packet header.",
    ),
    mutation(
        "logic-op-off-header",
        STATE,
        "        d3d8_guest_store32(cursor, PUSH_LOGIC_OP_OFF);",
        "        d3d8_guest_store32(cursor, PUSH_LOGIC_OP_ON);",
        "a zero logic op writes the one parameter disable packet.",
    ),
    mutation(
        "edge-aa-second-word",
        STATE,
        "    d3d8_guest_store32(cursor + 8u, value);\n    d3d8_pushbuffer_end(cursor + 12u);\n    store_state(D3D8_STATE_97, value);",
        "    d3d8_guest_store32(cursor + 8u, 0u);\n    d3d8_pushbuffer_end(cursor + 12u);\n    store_state(D3D8_STATE_97, value);",
        "edge anti-aliasing sets both words of its packet.",
    ),
    mutation(
        "multisample-mask-shift",
        STATE,
        "    uint32_t word = (value << 16) | d3d8_guest_load32(D3D8_STATE_9E);",
        "    uint32_t word = (value << 8) | d3d8_guest_load32(D3D8_STATE_9E);",
        "the mask occupies the high half of the multisample control word.",
    ),
    mutation(
        "multisample-mask-antialias-bit",
        STATE,
        "        word |= 1u;\n    }\n    const uint32_t cursor = d3d8_pushbuffer_begin();",
        "        word |= 2u;\n    }\n    const uint32_t cursor = d3d8_pushbuffer_begin();",
        "the antialias enable is bit 0 of the control word.",
    ),
    mutation(
        "multisample-mask-flag",
        STATE,
        "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x8000u) != 0u &&\n        d3d8_guest_load32(D3D8_STATE_98) != 0u) {",
        "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x4000u) != 0u &&\n        d3d8_guest_load32(D3D8_STATE_98) != 0u) {",
        "device flag 0x8000 selects the multisampled render target (the HLE state 0x99, its library twin is t461-h9e-flag-mask).",
    ),
    mutation(
        "surface-control-yuv",
        STATE,
        "        word = 0x10100001u;",
        "        word = 0x10100000u;",
        "the yuv enable word.",
    ),
    mutation(
        "surface-control-state-8f",
        STATE,
        "        word |= 0x10000u;\n    }\n    const uint32_t depth",
        "        word |= 0x20000u;\n    }\n    const uint32_t depth",
        "state 0x8F value 2 sets bit 16.",
    ),
    mutation(
        "surface-control-depth-format",
        STATE,
        "format == 0x2Fu) {",
        "format == 0x30u) {",
        "the depth formats that set bit 12 are 0x2D 0x2B 0x31 and 0x2F.",
    ),
    mutation(
        "psmode-device-word",
        STATE,
        "    d3d8_guest_store32(device + 0x790u, value);",
        "    d3d8_guest_store32(device + 0x794u, value);",
        "the pixel shader texture modes are cached at device+0x790 for the stage program.",
    ),
    mutation(
        "psmode-dirty",
        STATE,
        "D3D8_GLOBAL_DIRTY_MASK) | 0x4000u);\n    store_state(D3D8_STATE_88, value);",
        "D3D8_GLOBAL_DIRTY_MASK) | 0x2000u);\n    store_state(D3D8_STATE_88, value);",
        "0x4000 selects the stage program emitter.",
    ),
    mutation(
        "cull-front-face-compare",
        STATE,
        "                           0x404u + (value != d3d8_guest_load32(D3D8_STATE_92) ? 1u : 0u));",
        "                           0x404u + (value == d3d8_guest_load32(D3D8_STATE_92) ? 1u : 0u));",
        "the front face winding is selected by comparing the cull mode with state 0x92.",
    ),
    mutation(
        "stencil-occlusion-bit",
        STATE,
        "        cull |= 1u;\n    }\n    d3d8_guest_store32(cursor, 0x00041D84u);",
        "        cull |= 2u;\n    }\n    d3d8_guest_store32(cursor, 0x00041D84u);",
        "occlusion culling is bit 0 of the cull control word.",
    ),
    mutation(
        "stencil-enable-test",
        STATE,
        "    uint32_t cull = d3d8_guest_load32(D3D8_STATE_A2) != 0u ? 2u : 0u;\n    if (d3d8_guest_load32(D3D8_STATE_A1) != 0u &&\n        (d3d8_guest_load32(D3D8_STATE_90) == 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {",
        "    uint32_t cull = d3d8_guest_load32(D3D8_STATE_A2) != 0u ? 2u : 0u;\n    if (d3d8_guest_load32(D3D8_STATE_A1) != 0u &&\n        (d3d8_guest_load32(D3D8_STATE_90) != 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {",
        "occlusion culling applies with stencil disabled or a keep fail operation (state 0x90's own word, the library twin of cull_word is t461-cull-90-compare).",
    ),
    mutation(
        "texture-factor-shader-bound",
        STATE,
        "    if (d3d8_device_load32(0x784u) == 0u) {\n        const uint32_t cursor = d3d8_pushbuffer_begin();\n        d3d8_guest_store32(cursor, 0x00400A60u);",
        "    if (d3d8_device_load32(0x784u) != 0u) {\n        const uint32_t cursor = d3d8_pushbuffer_begin();\n        d3d8_guest_store32(cursor, 0x00400A60u);",
        "a bound pixel shader owns the combiner factors, so the texture factor is only stored.",
    ),
    mutation(
        "zbias-state-index",
        STATE,
        "        d3d8_set_render_state_notinline((int32_t)(RS_DEPTH_BIAS_FIRST + index), states[index]);",
        "        d3d8_set_render_state_notinline((int32_t)(RS_DEPTH_BIAS_FIRST + index + 1u), states[index]);",
        "the depth bias writes states 0x4D to 0x51 in order.",
    ),
    mutation(
        "zbias-flag-states",
        STATE,
        "    const uint32_t states[5] = {bits_of_float(scaled_float), bits_of_float(negated_float), flag,\n                                flag, flag};",
        "    const uint32_t states[5] = {bits_of_float(negated_float), bits_of_float(scaled_float), flag,\n                                flag, flag};",
        "the scaled bias goes to the polygon offset scale state and the plain one to the bias.",
    ),
]

MUTATIONS += [
    cascade(
        "comb-source-first-stage",
        COMBINER,
        "esi = (edx & 0x10u) != 0u ? 4u : 0xCu;",
        "esi = (edx & 0x10u) != 0u ? 0xCu : 4u;",
        "selector 1 is current or diffuse by whether this is the first stage.",
    ),
    cascade(
        "comb-source-texture-index",
        COMBINER,
        "== 0u ? 0xFFFFFFFFu : index + 8u;",
        "== 0u ? 0xFFFFFFFFu : index + 9u;",
        "selector 2 names the stage's texture.",
    ),
    cascade(
        "comb-source-fog-flag",
        COMBINER,
        "        *flags |= FLAG_FOG;\n        esi = 5u;",
        "        *flags |= 0u;\n        esi = 5u;",
        "selector 4 sets the device flag the lighting emitter reads.",
    ),
    cascade(
        "comb-input-ecx-flag",
        COMBINER,
        "    eax |= ecx & 0x40u;",
        "    eax |= ecx & 0x20u;",
        "bit 6 of the argument word.",
    ),
    cascade(
        "comb-input-shift",
        COMBINER,
        "const uint32_t shift = ((ecx >> 13) & 0x78u) & 0x1Fu;",
        "const uint32_t shift = ((ecx >> 12) & 0x78u) & 0x1Fu;",
        "the byte lane of the input word.",
    ),
    cascade(
        "comb-disable-flag",
        COMBINER,
        "    eax = (ecx << 0x17) | 0x4200000u;\n    ebp = 0u;",
        "    eax = (ecx << 0x17) | 0x4000000u;\n    ebp = 0u;",
        "the disabled stage word.",
    ),
    cascade(
        "comb-disable-alpha-word",
        COMBINER,
        "    eax |= 0x10000000u;\n    put(buffer, 1u, edi, edx);",
        "    eax |= 0x20000000u;\n    put(buffer, 1u, edi, edx);",
        "the alpha input of a disabled stage.",
    ),
    cascade(
        "comb-select-arg1-flag",
        COMBINER,
        "    eax |= 0x200000u;\n    goto finish;\n\nop_select_arg2:",
        "    eax |= 0x400000u;\n    goto finish;\n\nop_select_arg2:",
        "the SELECTARG1 output bit.",
    ),
    cascade(
        "comb-select-arg2-flag",
        COMBINER,
        "    eax |= 0x2000u;\n    goto finish;",
        "    eax |= 0x4000u;\n    goto finish;",
        "the SELECTARG2 output bit.",
    ),
    cascade(
        "comb-modulate-4x",
        COMBINER,
        "    s14 = edx | 0x20000u;\n    goto op_modulate;",
        "    s14 = edx | 0x10000u;\n    goto op_modulate;",
        "MODULATE4X scales the output by four.",
    ),
    cascade(
        "comb-modulate-2x",
        COMBINER,
        "    s14 = edx | 0x10000u;\n    /* fall through */",
        "    s14 = edx | 0x20000u;\n    /* fall through */",
        "MODULATE2X scales the output by two.",
    ),
    cascade(
        "comb-add-signed",
        COMBINER,
        "    s14 |= 0x8000u;\n    /* fall through */",
        "    s14 |= 0x4000u;\n    /* fall through */",
        "ADDSIGNED bias.",
    ),
    cascade(
        "comb-add-word",
        COMBINER,
        "    esi = eax | 0x202000u;",
        "    esi = eax | 0x201000u;",
        "ADD.",
    ),
    cascade(
        "comb-subtract-word",
        COMBINER,
        "    esi = eax | 0x204000u;",
        "    esi = eax | 0x208000u;",
        "SUBTRACT.",
    ),
    cascade(
        "comb-add-smooth-selector",
        COMBINER,
        "    eax = combiner_input(0x10010u, ebx, edi, &flags);\n    esi = s10;\n    goto tail_or_esi;\n\nop_blend",
        "    eax = combiner_input(0x10020u, ebx, edi, &flags);\n    esi = s10;\n    goto tail_or_esi;\n\nop_blend",
        "ADDSMOOTH's second input.",
    ),
    cascade(
        "comb-blend-operation-base",
        COMBINER,
        "    eax -= 0xCu;",
        "    eax -= 0xBu;",
        "the four blend operations select the source by the operation minus 12.",
    ),
    cascade(
        "comb-blend-second-input",
        COMBINER,
        "    eax = combiner_input(0x20020u, ebx, edi, &flags);\n    ecx = 0x10030u;",
        "    eax = combiner_input(0x20030u, ebx, edi, &flags);\n    ecx = 0x10030u;",
        "the blend's second input mask.",
    ),
    cascade(
        "comb-premodulate-selector",
        COMBINER,
        "    edi = 2u;\n    ecx = 0x10030u;\n    goto tail_call;",
        "    edi = 3u;\n    ecx = 0x10030u;\n    goto tail_call;",
        "PREMODULATE reads the texture.",
    ),
    cascade(
        "comb-modulate-alpha-first-stage",
        COMBINER,
        "    if ((ebx & 0x10u) == 0u) {\n        esi |= 0x200000u;",
        "    if ((ebx & 0x10u) != 0u) {\n        esi |= 0x200000u;",
        "the first stage has no previous texture.",
    ),
    cascade(
        "comb-inv-color-selector",
        COMBINER,
        "    eax = combiner_input(0x10020u, ebx, edi, &flags);\n    esi = s10;\n    goto tail_or_esi;\n\nop_modulate_inv_alpha_add_color:",
        "    eax = combiner_input(0x10030u, ebx, edi, &flags);\n    esi = s10;\n    goto tail_or_esi;\n\nop_modulate_inv_alpha_add_color:",
        "MODULATEALPHA_ADDCOLOR style mask.",
    ),
    cascade(
        "comb-bump-base",
        COMBINER,
        "    ecx = 0x30000u;\n    goto op_bump_shared;",
        "    ecx = 0x30010u;\n    goto op_bump_shared;",
        "the two bump operations differ in the first input mask.",
    ),
    cascade(
        "comb-bump-word",
        COMBINER,
        "    eax |= 0x20u;\n    goto finish;",
        "    eax |= 0x40u;\n    goto finish;",
        "the bump output bit.",
    ),
    cascade(
        "comb-luminance-output",
        COMBINER,
        "    ecx |= 0x820000u;",
        "    ecx |= 0x800000u;",
        "the luminance operation's output word.",
    ),
    cascade(
        "comb-luminance-shift",
        COMBINER,
        "    ecx >>= 4;\n    edx = 0u;",
        "    ecx >>= 3;\n    edx = 0u;",
        "the luminance output word is the default shifted by four.",
    ),
    cascade(
        "comb-dot3-input",
        COMBINER,
        "    eax = combiner_input(0x10000u, ebx, edi, &flags);\n    esi = s10;\n    goto tail_or_esi;\n\nop_multiply_add:",
        "    eax = combiner_input(0x10010u, ebx, edi, &flags);\n    esi = s10;\n    goto tail_or_esi;\n\nop_multiply_add:",
        "DOTPRODUCT3's second input.",
    ),
    cascade(
        "comb-lerp-flag",
        COMBINER,
        "    flags |= FLAG_PROGRAM;",
        "    flags |= 0u;",
        "operations 25 and 26 set the device flag that dirties the program and stage families.",
    ),
    cascade(
        "comb-saturate-word",
        COMBINER,
        "    if (edx == 0xFF000000u) {",
        "    if (edx == 0xFE000000u) {",
        "an input of all ones is replaced by the zero word.",
    ),
    cascade(
        "comb-saturate-first",
        COMBINER,
        "        eax = ((~ebx) & 0x10u) << 0x17;",
        "        eax = (ebx & 0x10u) << 0x17;",
        "the replacement word follows the first stage bit.",
    ),
    cascade(
        "comb-alpha-mask",
        COMBINER,
        "    ecx = ebx & 0x48u;\n    put_slot",
        "    ecx = ebx & 0x40u;\n    put_slot",
        "the alpha pass writes the second pair of packets.",
    ),
    cascade(
        "comb-alpha-flags",
        COMBINER,
        "        ebx |= 0xE8u;",
        "        ebx |= 0xA8u;",
        "the alpha pass flags.",
    ),
    cascade(
        "comb-fill-zero",
        COMBINER,
        "    ebp = 0u;\n\nfill:",
        "    ebp = 1u;\n\nfill:",
        "unused stages are zero.",
    ),
    cascade(
        "comb-stage-count",
        COMBINER,
        "    esi -= start;",
        "    esi -= start * 0u;",
        "the stage count is relative to the first stage.",
    ),
    cascade(
        "comb-header-order",
        COMBINER,
        "{0x00200AC0u, 0x00201E40u, 0x00200260u, 0x00200AA0u}",
        "{0x00200AC0u, 0x00200260u, 0x00201E40u, 0x00200AA0u}",
        "the four packet headers.",
    ),
    cascade(
        "comb-fog-pair",
        COMBINER,
        "        plan->words[plan->count++] = 0x000403B8u;",
        "        plan->words[plan->count++] = 0x000403BCu;",
        "the lighting fog pair.",
    ),
    cascade(
        "comb-fog-pair-condition",
        COMBINER,
        "d3d8_guest_load32(RS_FOG_PAIR) == 0u) {",
        "d3d8_guest_load32(RS_FOG_PAIR) != 0u) {",
        "the fog pair is written only while render state 0x67 is zero.",
    ),
    cascade(
        "comb-dirty-after",
        COMBINER,
        "        plan->dirty_after |= 0x400Fu;",
        "        plan->dirty_after |= 0x4007u;",
        "a program flag change redirties the stage program and all four stages.",
    ),
    cascade(
        "comb-first-stage-start",
        COMBINER,
        "d3d8_guest_load32(RS_FIRST_STAGE) != 0u ? 3u : 0u;",
        "d3d8_guest_load32(RS_FIRST_STAGE) != 0u ? 2u : 0u;",
        "render state 0x76 starts the walk at stage 3.",
    ),
    cascade(
        "comb-result-arg",
        COMBINER,
        "!= 5u ? 0xC00u : 0xD00u;",
        "!= 5u ? 0xD00u : 0xC00u;",
        "the default output word follows the result argument.",
    ),
    cascade(
        "comb-alpha-arg",
        COMBINER,
        "    ecx = d3d8_guest_load32(eax + 0x44u);",
        "    ecx = d3d8_guest_load32(eax + 0x48u);",
        "the alpha pass reads states 0x11 to 0x13.",
    ),
    {
        # The simulated cursor is visible only to the C twin check (the oracle ring never reaches an unmapped span).
        **cascade(
            "comb-site-bytes",
            COMBINER,
            "    d3d8_pushbuffer_sim_site(sim, ENTRY, plan.count * 4u);",
            "    d3d8_pushbuffer_sim_site(sim, ENTRY, plan.count * 4u - 4u);",
            "the plan's reservation size decides where the roll-over falls.",
        ),
        "targets": [*T_STATE, "test_d3d8_dirty", ORACLE_CASCADE],
    },
    cascade(
        "comb-flags-written",
        COMBINER,
        "    d3d8_device_store32(D3D8_DEV_FLAGS, plan.flags_after);",
        "    d3d8_device_store32(D3D8_DEV_FLAGS, plan.flags_after ^ 1u);",
        "the device flags after the selection.",
    ),
    cascade(
        "ff-program-previous-operation",
        DIRTY,
        "(stage - 1u) * 0x80u + 0x30u) : 0u;",
        "(stage - 1u) * 0x80u + 0x34u) : 0u;",
        "a stage reads the colour operation of the stage below it.",
    ),
    cascade(
        "ff-program-bump-type",
        DIRTY,
        "    if (chained && previous_operation == 0x19u) return 6u;",
        "    if (chained && previous_operation == 0x19u) return 7u;",
        "operation 0x19 selects the bump type 6.",
    ),
    cascade(
        "ff-program-cube-type",
        DIRTY,
        "    if (chained && previous_operation == 0x1Au) return 7u;\n    if ((texture & 4u) != 0u) return 3u;",
        "    if (chained && previous_operation == 0x1Au) return 7u;\n    if ((texture & 4u) != 0u) return 2u;",
        "a cube map is type 3.",
    ),
    cascade(
        "ff-program-3d-type",
        DIRTY,
        "    if ((texture & 4u) != 0u) return 3u;\n    return ((texture & 0x40000000u) != 0u || (texture & 0xF0u) == 0x30u) ? 2u : 1u;",
        "    if ((texture & 4u) != 0u) return 3u;\n    return ((texture & 0x40000000u) != 0u || (texture & 0xF0u) == 0x20u) ? 2u : 1u;",
        "a volume format is type 2.",
    ),
    cascade(
        "ff-program-shift",
        DIRTY,
        "program = (program << 5) | fixed_function_stage_type",
        "program = (program << 4) | fixed_function_stage_type",
        "five bits per stage.",
    ),
    cascade(
        "transform-header",
        DIRTY,
        "#define TRANSFORM_PAIR 0x00040420u",
        "#define TRANSFORM_PAIR 0x00040424u",
        "the texture transform pair method.",
    ),
    {
        **cascade(
            "transform-stage-count",
            DIRTY,
            "    for (uint32_t stage = 0u; stage < 4u; stage++)\n        d3d8_pushbuffer_sim_site(sim, 0x003DE080u, shape_bytes(stage_transform_shape(stage)));",
            "    for (uint32_t stage = 0u; stage < 3u; stage++)\n        d3d8_pushbuffer_sim_site(sim, 0x003DE080u, shape_bytes(stage_transform_shape(stage)));",
            "each of the four stages reserves for itself.",
        ),
        "targets": [*T_STATE, "test_d3d8_dirty", ORACLE_CASCADE],
    },
    {
        **cascade(
            "transform-validate-in-plan",
            DIRTY,
            "    validate_texture_transforms();\n    for (uint32_t stage = 0u; stage < 4u; stage++)\n        d3d8_pushbuffer_sim_site",
            "    for (uint32_t stage = 0u; stage < 4u; stage++)\n        d3d8_pushbuffer_sim_site",
            "a refusal must precede every write of the cascade (T590: since T461 only a NaN matrix word reaches it, the garbage jump refuses in the shape lookup the plan loop runs anyway).",
        ),
        "targets": [*T_STATE, "test_d3d8_dirty", ORACLE_TRANSFORM_NAN],
    },
    cascade(
        "matrix-order",
        DIRTY,
        "    matrix_multiply(plan->first, world_view, projection);",
        "    matrix_multiply(plan->first, projection, world_view);",
        "the product's operand order.",
    ),
    cascade(
        "matrix-viewport",
        DIRTY,
        "    matrix_multiply(plan->second, plan->first, plan->viewport);",
        "    matrix_multiply(plan->second, plan->viewport, plan->first);",
        "the second product's operand order.",
    ),
    cascade(
        "matrix-add-order",
        DIRTY,
        "            term = a[row * 4u + 3u] * b[12u + column];",
        "            term = a[row * 4u + 3u] * b[8u + column];",
        "the fourth term of each product.",
    ),
    cascade(
        "matrix-transpose",
        DIRTY,
        "(column * 4u + row) * 4u, bits);",
        "(row * 4u + column) * 4u, bits);",
        "the packet carries the matrix column major.",
    ),
    cascade(
        "matrix-second-header",
        DIRTY,
        "#define PACKET_VIEWPORT 0x00400680u",
        "#define PACKET_VIEWPORT 0x00400480u",
        "the second packet's method.",
    ),
    cascade(
        "matrix-bytes",
        DIRTY,
        "#define MATRIX_PACKET_BYTES 68u",
        "#define MATRIX_PACKET_BYTES 66u",
        "two 17 dword packets.",
    ),
    cascade(
        "matrix-program-skip",
        DIRTY,
        "    return (dirty & 0x80000000u) == 0u && !d3d8_dirty_programmable();",
        "    return (dirty & 0u) == 0u && !d3d8_dirty_programmable();",
        "bit 31 of the mask skips the matrices.",
    ),
    {
        **cascade(
            "cascade-combiner-gate",
            RESOURCE,
            "    if ((dirty & 0x800u) != 0u && d3d8_device_load32(0x784u) == 0u)\n        dirty = d3d8_emit_fixed_function_combiner(dirty);",
            "    if ((dirty & 0x800u) != 0u)\n        dirty = d3d8_emit_fixed_function_combiner(dirty);",
            "a bound pixel shader returns the mask without the selection.",
        ),
        "targets": [*T_STATE, ORACLE_CASCADE, "pytest:tests/test_d3d8_cascade_refill_oracle.py"],
    },
    cascade(
        "cascade-plan-combiner-gate",
        RESOURCE,
        "    if ((dirty & 0x800u) != 0u && d3d8_device_load32(0x784u) == 0u)\n        dirty = d3d8_plan_fixed_function_combiner(dirty, sim);",
        "    if ((dirty & 0x800u) != 0u && d3d8_device_load32(0x784u) == 0u)\n        (void)d3d8_plan_fixed_function_combiner(dirty, sim);",
        "the plan must carry the mask the selection returns into the later families.",
    ),
    cascade(
        "cascade-order-transforms",
        RESOURCE,
        "    if ((dirty & 0x400u) != 0u) d3d8_emit_texture_transforms();\n    if ((dirty & 0xFF1000u) != 0u) (void)d3d8_emit_default_lighting_state(dirty);",
        "    if ((dirty & 0xFF1000u) != 0u) (void)d3d8_emit_default_lighting_state(dirty);\n    if ((dirty & 0x400u) != 0u) d3d8_emit_texture_transforms();",
        "the controller's order.",
    ),
    cascade(
        "cascade-mask-kept",
        RESOURCE,
        "    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty & 0xC0000070u);\n}\n\n/* 0x003DED80",
        "    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty & 0xC0000071u);\n}\n\n/* 0x003DED80",
        "the controller keeps 0xC0000070.",
    ),
]

MUTATIONS += [
    mutation(
        "begin-cascade",
        IMMEDIATE,
        "    d3d8_run_dirty_cascade();\n    const uint32_t cursor = d3d8_pushbuffer_begin();\n    d3d8_guest_store32(cursor, PUSH_BEGIN_END);\n    d3d8_guest_store32(cursor + 4u, primitive);",
        "    const uint32_t cursor = d3d8_pushbuffer_begin();\n    d3d8_guest_store32(cursor, PUSH_BEGIN_END);\n    d3d8_guest_store32(cursor + 4u, primitive);",
        "Begin runs the dirty cascade before it starts the primitive.",
        ORACLE_IMMEDIATE,
    ),
    mutation(
        "begin-flag",
        IMMEDIATE,
        "D3D8_DEV_FLAGS) | FLAG_IMMEDIATE);",
        "D3D8_DEV_FLAGS) | 0u);",
        "Begin marks the device as inside a primitive.",
        ORACLE_IMMEDIATE,
    ),
    mutation(
        "vertex-data-slot",
        IMMEDIATE,
        "PUSH_VERTEX_DATA_2F + slot * 8u);",
        "PUSH_VERTEX_DATA_2F + slot * 4u);",
        "the method advances eight bytes per vertex attribute.",
        ORACLE_IMMEDIATE,
    ),
    mutation(
        "vertex-data-order",
        IMMEDIATE,
        "    d3d8_guest_store32(cursor + 4u, x);\n    d3d8_guest_store32(cursor + 8u, y);",
        "    d3d8_guest_store32(cursor + 4u, y);\n    d3d8_guest_store32(cursor + 8u, x);",
        "the pair is x then y.",
        ORACLE_IMMEDIATE,
    ),
    mutation(
        "end-flags",
        IMMEDIATE,
        "flags & 0xFFFFE7FFu);",
        "flags & 0xFFFFEFFFu);",
        "End clears the immediate and the fence owed flags.",
        ORACLE_IMMEDIATE,
    ),
    mutation(
        "end-value",
        IMMEDIATE,
        "    d3d8_guest_store32(cursor + 4u, 0u);\n    d3d8_pushbuffer_end(cursor + 8u);\n    const uint32_t flags",
        "    d3d8_guest_store32(cursor + 4u, 1u);\n    d3d8_pushbuffer_end(cursor + 8u);\n    const uint32_t flags",
        "End writes primitive 0.",
        ORACLE_IMMEDIATE,
    ),
    mutation(
        "copy-snapshot-shader-copy",
        COPY,
        "        frame[3] = d3d8_guest_load32(GLOBAL_PS_TEXTURE_MODES);",
        "        frame[3] = 0u;",
        "the snapshot keeps the pixel shader texture modes.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-snapshot-shadow-count",
        COPY,
        "#define SHADOW_STATES 0x39u",
        "#define SHADOW_STATES 0x38u",
        "57 render state shadow words.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-snapshot-stage-one",
        COPY,
        "    frame[2] = d3d8_guest_load32(GLOBAL_STAGE1_OPERATION);",
        "    frame[2] = 1u;",
        "the snapshot keeps stage 1's colour operation.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-force",
        COPY,
        "        if ((state < 0x5C && (device_flags_byte() & FLAG_FORCE_STATES) != 0u) || value != shadow) {",
        "        if (value != shadow) {",
        "device flag 0x10 forces every immediate state out.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-stage-compare",
        COPY,
        "        if (value != d3d8_guest_load32(stage_state_address(state))) {\n            d3d8_set_texture_stage_state_notinline(0u, (int32_t)state, value);",
        "        if (value == d3d8_guest_load32(stage_state_address(state))) {\n            d3d8_set_texture_stage_state_notinline(0u, (int32_t)state, value);",
        "only the stage states that differ are written.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-filter-min",
        COPY,
        "        d3d8_set_texture_stage_state_notinline(0u, 4, filter);",
        "        d3d8_set_texture_stage_state_notinline(0u, 4, filter ^ 1u);",
        "the minification filter follows render state 0x7E.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-filter-mag",
        COPY,
        "        d3d8_set_texture_stage_state_notinline(0u, 3, filter);",
        "        d3d8_set_texture_stage_state_notinline(0u, 3, filter ^ 1u);",
        "the magnification filter follows render state 0x7E.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-stage-one-off",
        COPY,
        "        d3d8_set_texture_stage_state_notinline(1u, (int32_t)D3D8_TS_COLOR_OP, 1u);\n    }\n    d3d8_run_dirty_cascade();",
        "        d3d8_set_texture_stage_state_notinline(1u, (int32_t)D3D8_TS_COLOR_OP, 2u);\n    }\n    d3d8_run_dirty_cascade();",
        "stage 1 is disabled.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-program-count",
        COPY,
        "    push_program(COPY_PROGRAM, PROGRAM_DWORDS);\n    const uint32_t cursor",
        "    push_program(COPY_PROGRAM, PROGRAM_DWORDS - 4u);\n    const uint32_t cursor",
        "the two instruction vertex program is eight dwords.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-setup-raw",
        COPY,
        "0x00080394u, 0u, 0x4B7FFFFFu};",
        "0x00080394u, 1u, 0x4B7FFFFFu};",
        "the depth range raw packet.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-triangle-half-pixel",
        COPY,
        "        if (scale_x == two) offset_x = 0.5f;",
        "        if (scale_x == two) offset_x = 0.25f;",
        "a 2x scaled render target samples half a texel in.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-triangle-filter-gate",
        COPY,
        "    if (minification == 4u || minification == 5u) {",
        "    if (minification == 4u) {",
        "filters 4 and 5 apply the half texel offset.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-triangle-width",
        COPY,
        "                                      : (size & 0xFFFu) + 1u;",
        "                                      : (size & 0xFFFu);",
        "the linear surface width is the size field plus one.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-triangle-height-shift",
        COPY,
        "                                       : ((size >> 12) & 0xFFFu) + 1u;",
        "                                       : ((size >> 11) & 0xFFFu) + 1u;",
        "the linear surface height field.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-triangle-vertex-scale",
        COPY,
        "(long double)four +\n                                    (long double)offset_x),",
        "(long double)two +\n                                    (long double)offset_x),",
        "the second vertex is four widths along.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-triangle-primitive",
        COPY,
        "    d3d8_begin(5u);",
        "    d3d8_begin(6u);",
        "a triangle list.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-shader",
        COPY,
        "    (void)d3d8_set_pixel_shader_v(frame[0]);\n    const uint32_t flags",
        "    (void)d3d8_set_pixel_shader_v(0u);\n    const uint32_t flags",
        "the pixel shader is rebound.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-force-condition",
        COPY,
        "        if ((state > 0x5B || (device_flags_byte() & FLAG_FORCE_STATES) == 0u) &&",
        "        if ((state > 0x5B || (device_flags_byte() & FLAG_FORCE_STATES) != 0u) &&",
        "an immediate state is skipped while device flag 0x10 forces them.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-shadow-skip",
        COPY,
        "                if (d3d8_guest_load32(definition + 0x20u) != 0u ||",
        "                if (d3d8_guest_load32(definition + 0x24u) != 0u ||",
        "states 8 and 9 are skipped only for a shader without inputs.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-mode-state",
        COPY,
        "            d3d8_set_render_state_notinline(0x88, frame[3]);",
        "            d3d8_set_render_state_notinline(0x89, frame[3]);",
        "the pixel shader texture modes go back through state 0x88.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-program-source",
        COPY,
        "        push_program(D3D8_DEVICE_BASE + DEV_SAVED_PROGRAM, PROGRAM_DWORDS);",
        "        push_program(COPY_PROGRAM, PROGRAM_DWORDS);",
        "the restore reloads the program saved at device+0x10A8.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-programmable-tail",
        COPY,
        "    if ((shader_flags & 0x12u) != 0u) {\n        d3d8_guest_store32(cursor, 0x00081E94u);",
        "    if ((shader_flags & 0x12u) == 0u) {\n        d3d8_guest_store32(cursor, 0x00081E94u);",
        "a vertex shader selects the programmable tail.",
        ORACLE_COPY,
    ),
    mutation(
        "copy-restore-viewport",
        COPY,
        "        end = d3d8_viewport_emit(cursor + 8u, shader_flags);",
        "        end = d3d8_viewport_emit(cursor + 12u, shader_flags);",
        "the viewport packets follow the execution mode pair.",
        ORACLE_COPY,
    ),
    mutation(
        "present-copy-setup",
        PRESENT,
        "            d3d8_copy_setup();",
        "            (void)0;",
        "flag 1 forces the copy state.",
        ORACLE_SWAP,
    ),
    mutation(
        "present-copy-restore",
        PRESENT,
        "        d3d8_copy_restore(copy_frame);",
        "        (void)copy_frame;",
        "flag 1 restores the state.",
        ORACLE_SWAP,
    ),
    mutation(
        "present-copy-triangle",
        PRESENT,
        "        d3d8_copy_draw_triangle();",
        "        (void)0;",
        "flag 1 draws the copy triangle.",
        ORACLE_SWAP,
    ),
    {
        # T536: re-derived on the T449 form (`0x300u | (scale != 1.0 ? 0x10Fu : 0u)`). The old mutant
        # (0x300 to 0x100) is now EQUIVALENT: d3d8_rebuild_viewport_matrix, which T449 runs inside
        # SetRenderTarget, ORs 0x200 itself, so only the 0x100 bit (point parameters) is still this
        # line's own. The mutant drops it (0x300 to 0x200). It no longer targets ORACLE_SWAP: that suite
        # has failed at baseline since T449 (the port's SetRenderTarget now writes its 75 dword stream
        # into the composition, the suite's expectation has none), so a kill by it proved nothing.
        # test_d3d8_bind (from a cleared mask) and the SetRenderTarget and bind oracles pass clean.
        "id": "t443-bind-render-target-dirty",
        "file": BIND,
        "old": "    or_dirty_mask(0x300u);",
        "new": "    or_dirty_mask(0x200u);",
        "targets": ["test_d3d8_bind", ORACLE_SRT, ORACLE_BIND],
        "why": "SetRenderTarget leaves 0x100 (point parameters, from the SetViewport it runs) on top of the 0x200 the matrix rebuild sets.",
    },
    {
        "id": "t443-bind-render-target-dirty-scaled",
        "file": "src/gpu/d3d8_scaled_viewport.c",
        "old": "d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x10Fu);",
        "new": "d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x100u);",
        "targets": ["test_d3d8_bind", ORACLE_SRT, ORACLE_BIND],
        "why": "T536, added with the re-anchor above: a SetRenderTarget whose sample scale changes also marks 0x10F (the viewport and projection words), which the sample mode cases of T449 reach. T533: the OR moved into d3d8_scaled_viewport_store, which 0x9A and 0x9B share.",
    },
    mutation(
        "copy-title-restore-shader",
        COPY,
        "    (void)d3d8_set_pixel_shader_v(frame[0]);\n    const uint32_t flags",
        "    const uint32_t flags",
        "the title's bound pixel shader survives the composition.",
        ORACLE_TITLE,
    ),
]
