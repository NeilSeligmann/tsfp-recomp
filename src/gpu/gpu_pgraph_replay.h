/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay of the draw list gpu_pgraph.h decoded (T84), through gpu_vsh_draw (T100d), into an RGBA8
 * target on a real Vulkan device. The pieces that need no device (program resolution, vertex
 * assembly, primitive expansion) are separate functions so they are testable without one.
 *
 * WHAT A DRAW NEEDS AND WHERE EACH PIECE COMES FROM
 *
 *   program    the 16-byte instructions uploaded into the program file (0x1E9C + 0x0B00 run), read
 *              from the START slot (0x1EA0) up to the first instruction with the FINAL bit
 *              (dword 3 bit 0, MEASURED: every one of the 46 static and 7,808 generated programs
 *              has it on its last instruction and on no other). The SHA-256 of
 *              `0x2078, instruction count (two u16), instructions` names the module, because the
 *              corpus names its modules by exactly that digest (tools/nv2a/corpus.py). The module
 *              is then found through the T95 selector table (gpu_vsh_lookup_name over
 *              `static_<digest>` first, then `generated_<digest>`). No hit is a refusal that prints
 *              the digest; there is no nearest match.
 *   constants  the 192 rows written through 0x1EA4 + 0x0B80, unwritten rows are 0.
 *   attributes per slot n: format and guest address from the 16-slot arrays, the vertex bytes
 *              from the draw's vertex snapshot when the decoder took one (T262,
 *              gpu_pgraph_set_vertex_capture: the guest bytes as they stood when the draw ended in the
 *              decoder), else fetched through the backend's read_guest AT THE REPLAY (the pushbuffer
 *              holds only addresses), which sees whatever the guest wrote since.
 *   topology   the BEGIN_END operation expanded to a triangle list, a point list (POINTS) or a line
 *              list (LINES, LINE_STRIP), the three lists gpu_vsh_render draws (T84b). LINE_LOOP,
 *              QUAD_STRIP and POLYGON are refused: nothing in the title reaches them (6.1).
 *
 * INFERENCES. Each is something the replay needs and nothing in the repository measured. A draw
 * that needs one is REFUSED unless the caller's `allowed_inferences` has the bit, and the report
 * says which were used. Nothing is silently assumed.
 *
 *   PROGRAM_HEADER       the u16 the title's upload block carries before the count is not in the
 *                        stream. Measured: 0x2078 on every program found in the image and every
 *                        generated one, so the digest is built with it.
 *   VIEWPORT_CONSTANTS   0x0AF0 (scale) feeds hardware constant 58 and 0x0A20 (offset) constant
 *                        59. The library emits those two methods, and the programs read constants
 *                        58 and 59 as the viewport (docs/vertex-translator.md 10.2), but nothing
 *                        measured that the hardware registers ARE those rows. Skipped when the
 *                        stream wrote the rows itself.
 *   COMPONENT_DEFAULTS   components a vertex array does not supply, and disabled slots, are
 *                        (0, 0, 0, 1).
 *   D3DCOLOR_ORDER       a UB_D3D size-4 attribute is the D3DCOLOR 0xAARRGGBB, so its bytes in
 *                        memory are B, G, R, A and the program sees (R, G, B, A) / 255.
 *   POINT_SIZE           (T84b) a POINTS draw is rasterized by Vulkan's point rule at the size the
 *                        PROGRAM writes to oPts.x (gl_PointSize). Nothing measured that the NV2A takes
 *                        its point size from the program, from its own register, or what its point
 *                        covers, and the device has no largePoints so only 1.0 is defined.
 *   LINE_WIDTH           (T84b) a LINES or LINE_STRIP draw is rasterized by Vulkan's line rule at one
 *                        pixel, because the backend gave no `line_width`. The title's line-width
 *                        state is not measured. An explicit `line_width` of 1.0 needs no inference,
 *                        any other value is refused (the device has no wideLines).
 *   S32K_UNNORMALISED    an S32K size-2 attribute is two signed 16-bit integers, each converted to
 *                        its own value as a float (no division). MEASURED: the title's builder
 *                        0x1E970 writes S32K only as size 2 at v0, and v0 is read by NOTHING but
 *                        ARL (lane x) in all 2,570 programs that read it, a normalised [-1, 1]
 *                        index would floor to -1 or 0. INFERRED: the scale and the sign (the type
 *                        name and the xemu source, never the image).
 *
 * OUTPUT STATE (T267, OPT-IN, default off). With `backend.output_groups` naming a group AND the model told to
 * decode it (gpu_pgraph_set_output_groups), the state a draw's snapshot carries is applied through a
 * gpu_vsh_output instead of being counted as an unhandled method. Both off, the draw is byte for byte what
 * it always was. A stream that WROTE a group's state while the backend has not enabled the group is REFUSED
 * (the replay would otherwise ignore state the title set). What each group rests on, and what it refuses:
 *
 *   SCISSOR  MEASURED: the emitter and the value layout. The port of SetScissors (0x003D4470,
 *            d3d8_scissor.c, checked word for word against the original under emulation) writes the
 *            rectangle as 0x0200 = x | (right - left) << 16 and 0x0204 = y | (bottom - top) << 16, a window
 *            clip type 0x02B4 = 0 and window clip entry 0 (0x02C0 = fields[0] << 16, 0x02E0 = fields[1] << 16,
 *            xmin = ymin = 0), and only for one inclusive rectangle. The replay applies the rectangle as the
 *            scissor in target pixels, y down from row 0 (mirrored under flip_y, so the rectangle is the
 *            rows of the FINISHED image). REFUSED: only one of 0x0200 and 0x0204 written, a rectangle past
 *            the target, a window clip type other than 0 (no emitter measured it, the port refuses an
 *            exclusive request), a window clip that starts after 0 or whose max is below the target size
 *            (the hardware semantics of a window clip are not measured, so a clipping one is not guessed at).
 *            INFERRED, behind GPU_PGRAPH_INFER_OUTPUT_SCISSOR, only when the rectangle is smaller than the
 *            target: that the hardware clips the pixels it writes to the surface clip rectangle (the library
 *            writes the user's scissor there, what the rasteriser does with it is docs/hardware-questions.md).
 *
 *   CULL     MEASURED: the emitter, from the library's own bytes (0x003D7060: 0x0308 = (mode != 0), and for a
 *            non-zero mode 0x039C = 0x404 + (mode != the stored front face), 0x003D70D0: 0x03A0 = its argument).
 *            NO PRODUCER in today's recorded stream (those handlers are ported without writing pairs), so this
 *            is exercised with pairs built from that arithmetic. The hardware enumerations are the two NV2A
 *            register headers' (FRONT 0x404, BACK 0x405, CW 0x900, CCW 0x901). enable 0 draws as before.
 *            REFUSED: an enable that is neither 0 nor 1, culling enabled with no face or no front face written
 *            (the stored front face's initial value is not measured), a face written with no enable, a face
 *            of 0x408 (FRONT_AND_BACK: the emitter never writes it) or anything else, a front face other than
 *            0x900 and 0x901. INFERRED, behind GPU_PGRAPH_INFER_OUTPUT_CULL_WINDING, whenever culling is on:
 *            that NV2A CW (0x900) is Vulkan COUNTER_CLOCKWISE on the finished image with row 0 at the top (flip_y swaps it; T1227, xemu-level), the
 *            orientation question of T100f (docs/hardware-questions.md).
 *
 *   BLEND    MEASURED: the method numbers and values the title's own state table writes (0x0304 enable 1,
 *            0x0344 source SRC_ALPHA 0x302, 0x0348 destination ZERO, 0x0350 equation FUNC_ADD 0x8006, 0x0358 colour
 *            mask 0x01010101) and that the pairs reach the recorded stream through the render-state primitive. The
 *            enumerations are the two register headers' (factors 0, 1, 0x300 to 0x308, 0x8001 to 0x8004, equations
 *            0x8006 ADD, 0x800A SUBTRACT, 0x800B REVERSE_SUBTRACT, 0x8007 MIN, 0x8008 MAX, colour mask B 1 << 0, G 1 << 8,
 *            R 1 << 16, A 1 << 24). A blended draw, or one that does not write every channel, is rendered ONCE onto the
 *            frame so far (gpu_vsh_output.destination) instead of twice onto two clears, because it depends on what is
 *            already there. REFUSED: blend factors or equation with no enable written, an enable that is not 0 or 1,
 *            an enabled blend without a source factor, destination factor and equation written, a factor or equation
 *            not in the lists (the signed equations 0xF005 and 0xF006 among them), a constant factor with no
 *            blend colour written, a colour mask with bits beyond the four channel enables. INFERRED, behind
 *            GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL: that the NV2A blend of the fragment against the frame equals
 *            Vulkan's blend on RGBA8 unorm (rounding, clamp, the alpha channel's factors), that the frame so far,
 *            starting as the replay's clear colour, is the destination (the title's Clear is not in the stream),
 *            that the blend colour 0x034C is ARGB (D3DCOLOR) and that the channel order of the colour mask
 *            is the replay's R, G, B, A.
 *
 *   ALPHA_TEST MEASURED: as BLEND (0x0300 enable 0, 0x033C function GREATER 0x204, 0x0340 reference 1 in the title's
 *            table), function 0x200 + n for NEVER .. ALWAYS from the headers. REFUSED: function or reference with
 *            no enable written, an enable that is not 0 or 1, an enabled test with no function or reference
 *            written, a function outside 0x200..0x207, a reference above 255, and any alpha test with
 *            `backend.combiner` (the combiner stage has its own alpha reference slot, not wired). ALWAYS depends on
 *            nothing and is not applied. INFERRED, behind GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL: that the test
 *            compares the fragment's alpha (oD0.w) clamped to [0, 1] and rounded to a byte with the reference byte,
 *            before blending.
 *
 *   DEPTH_STENCIL MEASURED: the method numbers and values of the title's own table for the depth function
 *            (0x0354 LEQUAL 0x203), depth mask (0x035C 1), stencil function (0x0364 ALWAYS 0x207), reference (0x0368 0),
 *            op on depth fail and on pass (0x0374, 0x0378, KEEP 0x1E00), pairs in the recorded stream. The enables
 *            0x030C and 0x032C (SetRenderTarget, ELIDED), the stencil write mask 0x0360, function mask 0x036C and op
 *            on fail 0x0370 have a measured method number and NO producer in the stream. Enumerations from the two
 *            headers: functions 0x200 + n, stencil ops KEEP 0x1E00, ZERO 0, REPLACE 0x1E01, INCRSAT 0x1E02, DECRSAT
 *            0x1E03, INVERT 0x150A, INCR 0x8507, DECR 0x8508. The buffers are the replay's: float32 depth and 8 bit
 *            stencil per pass (`replay_range`), held across the draws of a pass, a draw that tests either is rendered
 *            once onto the frame so far like a blended one. A state with no effect (depth ALWAYS with the mask off,
 *            stencil ALWAYS with every op KEEP) is applied as nothing and needs no inference. REFUSED: an enable that
 *            is not 0 or 1, a depth test on without a function or a mask written, a function outside 0x200..0x207, a
 *            depth mask above 1, a stencil reference or mask above 255, a stencil op outside the list. INFERRED:
 *            GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL, that the depth the vertex program's position gives (z / w as Vulkan
 *            interpolates it, in [0, 1], float32) compares and writes like the NV2A's depth buffer and starts at 1.0
 *            for each pass (the title's Clear is not in the stream); OUTPUT_STENCIL_MODEL, the same for an 8 bit
 *            stencil that starts at 0 and the Vulkan stencil operations; OUTPUT_DEPTH_STENCIL_ENABLE, that an enable
 *            word the stream never wrote (its emitter is elided) means enabled once the state words were written,
 *            as the title binds a depth surface; OUTPUT_STENCIL_DEFAULTS, that the stencil words never written are
 *            ALWAYS, reference 0, masks 0xFF and every op KEEP (the Direct3D 8 defaults, not measured here).
 *            NOT DECODED, so still unhandled and refused in strict mode: the depth clip range 0x0394 and 0x0398
 *            written with every viewport, and the depth bias and z clamp control words.
 *
 *   POLYGON_OFFSET (T502, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET) 0x0330 point enable, 0x0334 line enable, 0x0338 fill enable,
 *            0x0384 scale factor, 0x0388 bias (float32). MEASURED from the retail image (tests/test_gpu_state_census.py):
 *            the title's state table writes 0x0388, 0x0384, 0x0338, 0x0330 once at startup, all 0 (records 9, 10, 11, 12), the
 *            title's own helper at 0x00023570 rewrites them at run time (enables 1 with a float bias and scale, five call sites
 *            with (bias, scale) (-100, -100), (-2, -0.2), (-3, -0.4) and 0, 0 to turn it off), and the library's ZBIAS handler
 *            0x003D72A0 (state 0x95, value 0 in the table) writes all five through the out-of-line SetRenderState: 0x0384
 *            = -(float)v * 0.25 (the float at 0x475D4C), 0x0388 = -(float)v (a negative v adds 2^32 first), the three enables
 *            = (v != 0), so v = 0 writes -0.0 (0x80000000) to both floats. That handler is ported
 *            WITHOUT its pairs (docs/d3d8-usage.md 13.6), so 0x0334 and the handler's own writes are not in today's recorded
 *            stream. The steady loop never enables the offset (T441: five pairs, all zero). Replay: an enabled FILL offset with a
 *            scale or bias that is not zero becomes Vulkan's depth bias on triangle draws that test depth
 *            (gpu_vsh_output.depth_bias, constant factor = bias, slope factor = scale), applied before the depth test and the
 *            depth write. REFUSED: an enable that is not 0 or 1, a scale or bias that is NaN or infinite, an enabled fill offset
 *            whose scale or bias the stream never wrote, a stream that wrote the state with the group off in the backend.
 *            INFERRED: GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL (HQ55), that the NV2A offset is glPolygonOffset's
 *            (scale = factor on the slope, bias = units of the smallest depth step) and that Vulkan's unit on the replay's float32
 *            depth buffer matches the NV2A's 24 bit unit, which it does only for depths in [0.5, 1); a zero offset (the title's
 *            startup state, -0.0 included) applies as nothing and needs no inference, and an enabled non-zero offset when nothing
 *            tests depth is counted (gpu_pgraph_report.offset_unobserved, gpu_pgraph_output.offset_unobserved), not applied. T552: the
 *            bias is on the pipeline of TRIANGLE lists only (gpu_vsh_draw.c: RADV biases a line or point list and llvmpipe does
 *            not, HQ55 MEASURED), so gpu_pgraph_report.offset_applied counts triangle draws only and a line or point draw with the
 *            offset on is counted in offset_unobserved (the offset cannot show on it), never in both.
 *            GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY, whenever the point or line enable is 1: those two enables
 *            apply to polygons drawn in point or line FILL MODE, which the replay never rasterizes (every triangle is filled
 *            and the fill mode words are not decoded), so they change nothing. Lines and points primitives are never biased.
 *
 *   IGNORED  (T502, GPU_PGRAPH_OUTPUT_IGNORED) SKIPPED ON PURPOSE and named: 0x0310 dither enable on the 3D subchannel
 *            (state 0x41, in NO title record and written by no recovered emitter, so no producer today: the subchannel-5 0x310 of
 *            the fence is a different command, T391) and 0x09F8 (SET_SPECULAR_PARAMS + 6, the title's record 28, value 3 at
 *            startup, rewritten to 0xF and back by the title function at 0x000FE990). Both are decoded and counted
 *            (gpu_pgraph_stats.pairs_ignored) and need the group on in the backend like any other, and change no pixel: dither
 *            only acts when converting to a 16 bit target (the replay's target is A8R8G8B8, the HQ54 inference) and the
 *            specular parameters only feed fixed-function lighting, which the program execution mode the replay requires
 *            (0x1E94) does not run. INFERRED, HQ55, and not asserted by a hardware run.
 *
 *   CLEAR    MEASURED: the method numbers and the layout of the words, from the bytes of the library's Clear
 *            (see gpu_pgraph.h, NO CURRENT PRODUCER in the recorded stream) and the flag bits from the headers. A
 *            clear is an EVENT in the draw order (gpu_pgraph_clear): the replay applies it to the frame, and to the
 *            depth and stencil buffers, in the rectangle, before the draw it precedes, and the clears after the last
 *            draw at the end. gpu_pgraph_replay_range, which has draw counts and nothing else, gives a clear sitting
 *            exactly at the boundary of two passes to the pass that FOLLOWS. A caller that knows better names each pass's
 *            clear events itself (gpu_pgraph_replay_pass): d3d8_swap_replay notes the clear count at every render target
 *            switch, so there a clear lands on the target that was bound when it was written, and one in a pass with no
 *            draw (no image) is counted and logged, never given to another target. REFUSED: flags beyond Z, stencil and the four colour bits, a clear with no
 *            rectangle (both words) written, a rectangle that is empty or reaches past the target, a colour clear
 *            with no colour written, a depth or stencil clear with no value written or with the depth and stencil
 *            group not enabled in the backend, and a stream that cleared with the group off in the backend. No flag
 *            clears nothing. INFERRED, behind GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL: that the target is A8R8G8B8
 *            (Clear's SURFACE_FORMAT write 0x0208 is not decoded), so the colour value is 0xAARRGGBB, that the depth
 *            value is the D24S8 layout (24 bit depth << 8 | stencil, the depth being value / 16777215 as float32), that
 *            only the flagged channels are written whatever the colour and stencil write masks hold, and that the
 *            rectangle's rows are the rows of the FINISHED image (mirrored under flip_y).
 *
 *   SURFACE  (T462, T541, GPU_PGRAPH_OUTPUT_SURFACE) the SetRenderTarget packet, MEASURED against the original at 0x003D3800
 *            (docs/t541-bind-packet-proof.md: 40 pairs, retail back and front buffer, and tests/test_d3d8_set_render_target_oracle.py).
 *            0x0100 NO_OPERATION and 0x0110 WAIT_FOR_IDLE (value 0, counted in gpu_pgraph_stats.sync_pairs): the replay runs the
 *            stream in order and at once, so they change no pixel. The words 0x0208 surface format, 0x020C pitch, 0x0210 colour
 *            offset, 0x0214 zeta offset, 0x0290 CONTROL0, 0x0394 / 0x0398 clip min and max and 0x1D7C anti-aliasing control are
 *            shadowed. The render target the replay draws into is the one d3d8_swap_replay's SetRenderTarget hook measured (the
 *            header's data and size), NOT these words: the pitch and the two offsets are named ignores, they describe the
 *            same surfaces by address. REFUSED: a surface format that is not colour 8 (A8R8G8B8), zeta 2 (Z24S8), type 1 (pitch),
 *            antialiasing 0, log2 size 0 (the replay renders A8R8G8B8 targets, the HQ54 inference the CLEAR group states, now
 *            checked against the stream), a CONTROL0 other than the measured 0x00100001 (stencil write on, fixed point z,
 *            z perspective off, texture perspective on), a depth clip range other than the measured (0.0, 16777215.0) (0x4B7FFFFF),
 *            an anti-aliasing control other than the measured 0xFFFF0000 (off, every sample) and 0x00000000 (off, no sample: the library's own
 *            un-initialised shadow in the disc-less steady loop, T572, whether a zero sample mask hides pixels is NOT measured). INFERRED, behind
 *            GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE: the register NAMES (the nxdk and xemu headers for 0x0208 to 0x0214, 0x0290,
 *            0x0394, 0x0398, the local xboxrecomp copy of the xemu header for 0x1D7C, which nxdk lacks) and that the measured
 *            values change nothing the model does not already do (a linear z and perspective texturing are what Vulkan
 *            interpolates, the full range is the default 24 bit clip).
 *
 *   FIXED    (T462, GPU_PGRAPH_OUTPUT_FIXED) the fixed-function state the composition and the title's dirty cascade write and
 *            the program execution mode does not run, each REFUSED at any value but the measured one: 0x0314 lighting enable,
 *            0x03B8 specular enable, 0x03BC light enable mask, 0x02A4 fog enable (0, a fogged draw is a pixel effect the combiner
 *            already refuses), 0x0318 point parameters enable and 0x031C point smooth enable (0), 0x038C and 0x0390 polygon mode
 *            (T860: POINT 0x1B00, LINE 0x1B01 and FILL 0x1B02 are decoded into gpu_vsh_output.polygon_mode (xemu reference, HQ61),
 *            a front and back that differ are refused, and a written 0 is refused (T885 measured CreateDevice initializes these shadows), and the
 *            polygon offset enable of the mode decides), 0x0294 light control (bits 0, 16 and 17 only, measured 0x00020001: it feeds fixed-function lighting) and
 *            0x1D84 (the z-cull and occlusion enable the library writes with the stencil state, measured 0 and 1, values 0 to 3
 *            accepted: a hardware culling optimisation, its NAME is INFERRED from the xemu source and absent from the local
 *            headers, the NUMBER and the emitter shape are measured: tests d3d8_state library_set_90). 0x043C point size is
 *            shadowed (a float, measured 0): the POINT_SIZE inference above still decides where a point's size comes from.
 *            INFERRED behind GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE like SURFACE.
 *
 *   TEXTURE  (T462, GPU_PGRAPH_OUTPUT_TEXTURE) the texture stage words the title's own stage emitter writes (the T480 oracle
 *            measurement): 0x1B08 + 0x40 * stage address (0x00000303 U and V CLAMP_TO_EDGE and 0x00000101 U and V WRAP, both bound, 0 unbound), 0x1B0C control 0
 *            (0x4003FFC0 enabled, 0x0003FFC0 disabled: enable and xemu anisotropy mask0x30 may differ (anisotropy also requires the texture sampling inference); no alpha kill or colour key, the
 *            full LOD range), 0x1B14 filter (0x02062000 only: minification 6 and magnification 2, which the nxdk names read as
 *            linear, and kernel field 1, NOT the nearest the stand-in texture infers: a single colour shows no difference, a real
 *            texture bridge (T510) must decide) and the bump environment of stages 1 to 3 (0x1B28 + 0x40 * stage, six words,
 *            0 only: no stage program the combiner accepts reads it). The texture OFFSET, FORMAT and size words 0x1B00, 0x1B04
 *            and 0x1B1C are not in the recorded stream (the port of SetTexture elides them), so no texel is decoded here.
 *            INFERRED behind GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE.
 *
 *   IMMEDIATE (T462, GPU_PGRAPH_OUTPUT_IMMEDIATE) SET_VERTEX_DATA2F_M (0x1880 + 8 * slot, x then y) inside a BEGIN_END bracket:
 *            the Swap copy composition's triangle (three vertices, texture coordinate slot 9 then position slot 0). MEASURED:
 *            the writing order and values, from the original (tests/test_d3d8_immediate_oracle.py, the composition oracles).
 *            The model emits a vertex when attribute 0's y word is written and holds every other slot's current value, each
 *            slot a disabled array unless written, and the replay assembles the vertices through the one path every draw uses
 *            (z = 0 and w = 1 from the component defaults). REFUSED: vertex data outside a bracket, a y without its x, a slot
 *            first written after the first vertex, array vertices and immediate vertices in one bracket, a bracket past
 *            GPU_PGRAPH_MAX_INLINE_VERTICES, and a draw of immediate vertices when the group is off in the backend. INFERRED
 *            behind GPU_PGRAPH_INFER_OUTPUT_IMMEDIATE_VERTEX: that the position write emits the vertex (the NV2A convention, the
 *            headers state no rule) and that 2f sets (x, y, 0, 1).
 *
 *   UNWRITTEN VARYINGS (T462) A combiner that reads a varying the vertex program never writes (the copy composition's two
 *            instruction program writes oPos and oT0 and its alpha input reads oD0) was refused. With
 *            GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING the fragment module is rewritten (gpu_spirv_default_inputs) so that
 *            input starts at the NV2A's initial output value, (0, 0, 0, 1) (INFERRED: xboxdevwiki NV2A/Vertex_Shader,
 *            "initialized to XYZ=0 and W=1.0", no hardware run). tools/nv2a/translate.py initialises its own o[] to
 *            vec4(0), which differs in w for a program that reads an output back, a separate finding.
 *
 * THE COMBINER FRAGMENT STAGE (T75, OPT-IN). With `backend.combiner` set the fixed pass-through
 * fragment stage (oD0 written unchanged) is replaced by the translated register combiner the
 * stream programmed. The model must have been told to decode the combiner methods
 * (gpu_pgraph_set_combiner), the module is found in `fragment_table` by gpu_combiner.h's name for
 * the configuration, and the extra inferences are the GPU_PGRAPH_INFER_COMBINER_* bits below.
 * What is refused outright (fog register, texture modes other than a test 2D texture, a texture
 * register with no test texture) is listed in gpu_combiner.h. Without the flag nothing changes: the
 * fragment fields are ignored and the combiner methods stay unhandled.
 *
 * Types still refused, with what is missing (T84d): CMP, S1, UB_OGL and S32K of any other size.
 * The builder emits CMP (size 1, v5 from 0x19380 when the normal count is below 3, v6 and v7) but
 * the packing is not in the image (needs a real CMP vertex buffer), S1 and UB_OGL no emitter writes.
 *
 * The table must hold modules generated with `vsh_modules.py --undo-viewport` (T96) for the
 * viewport programs to land in clip space: the replay cannot tell which kind a table holds, so
 * `viewport_inverse_modules` states it and is reported. The Y orientation (T100f) is the backend's
 * `flip_y`: the title's own viewport scale has a NEGATIVE y (MEASURED in the port of 0x003D7860,
 * half height * -0.5), which with the inferred c58 reading means a y-up clip space, and Vulkan's is
 * y-down. It is an option, default off, applied as an exact row reversal of the finished frame.
 *
 * T560. A program that hands the rasterizer WINDOW coordinates in oPos (the copy composition's two instruction
 * program, `(0, 0) (4W, 0) (0, 4H)`; the x, y of the z-only class) covers only the lower right quarter of the
 * target when oPos is taken as clip space (76800 of 307200 pixels at 640 x 480). `window_clip_modules` STATES that
 * the table was generated with `vsh_modules.py --window-to-clip`, whose x and y conversion uses constants 58 and
 * 59 like the viewport inverse (so it needs the registers 0x0A20 and 0x0AF0, a stream write of the rows, or
 * `viewport_from_target`: a draw with no scale in c58 is REFUSED while the flag is set, never drawn raw). The
 * rule is INFERRED and never run on an NV2A (docs/hardware-questions.md HQ57). Default off, nothing changes.
 */

#ifndef TSFP_GPU_PGRAPH_REPLAY_H
#define TSFP_GPU_PGRAPH_REPLAY_H

#include "gpu_combiner.h"
#include "gpu_device.h"
#include "gpu_pgraph.h"
#include "gpu_standin_units.h"
#include "gpu_vsh_draw.h"
#include "gpu_vsh_select.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPU_PGRAPH_INFER_PROGRAM_HEADER 0x1u
#define GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS 0x2u
#define GPU_PGRAPH_INFER_COMPONENT_DEFAULTS 0x4u
#define GPU_PGRAPH_INFER_D3DCOLOR_ORDER 0x8u
#define GPU_PGRAPH_INFER_POINT_SIZE 0x10u
#define GPU_PGRAPH_INFER_LINE_WIDTH 0x20u
#define GPU_PGRAPH_INFER_S32K_UNNORMALISED 0x40u
/* T1204. CMP (type 6, size 1) is one 32-bit word, three signed normalised fields (11, 11, 10 bits): x the low 11 bits
 * over 1023, y the next 11 over 1023, z the top 10 over 511, with no floor (xemu array path, T1206) and w read as 1. Taken from xemu's
 * converter (HQ28), not measured in the image, so INFERRED until the HQ28 xemu experiment confirms it. NOT part of
 * INFER_ALL (like the execution-mode bit): the host composition d3d8_swap_replay_host_inferences adds it. */
#define GPU_PGRAPH_INFER_CMP_PACKED 0x400u
#define GPU_PGRAPH_INFER_ALL 0x7Fu
/* T441. The title's recorded stream never writes the transform execution mode 0x1E94 (the library writes it only
 * when the vertex shader class changes, and the CreateDevice init that would have set it is elided), yet it loads
 * and starts vertex programs. With this bit allowed, a stream that wrote NO mode but loaded a program (0x1EA0
 * written) is replayed as the program mode. INFERRED, a mode that WAS written still decides (a fixed-function mode
 * stays refused). NOT part of INFER_ALL, so every existing caller keeps the refusal. */
#define GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN 0x80u
/* T477. Seed c58/c59 from a whole-target D3D viewport when the stream carries no
 * 0x0A20/0x0AF0 words. The target size is measured; the D3D half-size and negative-y
 * mapping are derived from the measured viewport emitter and remain an inference. */
#define GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET 0x100u
/* 0x200 was GPU_PGRAPH_INFER_EXECUTION_MODE_FIXED_WITH_PROGRAM (T846), deleted by T870: the retail intro's mode 4 draws came from a
 * CreateDevice state divergence (the title declaration header flags), not from a hardware rule. Do not reuse the value. */
/* The output-state inferences (T267, see the OUTPUT STATE block above). NOT part of INFER_ALL, like the
 * execution-mode, viewport and combiner bits, so a caller that allows "all" and also opts into an output
 * group must say so. They start at 0x2000. */
#define GPU_PGRAPH_INFER_OUTPUT_SCISSOR 0x2000u
#define GPU_PGRAPH_INFER_OUTPUT_CULL_WINDING 0x4000u
#define GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL 0x8000u
#define GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL 0x10000u
#define GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL 0x20000u
#define GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL 0x40000u
#define GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE 0x80000u
#define GPU_PGRAPH_INFER_OUTPUT_STENCIL_DEFAULTS 0x100000u
#define GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL 0x200000u
/* T502 */
#define GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL 0x400000u
#define GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY 0x800000u
/* T462 */
#define GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE 0x1000000u
#define GPU_PGRAPH_INFER_OUTPUT_IMMEDIATE_VERTEX 0x2000000u
#define GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING 0x4000000u
/* T578 */
#define GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL 0x8000000u
/* T510: a render target image of an earlier pass stands for a bound texture: the replayed image is what the hardware surface holds,
 * the A8R8G8B8 linear texture takes its coordinate in texels, is sampled bilinear and clamped as the stream's words say. NOT in
 * INFER_OUTPUT_ALL: only `--gpu-replay-rt-texture` allows it. */
#define GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE 0x10000000u
#define GPU_PGRAPH_INFER_OUTPUT_ALL 0xFFFE000u
/* T832 (HQ58, xemu-level): `gpu_pgraph_replay_copy` adopts the rules of the live T793 planner (live_target_plan_blit): a row wider than
 * the narrower pitch is clamped to it (offsets 0), rectangles of one image may overlap (rows ascending, one buffered row), colour formats
 * 7 and 6 force the alpha byte to 0xFF and 0. NOT in INFER_OUTPUT_ALL (the default refusals stay as measured), only `--gpu-replay-output-state`
 * adds it (d3d8_swap_replay_host_inferences). */
#define GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES 0x20000000u
/* T633 (1) and T596: a surface no pass of the frame drew has the contents of its kept image from an earlier frame or, with none, the bytes of guest
 * memory (the host never writes a GPU result there, so MEASURED all zero on the retail boots) read at the present, taken as what the hardware surface held at
 * the draw or blit (the rule is MEASURED in xemu, T736, that the bytes at the present are those at the draw is INFERRED). NOT in INFER_OUTPUT_ALL: only `--gpu-replay-surface-source` allows it. */
#define GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE 0x40000000u
/* T633 (2): a render target pass starts from the image its Data word held after an earlier frame's pass (and the blits since) instead of a fresh
 * image, as a title that does not clear every frame would see its previous frame (MEASURED in xemu, T736, xemu-level). NOT in INFER_OUTPUT_ALL: only `--gpu-replay-target-persist`. */
#define GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST 0x80000000u
/* The combiner inferences (gpu_combiner.h says what each one is). NOT part of INFER_ALL, so a caller
 * that allows "all" and also opts into the combiner must say so. */
#define GPU_PGRAPH_INFER_COMBINER_UNWRITTEN GPU_COMBINER_INFER_UNWRITTEN
#define GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM GPU_COMBINER_INFER_STAGE_PROGRAM
#define GPU_PGRAPH_INFER_COMBINER_INITIAL_STATE GPU_COMBINER_INFER_INITIAL_STATE
#define GPU_PGRAPH_INFER_COMBINER_CONSTANT_BYTES GPU_COMBINER_INFER_CONSTANT_BYTES
#define GPU_PGRAPH_INFER_COMBINER_COLOUR_RANGE GPU_COMBINER_INFER_COLOUR_RANGE
#define GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING GPU_COMBINER_INFER_TEXTURE_SAMPLING
#define GPU_PGRAPH_INFER_COMBINER_ALL GPU_COMBINER_INFER_ALL
/* T478. The combiner inferences the swap replay's combiner option (`--gpu-replay-combiner`) allows, the two the
 * title's texture-free configurations need (docs/combiner-translator.md section 15: 16 need CONSTANT_BYTES only, 14
 * both, 3 COLOUR_RANGE only). The others stay refused: UNWRITTEN, STAGE_PROGRAM and INITIAL_STATE would invent state
 * the stream never wrote, TEXTURE_SAMPLING is about a test texture the swap replay never provides. NOT in INFER_ALL. */
#define GPU_PGRAPH_INFER_COMBINER_REPLAY (GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE)

/* The program header's low u16 the digest is built with (see PROGRAM_HEADER above). */
#define GPU_PGRAPH_PROGRAM_HEADER_VERSION 0x2078u

/* gpu_pgraph_read_fn (the guest memory reader) is in gpu_pgraph.h. */
typedef bool (*gpu_pgraph_module_fn)(void *context, uint32_t module, const uint32_t **words,
                                     size_t *word_count);

/* T510. A per-draw, per-stage texture provider for the combiner: called for every stage of every draw BEFORE the combiner is
 * planned, with the draw's index in the model's list and its state snapshot. It fills `out` (zeroed first) with the stage's
 * image (`rgba`, `width`, `height`, `linear`, `unnormalised`) or, when the stage has no texture, a NAMED `refusal` that the plan
 * prints only if the combiner actually reads the stage. `refusal` and a non-NULL `rgba` are never both set. The pixels must stay
 * valid until the pass returns. `texture_sampled` is called after a draw is drawn, once per stage its combiner sampled. */
typedef void (*gpu_pgraph_texture_fn)(void *context, size_t draw, const gpu_pgraph_state *state, uint32_t stage,
                                      gpu_combiner_texture *out);
typedef void (*gpu_pgraph_texture_sampled_fn)(void *context, size_t draw, uint32_t stage);

/* T847: make a module that is in no table. `fragment` false: `name` is `generated_<sha256>` and `bytes` the program as the replay digests it
 * (header 0x2078, instruction count, instructions); true: `name` is the planned `combiner_<sha256>` and `bytes` the 57 combiner state words
 * padded to a 240 byte definition block. True when a module of that name now exists where the table's `load_module` / `load_fragment_module`
 * find it (the caller registers it in its table before returning); false with `error` filled. Called at most once per distinct miss that
 * the maker does not itself remember. */
typedef bool (*gpu_pgraph_module_make_fn)(void *context, bool fragment, const char *name, const uint8_t *bytes, size_t byte_count,
                                          char *error, size_t error_bytes);

typedef struct {
    const struct gpu_vsh_table *table;  /* T95 selector table (vsh_table.inc) */
    gpu_pgraph_read_fn read_guest;      /* guest memory: vertex bytes. Address as recorded. */
    gpu_pgraph_module_fn load_module;   /* SPIR-V words of a table module index */
    void *context;
    uint32_t allowed_inferences;        /* GPU_PGRAPH_INFER_* bits a draw may depend on */
    bool viewport_inverse_modules;      /* the table was generated with --undo-viewport */
    bool live_raster_modules; /* T918: xemu-derived window raster map, appended
                                 extent/depth uniform. */
    bool live_texture_modes;    /* T1490 opt-in (INFERRED): stage programs 3 (cube), 9 (DOT_ST) and 17 (DOTPRODUCT), default false, the live host sets it with --live-inferred. */
    bool live_swizzled_targets; /* T1489 opt-in (INFERRED): a swizzled surface format (type 2, log2 size in bits 16..31) is a target drawn in picture order, default false. */
    bool live_fog_modules; /* T1059 opt-in v5 generated programmable fog profile, default false. */
    bool window_clip_modules;           /* T714: table generated with --window-to-clip (xemu-level none class;
                                        * z-only xy remains open, HQ21) */
    bool flip_y;                        /* reverse the rows of the finished frame (T100f) */
    bool viewport_from_target;          /* T477: infer whole-target c58/c59 from replay dimensions */
    uint32_t viewport_width;            /* T477: measured bound render-target dimensions */
    uint32_t viewport_height;
    float line_width;                   /* pixels for line draws, 0 = unstated (INFER_LINE_WIDTH) */
    /* The combiner fragment stage (T75), all ignored unless `combiner` is true. */
    uint32_t output_groups;             /* T267: GPU_PGRAPH_OUTPUT_* groups applied, see above. 0 = none */
    bool combiner;                      /* replace the fixed fragment stage, see above */
    const struct gpu_vsh_table *fragment_table; /* modules named combiner_<sha256>, by name only */
    /* SPIR-V words of a fragment_table module. The vertex module's words (load_module) stay valid
     * while this one is loaded: a loader that keeps one buffer for both would corrupt the draw. */
    gpu_pgraph_module_fn load_fragment_module;
    gpu_combiner_texture test_textures[GPU_COMBINER_TEXTURE_STAGES]; /* caller-provided, not the title's */
    /* T719: when `standin_unit_rule_count` is not 0, stage 0 of test_textures takes its `unnormalised` per draw from the draw's
     * vertex program digest (gpu_standin_unit_lookup), overriding the field above. A draw whose combiner samples stage 0 with a
     * program in no rule refuses. INFERRED, caller measured. */
    gpu_standin_unit_rule standin_unit_rules[GPU_STANDIN_UNIT_RULES];
    uint32_t standin_unit_rule_count;
    const char *draw_dump_path; /* T724: when set, one JSON line per replayed draw is appended (digest, t0 sampled, attributes, constants) */
    /* T510: when `resolve_texture` is set it REPLACES test_textures for every draw (a stage it gives no image has none). */
    gpu_pgraph_texture_fn resolve_texture;
    gpu_pgraph_texture_sampled_fn texture_sampled;
    void *texture_context;
    /* T847: optional, NULL (the default) keeps every table miss a refusal. Set, a program or a combiner configuration that is in no table
     * is handed to the maker (translated on demand, INFERRED like every translator output) and the lookup is retried once. */
    gpu_pgraph_module_make_fn make_module;
    void *make_module_context;
    /* T633 (2), MEASURED in xemu (T736): when not NULL, the `width` x `height` x 4 bytes of RGBA8 (row 0 first, tightly packed) a replayed pass STARTS
     * from instead of the clear colour, the kept contents of a render target an earlier frame drew. The pass's own clear events still apply
     * on top. Refused (GPU_PGRAPH_ERR_ARGUMENT) with `flip_y`: the kept image would be mirrored a second time. NULL is the pass as it always
     * was, a fresh image. */
    const uint8_t *initial_pixels;
} gpu_pgraph_backend;

typedef struct {
    uint32_t draws;            /* draws in the list */
    uint32_t drawn;            /* draws that reached the device */
    uint32_t degenerate;       /* draws that expanded to no triangle */
    uint32_t vertices;         /* expanded vertices sent to the device */
    uint32_t used_inferences;  /* GPU_PGRAPH_INFER_* bits some drawn draw depended on */
    uint32_t clears_applied;   /* T267: clear events applied to the frame (gpu_pgraph_clear) */
    uint32_t offset_applied;   /* T502/T552: drawn TRIANGLE draws that ran with a Vulkan depth bias (polygon offset) */
    uint32_t offset_unobserved; /* T502/T552: drawn draws with an enabled non-zero polygon offset that cannot show: no depth test, or a line or point list (never biased): counted, no effect */
    uint32_t textured_draws;   /* T497: drawn draws whose combiner plan sampled a test texture */
    size_t failed_draw;        /* index of the draw that was refused, valid on failure */
    char error[320];           /* why, "" when nothing failed */
} gpu_pgraph_report;

typedef struct {
    uint32_t module;           /* index into table->module_names */
    bool is_static;
    uint32_t instructions;
    bool mode_inferred; /* T441: the execution mode was never written and INFER_EXECUTION_MODE_UNWRITTEN stood in */
    char digest[65];
} gpu_pgraph_program;

/* Primitive operation to a triangle list of vertex positions in `indices` order. Writes
 * `capacity` indices at most and returns how many, or UINT32_MAX when the operation is not
 * a triangle one or does not fit. TRIANGLES drops a remainder of fewer than three, QUADS split
 * (0,1,2) (0,2,3), STRIP alternates, FAN pivots on the first. */
uint32_t gpu_pgraph_triangulate(uint32_t primitive, const uint32_t *indices, uint32_t count,
                                uint32_t *out, uint32_t capacity);

/* LINES and LINE_STRIP to a line list of vertex positions in `indices` order (two per segment).
 * LINES drops a trailing odd vertex, LINE_STRIP joins consecutive vertices. Returns the number of
 * indices written, or UINT32_MAX when the operation is not one of those two or does not fit.
 * POINTS needs no expansion, a point list is the indices themselves. */
uint32_t gpu_pgraph_lineate(uint32_t primitive, const uint32_t *indices, uint32_t count,
                            uint32_t *out, uint32_t capacity);

typedef struct {
    gpu_combiner_plan plan;
    uint32_t module;           /* index into backend->fragment_table->module_names */
    bool alpha_in_module;      /* T860: the module is the `_alpha` variant, the alpha test runs in it (plan.constants vec4 19) */
} gpu_pgraph_fragment;

/* T267. The output state of one draw, resolved from its snapshot. `active` false means gpu_vsh_draw.output
 * stays NULL (nothing the draw depends on was set). `used_inferences` are the GPU_PGRAPH_INFER_OUTPUT_*
 * bits the draw depends on. */
typedef struct {
    bool active;
    bool needs_destination; /* the draw depends on the frame so far (blend, partial colour write, depth) */
    bool needs_depth;       /* the draw tests depth or stencil: the replay's buffers are passed to it */
    bool offset_unobserved; /* T502: polygon offset enabled and non-zero but nothing tests depth, so it cannot show */
    gpu_vsh_output output;
    uint32_t used_inferences;
} gpu_pgraph_output;

/* T267, the CLEAR group: one clear event turned into what to do to a `width` x `height` frame. The rectangle is
 * INCLUSIVE and already mirrored under flip_y. `channels` is a mask of GPU_VSH_CHANNEL_* of the colour channels to
 * write with `rgba`. */
typedef struct {
    uint32_t x_min, y_min, x_max, y_max;
    bool colour;
    uint32_t channels;
    uint8_t rgba[4];
    bool depth;
    float z;
    bool stencil;
    uint8_t stencil_value;
    uint32_t used_inferences;
} gpu_pgraph_clear_resolved;

/* Resolve one clear event. Refusals fill report->error and return GPU_PGRAPH_ERR_UNMEASURED. Device free. */
gpu_pgraph_result gpu_pgraph_resolve_clear(const gpu_pgraph_clear *clear,
                                           const gpu_pgraph_backend *backend, uint32_t width,
                                           uint32_t height, gpu_pgraph_clear_resolved *out,
                                           gpu_pgraph_report *report);

/* Resolve the output state of a snapshot for a `width` x `height` target. Refusals (see the OUTPUT STATE block)
 * fill report->error and return GPU_PGRAPH_ERR_UNMEASURED. Device free. */
gpu_pgraph_result gpu_pgraph_resolve_output(const gpu_pgraph_state *state,
                                            const gpu_pgraph_backend *backend, uint32_t width,
                                            uint32_t height, gpu_pgraph_output *out,
                                            gpu_pgraph_report *report);

/* The combiner stage of a state: the plan (gpu_combiner_plan_build) and its module in the fragment
 * table. Refusals fill report->error, a name missing from the table is a refusal that prints it. */
gpu_pgraph_result gpu_pgraph_resolve_fragment(const gpu_pgraph_state *state,
                                              const gpu_pgraph_backend *backend,
                                              gpu_pgraph_fragment *out, gpu_pgraph_report *report);

/* Find the module of the program a state would run. Refusals fill report->error. */
gpu_pgraph_result gpu_pgraph_resolve_program(const gpu_pgraph_state *state,
                                             const gpu_pgraph_backend *backend,
                                             gpu_pgraph_program *out, gpu_pgraph_report *report);

/* T791. The GPU_VSH_TOPOLOGY_* a BEGIN_END operation draws as (POINTS, the two line operations, else triangles), the one
 * mapping gpu_pgraph_assemble_draw uses, exported so the live pipeline cache keys on the same decision. */
uint32_t gpu_pgraph_topology_of(uint32_t primitive);

/* T791. What a point or line draw rasterizes with: the INFER_POINT_SIZE / INFER_LINE_WIDTH gate and the 1.0 line width
 * refusal, device free, the same check the replay runs per draw (it was a static helper). `*used` ORs the inference bit. */
gpu_pgraph_result gpu_pgraph_resolve_primitive(const gpu_pgraph_backend *backend, uint32_t topology,
                                               uint32_t *used, gpu_pgraph_report *report);

typedef struct {
    float *attributes;         /* vertex_count * GPU_PGRAPH_ATTRIBUTES * 4 floats, malloc'd */
    uint32_t vertex_count;     /* expanded vertices of the list named by `topology` */
    uint32_t topology;         /* GPU_VSH_TOPOLOGY_* */
    float constants[GPU_PGRAPH_CONSTANT_ROWS * 4u];
    uint32_t used_inferences;
} gpu_pgraph_assembled;

/* Fetch and convert one draw's vertices into gpu_vsh_draw's layout and its constants. */
gpu_pgraph_result gpu_pgraph_assemble_draw(const gpu_pgraph *pgraph, size_t draw_index,
                                           const gpu_pgraph_backend *backend,
                                           gpu_pgraph_assembled *out, gpu_pgraph_report *report);
void gpu_pgraph_assembled_free(gpu_pgraph_assembled *assembled);

/* Draw every draw of the list, in order, into a width x height RGBA8 frame cleared to `clear`.
 * Stops at the first refused draw (report->failed_draw, report->error), leaves `out` empty and
 * returns that draw's result. Draws are composited exactly: each is rendered twice onto two
 * different clears and a pixel is covered when it differs from its clear in either render. */
gpu_pgraph_result gpu_pgraph_replay(const gpu_pgraph *pgraph, gpu_device *device,
                                    const gpu_pgraph_backend *backend, uint32_t width,
                                    uint32_t height, const float clear[4], gpu_image *out,
                                    gpu_pgraph_report *report);

/* The same for draws [first_draw, first_draw + draw_count) of the list only (T262: the swap replay
 * decodes a frame once, at the kicks, and replays each render target's run of draws into its own
 * image). report->draws is draw_count, report->failed_draw is the index in the whole list. ERR_ARGUMENT
 * for a range outside the list. gpu_pgraph_replay is this over the whole list. */
gpu_pgraph_result gpu_pgraph_replay_range(const gpu_pgraph *pgraph, gpu_device *device,
                                          const gpu_pgraph_backend *backend, uint32_t width,
                                          uint32_t height, const float clear[4], size_t first_draw,
                                          size_t draw_count, gpu_image *out,
                                          gpu_pgraph_report *report);

/* T267. gpu_pgraph_replay_range with the clear events of the pass named by the caller instead of inferred from draw counts:
 * the events [first_clear, first_clear + clear_count) of the model's clear list (gpu_pgraph_clear_at) belong to this pass,
 * each applied before the first draw of the range at or after its `before_draw` and the rest after the last draw. A caller
 * that knows where each clear sat among its render target switches (d3d8_swap_replay notes the clear count at every switch)
 * gets the exact attribution that draw counts alone cannot give. report->clears_applied says how many events were applied. */
gpu_pgraph_result gpu_pgraph_replay_pass(const gpu_pgraph *pgraph, gpu_device *device,
                                         const gpu_pgraph_backend *backend, uint32_t width,
                                         uint32_t height, const float clear[4], size_t first_draw,
                                         size_t draw_count, size_t first_clear, size_t clear_count,
                                         gpu_image *out, gpu_pgraph_report *report);

/* T578, the BLIT group: one image blit (gpu_pgraph_copy) applied to two of the replay's surface images, device free. SRCCOPY
 * of the rectangle `in` of `source` to the rectangle `out` of `destination`, row by row, the pixels of an A8R8G8B8 image
 * copied byte for byte (both images hold the same layout, so no channel moves). `source` and `destination` may be the SAME
 * image when the two rectangles do not overlap. Under backend->flip_y both images hold their rows reversed (gpu_pgraph_backend),
 * so the rectangle rows are mirrored in each. REFUSED by name (GPU_PGRAPH_ERR_UNMEASURED, report->error): the backend's
 * output_groups lacking the BLIT group, a colour format other than A8R8G8B8 (the replay's surfaces are A8R8G8B8 images, the
 * Y8 and R5G6B5 blits of CopyRects' byte path have no pixel meaning here), a pitch that is not the image's tightly packed
 * width * 4 (the stride the blit used would not address the image's rows), a rectangle that is not inside its image (no
 * clamping is measured), overlapping rectangles of one image (the order the hardware copies is not measured), and the
 * inference GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL not being allowed: that the replayed image of a surface IS what the hardware
 * holds at that moment and that a same format blit is the byte copy xemu performs (INFERRED, xemu's image blit, no hardware
 * run here). With GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES (T832) allowed the clamp, the overlap and formats 7 and 6 are NOT refused but
 * follow live_target_plan_blit (formats 4 and 1 stay refused). A zero width or height moves nothing and is not refused (xemu runs no blit for it, INFERRED the same way) and
 * uses no inference. `*used_inferences` gets the bit when pixels moved. */
gpu_pgraph_result gpu_pgraph_replay_copy(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                         const gpu_image *source, gpu_image *destination,
                                         uint32_t *used_inferences, gpu_pgraph_report *report);

/* T832: gpu_pgraph_replay_copy with GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES allowed delegates here (gpu_pgraph_replay_copy_rules.c). */
gpu_pgraph_result gpu_pgraph_replay_copy_planner_rules(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                                       const gpu_image *source, gpu_image *destination,
                                                       uint32_t *used_inferences, gpu_pgraph_report *report);

/* T596 and T769, the BLIT group's BYTE path, device free: the same SRCCOPY as gpu_pgraph_replay_copy over two surfaces held as the BYTES the
 * guest memory has (rows `pitch` apart, no pixel meaning), for every measured colour format: Y8 (1 byte a pixel), R5G6B5 (2), A8R8G8B8 and
 * the X8R8G8B8 variants 7 and 6 (4, only the alpha byte of each pixel is forced to 0xFF or 0). MEASURED in xemu (T769): the row clamp
 * min(width, source_pitch / bpp, destination_pitch / bpp) pixels for every format, no overlap refusal and no pitch equality needed:
 * `source` may equal `destination` (one surface, two pitches allowed), rows ascend and each row is read whole before it is written. Row r moves
 * the clamped row bytes from source offset (in_y + r) * source_pitch + in_x * bpp to destination offset (out_y + r) * destination_pitch + out_x * bpp
 * (flat addresses: a row that passes its pitch runs on into the next row's bytes). `source_length` and `destination_length` are the bytes held:
 * a rectangle whose last row ends past them is REFUSED by name (GPU_PGRAPH_ERR_UNMEASURED), nothing is clamped to them. REFUSED by name too:
 * the BLIT group missing, an operation other than SRCCOPY, a colour format outside the five, and the inference
 * GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL not being allowed. A zero width or height moves nothing. */
gpu_pgraph_result gpu_pgraph_replay_byte_copy(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                              const uint8_t *source, size_t source_length, uint8_t *destination,
                                              size_t destination_length, uint32_t *used_inferences,
                                              gpu_pgraph_report *report);
/* The bytes of a blit side's extent in a surface, the length a surface must hold for `height` rows of `row_pixels` pixels starting at the row `y` of `x`
 * pixels in: the last row's end. 0 when a field is out of range. `row_pixels` is the CLAMPED row (gpu_pgraph_blit_row_pixels). */
size_t gpu_pgraph_blit_extent_bytes(uint32_t colour_format, uint32_t pitch, uint32_t x, uint32_t y, uint32_t row_pixels, uint32_t height);
uint32_t gpu_pgraph_blit_bytes_per_pixel(uint32_t colour_format);
/* T769: the pixels a row of the blit moves, min(width, source_pitch / bpp, destination_pitch / bpp), 0 for a format without bytes per pixel. */
uint32_t gpu_pgraph_blit_row_pixels(const gpu_pgraph_copy *copy);
/* T832: gpu_pgraph_replay_copy with GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES allowed delegates here (gpu_pgraph_replay_copy_rules.c). */
gpu_pgraph_result gpu_pgraph_replay_copy_planner_rules(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                                       const gpu_image *source, gpu_image *destination,
                                                       uint32_t *used_inferences, gpu_pgraph_report *report);

#endif
