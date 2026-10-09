# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for T533 (a), render states 0x9A and 0x9B and the piece SetRenderTarget shares with them.

Targets: the scaled viewport and clip recomputation 0x003D7B80 (src/gpu/d3d8_scaled_viewport.c), its
SetViewport(NULL) tail (SetScissors with count 0 in src/gpu/d3d8_scissor.c, the planned matrix check in
src/gpu/d3d8_viewport_matrix.c) and the two state helpers (src/gpu/d3d8_state.c). Every mutation is
covered by `tests/test_d3d8_scaled_viewport_oracle.py`, which replays the ORIGINAL bytes under the oracle,
the ctest binary beside it is the compile guard (a mutation that does not compile would make the pytest
runner build fail, which reads like a kill).

EQUIVALENT MUTANTS DROPPED (a survivor that cannot be killed is not evidence): the byte count of the LAST planned
site (the viewport packets, `viewport_packet_bytes`): nothing is planned after it, so the count only feeds a
mapping check of a span the oracle's ring always satisfies.

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror` turns
the latter into NOT-A-MUTANT, which reads like evidence.
"""

SCALED = "src/gpu/d3d8_scaled_viewport.c"
STATE = "src/gpu/d3d8_state.c"
SCISSOR = "src/gpu/d3d8_scissor.c"
MATRIX = "src/gpu/d3d8_viewport_matrix.c"
BIND = "src/gpu/d3d8_bind.c"
ORACLE = "pytest:tests/test_d3d8_scaled_viewport_oracle.py"
FAST = f'{ORACLE} -k "state_9a_at or state_9b_away"'
ROLLS = f'{ORACLE} -k "rolls_over or flag_4"'
REFUSALS = f'{ORACLE} -k "refuses or refuse or nan or unmapped"'
BIND_ORACLE = "pytest:tests/test_d3d8_bind_oracle.py"
SRT_ORACLE = "pytest:tests/test_d3d8_set_render_target_oracle.py"


def mutation(
    identifier: str, file: str, old: str, new: str, why: str, tests: list[str] | None = None
) -> dict:
    return {
        "id": f"t533-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": ["test_d3d8_state", *(tests or [FAST, ORACLE])],
        "why": why,
    }


MUTATIONS: list[dict] = [
    # --- the branch and the sizes -------------------------------------------------------------------------------
    mutation(
        "branch-condition",
        SCALED,
        "    const bool first_back_buffer = target == d3d8_device_load32(DEV_BACK_BUFFER);\n    uint32_t width;",
        "    const bool first_back_buffer = target != d3d8_device_load32(DEV_BACK_BUFFER);\n    uint32_t width;",
        "the first back buffer sizes from the front buffer and the sample scales, any other target from itself.",
    ),
    mutation(
        "sizes-from-the-front-buffer",
        SCALED,
        "        const uint32_t front = d3d8_device_load32(DEV_FRONT_BUFFER);\n        const uint32_t back = d3d8_device_load32(DEV_BACK_BUFFER);",
        "        const uint32_t front = d3d8_device_load32(DEV_BACK_BUFFER);\n        const uint32_t back = d3d8_device_load32(DEV_BACK_BUFFER);",
        "the scaled size is the front buffer's (+0x1A18), clamped to the back buffer's.",
    ),
    mutation(
        "width-clamp",
        SCALED,
        "        width = scaled_width < back_width ? scaled_width : back_width;",
        "        width = scaled_width + (back_width & 0u);",
        "the scaled width never exceeds the back buffer's.",
    ),
    mutation(
        "height-clamp",
        SCALED,
        "        height = scaled_height < back_height ? scaled_height : back_height;",
        "        height = scaled_height + (back_height & 0u);",
        "the scaled height never exceeds the back buffer's.",
    ),
    mutation(
        "height-uses-the-y-scale",
        SCALED,
        "surface_height(front), scale_y_word, half, two_pow_32);",
        "surface_height(front), scale_x_word, half, two_pow_32);",
        "the height scales by the y sample scale (+0x970).",
    ),
    mutation(
        "width-uses-the-x-scale",
        SCALED,
        "surface_width(front), scale_x_word, half, two_pow_32);",
        "surface_width(front), scale_y_word, half, two_pow_32);",
        "the width scales by the x sample scale (+0x96C).",
    ),
    mutation(
        "offscreen-x-scale",
        SCALED,
        "        scale_x_word = FLOAT_ONE_WORD;",
        "        scale_x_word = 0x3F000000u;",
        "an offscreen target starts at the x scale 1.0 (an immediate in the original).",
    ),
    mutation(
        "offscreen-y-scale",
        SCALED,
        "        scale_y_value = word_float(one);",
        "        scale_y_value = word_float(half) + 0.0L * word_float(one);",
        "an offscreen target starts at the y scale of the .rdata constant 1.0.",
    ),
    mutation(
        "size-word",
        SCALED,
        "    return size != 0u ? (size & 0xFFFu) + 1u\n",
        "    return size != 0u ? (size & 0xFFFu)\n",
        "a size word holds width minus one.",
    ),
    mutation(
        "height-exponent",
        SCALED,
        ">> 24) & 0xFu);\n}\n\nstatic uint32_t constant",
        ">> 20) & 0xFu);\n}\n\nstatic uint32_t constant",
        "a header without a size word has the height exponent in bits 24 to 27.",
    ),
    # --- the sample mode ----------------------------------------------------------------------------------------
    mutation(
        "swizzled-skips-the-mode",
        SCALED,
        "    if (mode != 0u && (format & 0x200u) == 0u) {",
        "    if (mode != 0u && (format & 0x200u) != 0u) {",
        "a swizzled target (surface format bit 0x200) is never halved.",
    ),
    mutation(
        "mode-zero",
        SCALED,
        "    if (mode != 0u && (format & 0x200u) == 0u) {",
        "    if ((format & 0x200u) == 0u) {",
        "mode 0 leaves the sizes and scales alone.",
    ),
    mutation(
        "sampled-flag",
        SCALED,
        "        flags |= DEV_DEVICE_FLAGS_SAMPLED;\n",
        "",
        "the sample mode sets device flag 0x8000.",
    ),
    mutation(
        "width-rounds-up",
        SCALED,
        "        width = (width + 1u) >> 1;",
        "        width >>= 1;",
        "the halved width rounds up.",
    ),
    mutation(
        "height-rounds-up",
        SCALED,
        "            height = (height + 1u) >> 1;",
        "            height >>= 1;",
        "the halved height rounds up.",
    ),
    mutation(
        "mode-two-halves-y",
        SCALED,
        "        if (mode == 2u) {\n            scale_y_value *= half_value;",
        "        if (mode == 3u) {\n            scale_y_value *= half_value;",
        "only mode 2 halves the height and the y scale.",
    ),
    mutation(
        "x-scale-halved",
        SCALED,
        "        scale_x_word = float_word(word_float(scale_x_word) * half_value);",
        "        scale_x_word = float_word(word_float(scale_x_word));",
        "every sample mode halves the x scale.",
    ),
    mutation(
        "y-scale-halved",
        SCALED,
        "            scale_y_value *= half_value;",
        "            scale_y_value *= 1.0L;",
        "mode 2 halves the y scale.",
    ),
    mutation(
        "format-bit-two",
        SCALED,
        "            format |= 0x2000u;\n",
        "            format |= 0x1000u;\n",
        "mode 2 marks the surface format 0x2000.",
    ),
    mutation(
        "format-bit-one",
        SCALED,
        "            format |= 0x1000u;\n        }\n    }",
        "            format |= 0x2000u;\n        }\n    }",
        "the other sample modes mark the surface format 0x1000.",
    ),
    # --- the sample scale, the table, the point packet ----------------------------------------------------------
    mutation(
        "sample-scale-is-the-smaller",
        SCALED,
        "    const uint32_t sample_word = scale_x_value < scale_y_value ? scale_x_word : scale_y_word;",
        "    const uint32_t sample_word = scale_x_value > scale_y_value ? scale_x_word : scale_y_word;",
        "the sample scale is the smaller of the two scales.",
    ),
    mutation(
        "changed-test",
        SCALED,
        "    plan->changed = !(word_float(d3d8_device_load32(DEV_SCALE_LAST)) == word_float(sample_word));",
        "    plan->changed = (word_float(d3d8_device_load32(DEV_SCALE_LAST)) == word_float(sample_word));",
        "the sample packet and the table word follow the scale CHANGING.",
    ),
    mutation(
        "changed-compares-the-x-scale",
        SCALED,
        "    plan->changed = !(word_float(d3d8_device_load32(DEV_SCALE_LAST)) == word_float(sample_word));",
        "    plan->changed = !(word_float(d3d8_device_load32(DEV_SCALE_LAST)) == word_float(scale_x_word));",
        "the old sample scale is compared with the new SAMPLE scale, not the x scale.",
    ),
    mutation(
        "table-index-doubles",
        SCALED,
        "        const uint32_t index = truncated(sample + sample + half_value);",
        "        const uint32_t index = truncated(sample + half_value);",
        "the table index is trunc(2 * scale + 0.5).",
    ),
    mutation(
        "table-stride",
        SCALED,
        "        const uint32_t address = SCALE_TABLE + index * 4u;",
        "        const uint32_t address = SCALE_TABLE + index * 2u;",
        "the table holds one dword per index.",
    ),
    mutation(
        "table-refusal",
        SCALED,
        "        if (!kernel_guest_read_bytes(address, &plan->table_word, sizeof(plan->table_word))) {",
        "        if (!kernel_guest_read_bytes(address, &plan->table_word, sizeof(plan->table_word)) && false) {",
        "an unmapped table word is refused before any write.",
        [ORACLE],
    ),
    mutation(
        "point-clamp",
        SCALED,
        "        plan->method_380 = clamp > POINT_LIMIT ? POINT_LIMIT : clamp;",
        "        plan->method_380 = clamp;",
        "the point scale packet is clamped to 0x1FF.",
    ),
    mutation(
        "point-uses-the-sample-scale",
        SCALED,
        "        const uint32_t clamp = truncated(point * sample * word_float(eight) + half_value);",
        "        const uint32_t clamp = truncated(point * word_float(eight) + half_value);",
        "the point scale packet scales by the sample scale.",
    ),
    mutation(
        "point-multiplier",
        SCALED,
        "        const uint32_t clamp = truncated(point * sample * word_float(eight) + half_value);",
        "        const uint32_t clamp = truncated(point * sample * word_float(one) + half_value + 0.0L * word_float(eight));",
        "the point scale packet multiplies by the constant 8.0.",
    ),
    mutation(
        "multisample-mask-antialias",
        SCALED,
        "    if ((flags & DEV_DEVICE_FLAGS_SAMPLED) != 0u && d3d8_guest_load32(GLOBAL_ANTIALIAS) != 0u) {",
        "    if ((flags & DEV_DEVICE_FLAGS_SAMPLED) != 0u || d3d8_guest_load32(GLOBAL_ANTIALIAS) != 0u) {",
        "bit 0 of the 0x41D7C word needs the sampled flag AND the antialias state.",
    ),
    mutation(
        "multisample-mask-shift",
        SCALED,
        "    uint32_t mask = (d3d8_guest_load32(GLOBAL_SAMPLE_ALPHA_HIGH) << 16) |",
        "    uint32_t mask = (d3d8_guest_load32(GLOBAL_SAMPLE_ALPHA_HIGH) << 8) |",
        "the high sample alpha word sits in the upper half of the 0x41D7C word.",
    ),
    # --- the stores ---------------------------------------------------------------------------------------------
    mutation(
        "table-word-store",
        SCALED,
        "        d3d8_device_store32(DEV_SCALE_TABLE, plan->table_word);\n",
        "",
        "the table word is stored at +0x968 when the sample scale changed.",
    ),
    mutation(
        "sample-scale-store",
        SCALED,
        "        d3d8_device_store32(DEV_SCALE_LAST, plan->sample_scale);\n",
        "",
        "the sample scale is stored at +0x964 when it changed.",
    ),
    mutation(
        "dirty-bits",
        SCALED,
        "d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x10Fu);",
        "d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x10Eu);",
        "a changed sample scale marks 0x10F.",
    ),
    mutation(
        "dirty-only-when-changed",
        SCALED,
        "    if (plan->changed) {\n        d3d8_device_store32(DEV_SCALE_LAST",
        "    if (plan->changed || true) {\n        d3d8_device_store32(DEV_SCALE_LAST",
        "an unchanged sample scale writes neither the scale, the table word nor the dirty bits.",
    ),
    mutation(
        "height-store",
        SCALED,
        "    d3d8_device_store32(DEV_TARGET_HEIGHT, plan->height);\n",
        "",
        "the clip height is stored at +0x958.",
    ),
    mutation(
        "flags-store",
        SCALED,
        "    d3d8_device_store32(D3D8_DEV_FLAGS, plan->device_flags);\n",
        "",
        "the device flags are stored.",
    ),
    # --- the run: the plan, the packets, the tail ---------------------------------------------------------------
    mutation(
        "first-site-bytes",
        SCALED,
        "    d3d8_pushbuffer_sim_site(&sim, ENTRY, plan.changed ? 16u : 8u);",
        "    d3d8_pushbuffer_sim_site(&sim, ENTRY, 8u);",
        "the first site writes 16 bytes when the sample scale changed.",
        [ROLLS, ORACLE],
    ),
    mutation(
        "second-site-bytes",
        SCALED,
        "    d3d8_pushbuffer_sim_site(&sim, ENTRY, 8u);\n    d3d8_plan_vertex_program_helper(&sim);",
        "    d3d8_pushbuffer_sim_site(&sim, ENTRY, 4u);\n    d3d8_plan_vertex_program_helper(&sim);",
        "the 0x41D7C pair is 8 bytes.",
        [ROLLS, ORACLE],
    ),
    mutation(
        "helper-plan",
        SCALED,
        "    d3d8_plan_vertex_program_helper(&sim);\n    d3d8_plan_viewport_scissor(&sim);",
        "    d3d8_plan_viewport_scissor(&sim);",
        "the vertex program helper reserves between the packet pairs and the scissor.",
        [ROLLS, ORACLE],
    ),
    mutation(
        "scissor-plan",
        SCALED,
        "    d3d8_plan_viewport_scissor(&sim);\n    d3d8_rebuild_viewport_matrix_check_scaled",
        "    d3d8_rebuild_viewport_matrix_check_scaled",
        "the scissor of SetViewport(NULL) is a site of the plan.",
        [ROLLS, ORACLE],
    ),
    mutation(
        "matrix-check",
        SCALED,
        "    d3d8_rebuild_viewport_matrix_check_scaled(plan.scale_x, plan.scale_y);\n    refuse_nan_offset",
        "    refuse_nan_offset",
        "a NaN the viewport matrix would reject is refused before the first write.",
        [REFUSALS, ORACLE],
    ),
    mutation(
        "offset-check",
        SCALED,
        "    refuse_nan_offset(0xEF8u);\n",
        "",
        "a NaN viewport offset is refused before the first write.",
        [REFUSALS, ORACLE],
    ),
    mutation(
        "state-store",
        SCALED,
        "    d3d8_guest_store32(state_address, mode);\n    d3d8_scaled_viewport_store(&plan);",
        "    (void)state_address;\n    d3d8_scaled_viewport_store(&plan);",
        "the caller's state global takes the mode.",
    ),
    mutation(
        "sample-packet",
        SCALED,
        "    if (plan.changed) {\n        d3d8_guest_store32(cursor, 0x00040380u);",
        "    if (plan.changed && false) {\n        d3d8_guest_store32(cursor, 0x00040380u);",
        "the point scale packet follows the 0x40208 pair when the sample scale changed.",
    ),
    mutation(
        "surface-format-header",
        SCALED,
        "    d3d8_guest_store32(cursor, 0x00040208u);",
        "    d3d8_guest_store32(cursor, 0x0004020Cu);",
        "the surface format packet is method 0x40208.",
    ),
    mutation(
        "mask-packet",
        SCALED,
        "    d3d8_guest_store32(cursor + 4u, plan.method_41d7c);",
        "    d3d8_guest_store32(cursor + 4u, plan.method_380);",
        "the multisample mask packet carries the 0x41D7C word.",
    ),
    mutation(
        "helper-run",
        SCALED,
        "    d3d8_run_vertex_program_helper();\n",
        "",
        "a programmable declaration runs the vertex program helper.",
        [ORACLE],
    ),
    mutation(
        "scissor-run",
        SCALED,
        "    d3d8_set_viewport_scissor(&scissor);\n",
        "    (void)scissor;\n",
        "SetViewport(NULL) sets the scissor of the viewport rectangle.",
    ),
    mutation(
        "matrix-run",
        SCALED,
        "    (void)d3d8_rebuild_viewport_matrix();\n    cursor = d3d8_pushbuffer_begin();\n    d3d8_pushbuffer_end(d3d8_viewport_emit",
        "    cursor = d3d8_pushbuffer_begin();\n    d3d8_pushbuffer_end(d3d8_viewport_emit",
        "SetViewport(NULL) rebuilds the viewport matrix.",
    ),
    mutation(
        "viewport-emit",
        SCALED,
        "    d3d8_pushbuffer_end(d3d8_viewport_emit(cursor, declaration_flags));\n}",
        "    d3d8_pushbuffer_end(cursor);\n}",
        "SetViewport(NULL) writes the viewport packets.",
    ),
    # --- the refusals -------------------------------------------------------------------------------------------
    mutation(
        "scale-finite-check",
        SCALED,
        "        if (!finite_word(scale_x_word) || !finite_word(scale_y_word)) {",
        "        if ((!finite_word(scale_x_word) || !finite_word(scale_y_word)) && false) {",
        "a NaN or infinite sample scale is refused before any write.",
        [REFUSALS, ORACLE],
    ),
    mutation(
        "scale-finite-check-x-only",
        SCALED,
        "        if (!finite_word(scale_x_word) || !finite_word(scale_y_word)) {",
        "        if (!finite_word(scale_x_word)) {",
        "the y sample scale is checked too.",
        [REFUSALS, ORACLE],
    ),
    mutation(
        "constant-nan-check",
        SCALED,
        '    if (!finite_word(word)) {\n        d3d8_hle_fatal(ENTRY, "float constant',
        '    if (!finite_word(word) && false) {\n        d3d8_hle_fatal(ENTRY, "float constant',
        "a NaN float constant of .rdata is refused.",
        [REFUSALS, ORACLE],
    ),
    mutation(
        "nan-offset-test",
        SCALED,
        "    if (!finite_word(word) && (word & 0x7FFFFFFFu) != 0x7F800000u) {",
        "    if (!finite_word(word)) {",
        "an infinite viewport offset is not a NaN: only NaN payloads are refused (equivalence of an infinity is MEASURED).",
        [ORACLE],
    ),
    # --- the state helpers --------------------------------------------------------------------------------------
    mutation(
        "state-9a-condition",
        STATE,
        "void d3d8_state_library_set_9a(uint32_t value)\n{\n    if (d3d8_device_load32(DEV_RENDER_TARGET) == d3d8_device_load32(DEV_BACK_BUFFER_0)) {\n        d3d8_scaled_viewport_run",
        "void d3d8_state_library_set_9a(uint32_t value)\n{\n    if (d3d8_device_load32(DEV_RENDER_TARGET) != d3d8_device_load32(DEV_BACK_BUFFER_0)) {\n        d3d8_scaled_viewport_run",
        "0x9A runs the recompute at the first back buffer.",
    ),
    mutation(
        "state-9b-condition",
        STATE,
        "    if (d3d8_device_load32(DEV_RENDER_TARGET) != d3d8_device_load32(DEV_BACK_BUFFER_0)) {\n        d3d8_scaled_viewport_run(value, D3D8_STATE_9B);",
        "    if (d3d8_device_load32(DEV_RENDER_TARGET) == d3d8_device_load32(DEV_BACK_BUFFER_0)) {\n        d3d8_scaled_viewport_run(value, D3D8_STATE_9B);",
        "0x9B runs the recompute away from the first back buffer.",
    ),
    mutation(
        "state-9a-global",
        STATE,
        "        d3d8_scaled_viewport_run(value, D3D8_STATE_9A);",
        "        d3d8_scaled_viewport_run(value, D3D8_STATE_9B);",
        "0x9A stores its own state global.",
    ),
    mutation(
        "state-9b-global",
        STATE,
        "        d3d8_scaled_viewport_run(value, D3D8_STATE_9B);",
        "        d3d8_scaled_viewport_run(value, D3D8_STATE_9A);",
        "0x9B stores its own state global.",
    ),
    mutation(
        "state-9a-store-only",
        STATE,
        "    store_state(D3D8_STATE_9A, value);\n}\n\n/* 0x003D7EE0. The packet first",
        "}\n\n/* 0x003D7EE0. The packet first",
        "0x9A off the first back buffer stores.",
    ),
    mutation(
        "state-9b-store-only",
        STATE,
        "        return;\n    }\n    store_state(D3D8_STATE_9B, value);\n}",
        "        return;\n    }\n}",
        "0x9B at the first back buffer stores.",
    ),
    # --- SetViewport(NULL): SetScissors with count 0 ------------------------------------------------------------
    mutation(
        "scissor-width-word",
        SCISSOR,
        "    const uint32_t packet[9]={0x80200u,(width<<16u)|left,(height<<16u)|top,0x402B4u,0u,0x402C0u,\n        in->width<<16u,0x402E0u,in->height<<16u};\n    const uint32_t cache[2]={0u,0u};",
        "    const uint32_t packet[9]={0x80200u,(width<<15u)|left,(height<<16u)|top,0x402B4u,0u,0x402C0u,\n        in->width<<16u,0x402E0u,in->height<<16u};\n    const uint32_t cache[2]={0u,0u};",
        "the viewport scissor packs width and left.",
    ),
    mutation(
        "scissor-clip-size",
        SCISSOR,
        "        in->width<<16u,0x402E0u,in->height<<16u};\n    const uint32_t cache[2]={0u,0u};",
        "        in->height<<16u,0x402E0u,in->height<<16u};\n    const uint32_t cache[2]={0u,0u};",
        "the clip width words come from the clip size at +0x954.",
    ),
    mutation(
        "scissor-cache-count",
        SCISSOR,
        "    const uint32_t cache[2]={0u,0u};\n    if (d3d8_pushbuffer_begin() != start)",
        "    const uint32_t cache[2]={1u,0u};\n    if (d3d8_pushbuffer_begin() != start)",
        "count 0 leaves the cache count 0 (+0x1C00).",
    ),
    mutation(
        "scissor-top-scale",
        SCISSOR,
        "    const uint32_t top=d3d8_scaled_integer(viewport[1],in->scale_y,bias,adjust);",
        "    const uint32_t top=d3d8_scaled_integer(viewport[1],in->scale_x,bias,adjust);",
        "the top scales by the y scale.",
    ),
    mutation(
        "scissor-height-scale",
        SCISSOR,
        "    const uint32_t height=d3d8_scaled_integer(viewport[3],in->scale_y,bias,adjust);",
        "    const uint32_t height=d3d8_scaled_integer(viewport[3],in->scale_x,bias,adjust);",
        "the height scales by the y scale.",
    ),
    mutation(
        "scissor-unsigned-adjust",
        SCISSOR,
        "    const uint32_t negative = (value & 0x80000000u) != 0u;",
        "    const uint32_t negative = (value & 0x80000000u) != 0u && false;",
        "a negative viewport word adds 2^32 after the fild (the original's `jge`).",
    ),
    mutation(
        "scissor-plan-site",
        SCISSOR,
        "    const uint32_t start=d3d8_pushbuffer_sim_site(sim,ENTRY,36u);",
        "    const uint32_t start=d3d8_pushbuffer_sim_site(sim,ENTRY,32u);",
        "the viewport scissor writes 36 bytes.",
        [ROLLS, ORACLE],
    ),
    # --- the planned matrix check -------------------------------------------------------------------------------
    mutation(
        "matrix-check-uses-the-new-scales",
        MATRIX,
        "    if(override_scales){memcpy(device+0x95Cu,&scale_x,4u);memcpy(device+0x960u,&scale_y,4u);}",
        "    if(override_scales && false){memcpy(device+0x95Cu,&scale_x,4u);memcpy(device+0x960u,&scale_y,4u);}",
        "the matrix check runs over the scales about to be stored, not the stale ones.",
        [ORACLE],
    ),
    # --- SetRenderTarget, which shares the piece ----------------------------------------------------------------
    mutation(
        "bind-sample-packet",
        BIND,
        "    if (plan.changed) {\n        /* Insert the sample-control pair",
        "    if (plan.changed && false) {\n        /* Insert the sample-control pair",
        "SetRenderTarget emits the point scale packet when the sample scale changed.",
        [SRT_ORACLE, BIND_ORACLE],
    ),
    mutation(
        "bind-sample-value",
        BIND,
        "        stream[57] = plan.method_380;",
        "        stream[57] = 4u;",
        "SetRenderTarget emits the point scale the recompute computed.",
        [SRT_ORACLE, BIND_ORACLE],
    ),
    mutation(
        "bind-surface-format",
        BIND,
        "    const uint32_t method_208 = plan.method_208;",
        "    const uint32_t method_208 = plan.method_208 & ~0x3000u;",
        "SetRenderTarget emits the surface format with the sample bits the recompute adds.",
        [SRT_ORACLE, BIND_ORACLE],
    ),
    mutation(
        "bind-multisample-mask",
        BIND,
        "    const uint32_t multisample_mask = plan.method_41d7c;",
        "    const uint32_t multisample_mask = plan.method_41d7c & ~1u;",
        "SetRenderTarget emits the multisample mask the recompute computed.",
        [SRT_ORACLE, BIND_ORACLE],
    ),
]
