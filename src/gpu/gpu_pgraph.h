/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interpreter of the recorded NV2A command stream (T84), decoder half: method/data pairs in,
 * a state model and a draw list out. NO VULKAN HERE, so it is testable on any machine. The replay
 * of the draw list is gpu_pgraph_replay.h.
 *
 * INPUT. The pairs d3d8_gpu.c records (`d3d8_gpu_command`, same layout): the title's own pushes
 * and the blocks the ported library functions write. It is a STRICT SUBSET of what a console would
 * have run (docs/d3d8-usage.md section 14, d3d8_gpu.h), so a stream decoded here says what the replayed
 * subset did, not what the frame looked like.
 *
 * MEASURED METHODS ONLY. Every method this file acts on is one a ported library emitter writes,
 * with the number checked against the nxdk and xemu register headers (two derivations that agree,
 * numbers only). Where the emission is, in src/gpu:
 *
 *   0x0A20 / 0x0AF0   viewport offset / scale, 4 floats each    d3d8_shader.c vertex_viewport_emit
 *   0x0B00..0x0B7C    vertex program words, auto-advancing      d3d8_vertex_program.c
 *   0x0B80..0x0BFC    vertex constant words, auto-advancing     d3d8_vertex_constants.c
 *   0x1720 + 4n       vertex array n: guest address             d3d8_resource.c flush_draw_streams
 *   0x1760 + 4n       vertex array n: stride << 8 | size << 4 | type   (same)
 *   0x17FC            BEGIN_END, a primitive op, 0 ends it      d3d8_resource.c d3d8_draw_vertices
 *   0x1800 / 0x1808   ARRAY_ELEMENT16 (two indices per dword) / ARRAY_ELEMENT32   d3d8_indexed.c
 *   0x1810            DRAW_ARRAYS, (count - 1) << 24 | start    d3d8_resource.c
 *   0x1E94            transform execution mode                  d3d8_shader.c
 *   0x1E98            program context write enable, 0 only (T521, a counted no-op)   d3d8_shader.c
 *   0x1E9C            program load cursor, in 16-byte slots     d3d8_vertex_program.c
 *   0x1EA0            program start slot                        d3d8_shader.c
 *   0x1EA4            constant load cursor, in 16-byte rows     d3d8_vertex_constants.c
 *
 * OPT-IN: the register-combiner state (T75, gpu_pgraph_set_combiner). Off by default, so every
 * method below stays an unhandled (strict: refused) method exactly as before. When on, the
 * methods of the pixel-shader writer (d3d8_shader.c pixel_shader_apply, 0x003D9320) and of the
 * pixel-constant setter (0x003D9520) are held in a 57-dword shadow whose index is the D3D8
 * render-state index of the same word (d3d8_render_state_table.c), which is also the index of the
 * 60-dword pixel-shader definition (tools/nv2a_combiner/config.py):
 *
 *   0x0260 + 4i      idx 0..7    alpha input words              d3d8_shader.c block 0
 *   0x0288, 0x028C   idx 8, 9    final combiner words 0 and 1   same, emitted only if their OR != 0
 *   0x0A60..0x0ADC   idx 10..41  factor0, factor1, alpha out, colour in (8 each)  block 1 and
 *                                 0x003D9520 (factors, 0x0A60 + 0x20 * bank + 4 * stage)
 *   0x17F8           idx 42      shader clip-plane mode
 *   0x1E20, 0x1E24   idx 43, 44  final combiner constants       block 3 and 0x003D9520
 *   0x1E40..0x1E60   idx 45..53  colour output words (8) and the combiner control word
 *   0x1E70           idx 54      texture stage program (the unported texture-stage emitter 0x003DD930)
 *   0x1E74, 0x1E78   idx 55, 56  dot mapping and other-stage input
 *
 * OPT-IN: the OUTPUT STATE groups (T267, gpu_pgraph_set_output_groups). Off by default, so every
 * method below stays an unhandled (strict: refused) method exactly as before. Each group holds
 * the methods a MEASURED emitter writes (the byte of evidence is stated per group in
 * gpu_pgraph_replay.h, which also says what the replay does with it and what it refuses):
 *
 *   SCISSOR   0x0200 / 0x0204  surface clip horizontal / vertical, x | width << 16, y | height << 16
 *             0x02B4           window clip type
 *             0x02C0 / 0x02E0  window clip 0 horizontal / vertical
 *             d3d8_scissor.c (the port of SetScissors 0x003D4470, checked word for word against the
 *             original under emulation: tests/test_d3d8_scissor_oracle.py). Only window clip entry 0
 *             is written, so 0x02C4..0x02DC and 0x02E4..0x02FC stay unhandled.
 *   CULL      0x0308 cull enable, 0x039C cull face, 0x03A0 front face
 *             read from the BYTES of the library's per-state helpers 0x003D7060 (enable = mode != 0,
 *             face = 0x404 + (mode != the stored front face)) and 0x003D70D0 (front face = its argument).
 *             NO CURRENT PRODUCER: those handlers are ported without writing their pairs
 *             (docs/d3d8-usage.md 13.6), so today's recorded stream never holds these words.
 *   BLEND     0x0304 enable, 0x0344 source factor, 0x0348 destination factor, 0x034C blend colour,
 *             0x0350 equation, 0x0358 colour mask
 *   ALPHA_TEST 0x0300 enable, 0x033C function, 0x0340 reference
 *             both from the title's own state table (35 records of 0x18 bytes at 0x4B8D2C of the
 *             retail image, the 19 immediate records carry literal NV2A-encoded values, pinned by
 *             tests/test_gpu_state_census.py) through the inlined SetRenderState and the primitive
 *             0x003D6C90, whose pairs ARE in the recorded stream (docs/d3d8-usage.md 13.5).
 *   DEPTH_STENCIL 0x030C depth test enable, 0x0354 depth function, 0x035C depth mask, 0x032C stencil test
 *             enable, 0x0360 stencil write mask, 0x0364 stencil function, 0x0368 stencil reference,
 *             0x036C stencil function mask, 0x0370 / 0x0374 / 0x0378 stencil op on fail / depth fail / pass.
 *             The title's own table writes 0x0354, 0x035C, 0x0364, 0x0368, 0x0374, 0x0378 (stream producer);
 *             the two enables are written by SetRenderTarget (0x003D39E3, 0x003D3A05, ELIDED by its port,
 *             so no producer) and the other three words by no recovered emitter in the stream.
 *   CLEAR     0x1D8C depth and stencil clear value, 0x1D90 colour clear value, 0x1D98 / 0x1D9C clear rectangle
 *             (horizontal xmin | xmax << 16, vertical ymin | ymax << 16, both INCLUSIVE), and 0x1D94 CLEAR_SURFACE,
 *             which is an EVENT: the flags (Z 1, stencil 2, R 0x10, G 0x20, B 0x40, A 0x80) say what to clear and the
 *             decoder records one gpu_pgraph_clear at the point in the draw order where it was written. Read from the
 *             BYTES of the library's Clear (0x003D5EB0 at 0x003D61C4 and 0x003D61E9: one 0x81D98 header and one 0x0C1D8C
 *             run per rectangle, the rectangle words `min | (max_exclusive - 1) << 16`, the flags word the D3D flags).
 *             NO CURRENT PRODUCER: Clear is ported without writing pairs (docs/d3d8-usage.md 13.6). SURFACE_FORMAT
 *             0x0208, which Clear also writes, is not decoded.
 *
 * Any OTHER method is not interpreted. It is counted by number (gpu_pgraph_unhandled) so the gap
 * is a table, and in strict mode it is refused. A shape of a measured method that no emitter
 * produces (a draw outside BEGIN_END, vertex-stage state changing inside one, a data run past the
 * program or constant file) is refused with GPU_PGRAPH_ERR_MALFORMED or ERR_UNMEASURED and a
 * message from gpu_pgraph_error, never guessed at.
 *
 * WHAT THIS MODEL DOES NOT DECIDE. See gpu_pgraph_replay.h for the inferences the replay needs
 * and the policy mask that refuses them unless a caller allows them.
 */

#ifndef TSFP_GPU_PGRAPH_H
#define TSFP_GPU_PGRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPU_PGRAPH_ATTRIBUTES 16u
#define GPU_PGRAPH_PROGRAM_SLOTS 136u   /* 16-byte instructions, D3D8_VERTEX_PROGRAM_SLOTS */
#define GPU_PGRAPH_CONSTANT_ROWS 192u   /* 16-byte rows, D3D8_CONSTANT_REGISTERS */
/* T1268: PER-FRAME bounds (every list restarts at begin_frame), sized as memory budgets and not as a guess: a snapshot is
 * sizeof(gpu_pgraph_state) = 6404 bytes, a draw 608. The old 512 snapshots were hit by one heavy frame (~4000 of the Story
 * level, more than 512 distinct state changes between draws) and latched the whole replay off (the frozen video). The peaks are in
 * gpu_pgraph_stats (snapshots_peak, draws_peak, indices_peak) and in the replay's stop line. */
#define GPU_PGRAPH_MAX_DRAWS 131072u    /* about 76 MiB of draws, above the snapshots so each bound is reachable alone */
#define GPU_PGRAPH_MAX_SNAPSHOTS 65536u /* about 400 MiB of snapshots, only reached by a frame of 65536 state changes */
#define GPU_PGRAPH_MAX_INDICES (1u << 26)
#define GPU_PGRAPH_UNHANDLED_TABLE 128u
#define GPU_PGRAPH_COMBINER_BLOCK_BYTES 240u /* T847: the 60 dword definition block of tools/nv2a_combiner (57 words, three zeros) */
#define GPU_PGRAPH_COMBINER_WORDS 57u   /* render-state indices 0..56, see the OPT-IN block above */

/* T267 output state groups, a bit mask for gpu_pgraph_set_output_groups and the backend's
 * `output_groups`. NOT part of any "everything" mask: each is an opt-in. */
#define GPU_PGRAPH_OUTPUT_SCISSOR 0x01u
#define GPU_PGRAPH_OUTPUT_CULL 0x02u
#define GPU_PGRAPH_OUTPUT_BLEND 0x04u
#define GPU_PGRAPH_OUTPUT_ALPHA_TEST 0x08u
#define GPU_PGRAPH_OUTPUT_DEPTH_STENCIL 0x10u
#define GPU_PGRAPH_OUTPUT_CLEAR 0x20u
/* T502: polygon offset, the five words the title's state table and its ZBIAS handler write. */
#define GPU_PGRAPH_OUTPUT_POLYGON_OFFSET 0x40u
/* T502: words a title writes that the replay SKIPS ON PURPOSE (dither, the specular parameters): decoded,
 * validated as written, counted in gpu_pgraph_stats.pairs_ignored and named in gpu_pgraph_replay.h, never
 * silently dropped. */
#define GPU_PGRAPH_OUTPUT_IGNORED 0x80u
/* T462 / T541, the Swap flag 1 copy composition and the SetRenderTarget packet. SURFACE: the bind packet's NO_OPERATION and
 * WAIT_FOR_IDLE (value 0, counted), software NO_OPERATION(9) control-register writes, the surface
 * format, pitch and offsets, CONTROL0, the depth clip range and the
 * anti-aliasing control. FIXED: the fixed-function state words the program execution mode does not run (lighting, fog and
 * point enables, the polygon modes, the z-cull enable), decoded for the values measured and refused for any other. TEXTURE:
 * stage address, control 0 and filter and the bump environment of stages 1 to 3. IMMEDIATE: SET_VERTEX_DATA2F_M vertices
 * inside a BEGIN_END bracket (no guest array). All four are in ALL_MEASURED and need the replay to enable them too. */
#define GPU_PGRAPH_OUTPUT_SURFACE 0x200u
#define GPU_PGRAPH_OUTPUT_FIXED 0x400u
#define GPU_PGRAPH_OUTPUT_TEXTURE 0x800u
#define GPU_PGRAPH_OUTPUT_IMMEDIATE 0x1000u
/* T578, the 2D engine blit CopyRects emits (`d3d8_copy_rects`): the commands on subchannels 2 (image blit, class 0x9F) and 3
 * (context surfaces 2D, class 0x62). Decoded into copy events (gpu_pgraph_copy), refused by name outside the measured set. In
 * ALL_MEASURED, and the swap replay applies them over its surface images (d3d8_swap_replay.h). */
#define GPU_PGRAPH_OUTPUT_BLIT 0x2000u
#define GPU_PGRAPH_OUTPUT_COMPOSITION \
    (GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_IMMEDIATE)
#define GPU_PGRAPH_OUTPUT_ALL_MEASURED                                                        \
    (GPU_PGRAPH_OUTPUT_COMPOSITION | GPU_PGRAPH_OUTPUT_SCISSOR | GPU_PGRAPH_OUTPUT_CULL |      \
     GPU_PGRAPH_OUTPUT_BLEND | GPU_PGRAPH_OUTPUT_ALPHA_TEST | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL | \
     GPU_PGRAPH_OUTPUT_CLEAR | GPU_PGRAPH_OUTPUT_BLIT | \
     GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_IGNORED)
#define GPU_PGRAPH_CLEAR_SURFACE 0x1D94u /* the clear EVENT, see CLEAR above */
#define GPU_PGRAPH_MAX_CLEARS 256u
#define GPU_PGRAPH_MAX_COPIES 256u

/* The output-state words the decoder shadows when their group is on. The value is the raw dword the
 * stream wrote (the replay interprets it), or the equivalent method encoding of a software
 * control-register write (T1176). `output_written[word]` says the stream established it. */
typedef enum {
    GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL = 0, /* 0x0200 x | width << 16 */
    GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL,       /* 0x0204 y | height << 16 */
    GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE,            /* 0x02B4 */
    GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL,      /* 0x02C0 entry 0, xmin | xmax << 16 */
    GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL,        /* 0x02E0 entry 0, ymin | ymax << 16 */
    GPU_PGRAPH_OUT_CULL_ENABLE,                 /* 0x0308 0 or 1 */
    GPU_PGRAPH_OUT_CULL_FACE,                   /* 0x039C 0x404 front, 0x405 back */
    GPU_PGRAPH_OUT_FRONT_FACE,                  /* 0x03A0 0x900 CW, 0x901 CCW */
    GPU_PGRAPH_OUT_BLEND_ENABLE,                /* 0x0304 0 or 1 */
    GPU_PGRAPH_OUT_BLEND_SFACTOR,               /* 0x0344 */
    GPU_PGRAPH_OUT_BLEND_DFACTOR,               /* 0x0348 */
    GPU_PGRAPH_OUT_BLEND_COLOR,                 /* 0x034C */
    GPU_PGRAPH_OUT_BLEND_EQUATION,              /* 0x0350 */
    GPU_PGRAPH_OUT_COLOR_MASK,                  /* 0x0358 B 1 << 0, G 1 << 8, R 1 << 16, A 1 << 24 */
    GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE,           /* 0x0300 0 or 1 */
    GPU_PGRAPH_OUT_ALPHA_FUNC,                  /* 0x033C 0x200 NEVER .. 0x207 ALWAYS */
    GPU_PGRAPH_OUT_ALPHA_REF,                   /* 0x0340 */
    GPU_PGRAPH_OUT_DEPTH_ENABLE,                /* 0x030C 0 or 1 */
    GPU_PGRAPH_OUT_DEPTH_FUNC,                  /* 0x0354 0x200 NEVER .. 0x207 ALWAYS */
    GPU_PGRAPH_OUT_DEPTH_MASK,                  /* 0x035C 0 or 1, depth writes */
    GPU_PGRAPH_OUT_STENCIL_ENABLE,              /* 0x032C 0 or 1 */
    GPU_PGRAPH_OUT_STENCIL_MASK,                /* 0x0360 write mask */
    GPU_PGRAPH_OUT_STENCIL_FUNC,                /* 0x0364 0x200 .. 0x207 */
    GPU_PGRAPH_OUT_STENCIL_REF,                 /* 0x0368 */
    GPU_PGRAPH_OUT_STENCIL_FUNC_MASK,           /* 0x036C compare mask */
    GPU_PGRAPH_OUT_STENCIL_OP_FAIL,             /* 0x0370 */
    GPU_PGRAPH_OUT_STENCIL_OP_ZFAIL,            /* 0x0374 */
    GPU_PGRAPH_OUT_STENCIL_OP_ZPASS,            /* 0x0378 */
    GPU_PGRAPH_OUT_CLEAR_ZSTENCIL,              /* 0x1D8C */
    GPU_PGRAPH_OUT_CLEAR_COLOR,                 /* 0x1D90 */
    GPU_PGRAPH_OUT_CLEAR_RECT_HORIZONTAL,       /* 0x1D98 */
    GPU_PGRAPH_OUT_CLEAR_RECT_VERTICAL,         /* 0x1D9C */
    GPU_PGRAPH_OUT_POLY_OFFSET_POINT,           /* 0x0330 enable, 0 or 1 */
    GPU_PGRAPH_OUT_POLY_OFFSET_LINE,            /* 0x0334 enable, 0 or 1 */
    GPU_PGRAPH_OUT_POLY_OFFSET_FILL,            /* 0x0338 enable, 0 or 1 */
    GPU_PGRAPH_OUT_POLY_OFFSET_SCALE,           /* 0x0384 float32 slope factor */
    GPU_PGRAPH_OUT_POLY_OFFSET_BIAS,            /* 0x0388 float32 constant bias */
    GPU_PGRAPH_OUT_DITHER_ENABLE,               /* 0x0310 on the 3D subchannel, IGNORED group */
    GPU_PGRAPH_OUT_SPECULAR_PARAMS,             /* 0x09F8, IGNORED group */
    /* T462 SURFACE group */
    GPU_PGRAPH_OUT_SURFACE_FORMAT,              /* 0x0208 colour 0xF, zeta 0xF0, type 0xF00, antialiasing 0xF000, log2 size */
    GPU_PGRAPH_OUT_SURFACE_PITCH,               /* 0x020C colour pitch low 16 bits, zeta pitch high 16 */
    GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET,        /* 0x0210 */
    GPU_PGRAPH_OUT_SURFACE_ZETA_OFFSET,         /* 0x0214 */
    GPU_PGRAPH_OUT_CONTROL0,                    /* 0x0290 */
    GPU_PGRAPH_OUT_CLIP_MIN,                    /* 0x0394 float32 */
    GPU_PGRAPH_OUT_CLIP_MAX,                    /* 0x0398 float32 */
    GPU_PGRAPH_OUT_ANTI_ALIASING,               /* 0x1D7C */
    /* T462 FIXED group */
    GPU_PGRAPH_OUT_LIGHTING_ENABLE,             /* 0x0314 */
    GPU_PGRAPH_OUT_LIGHT_CONTROL,               /* 0x0294 */
    GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK,           /* 0x03BC */
    GPU_PGRAPH_OUT_SPECULAR_ENABLE,             /* 0x03B8 */
    GPU_PGRAPH_OUT_FOG_ENABLE,                  /* 0x02A4 */
    GPU_PGRAPH_OUT_FOG_COLOR,                   /* 0x02A8 raw ABGR */
    GPU_PGRAPH_OUT_FOG_MODE,                    /* 0x029C */
    GPU_PGRAPH_OUT_FOG_GEN_MODE,                /* 0x02A0 */
    GPU_PGRAPH_OUT_FOG_PARAM0,                  /* 0x09C0 float32 */
    GPU_PGRAPH_OUT_FOG_PARAM1,                  /* 0x09C4 float32 */
    GPU_PGRAPH_OUT_FOG_PARAM2,                  /* 0x09C8 float32 */
    GPU_PGRAPH_OUT_POINT_PARAMS_ENABLE,         /* 0x0318 */
    GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE,         /* 0x031C */
    GPU_PGRAPH_OUT_POINT_SIZE,                  /* 0x043C */
    GPU_PGRAPH_OUT_FRONT_POLYGON_MODE,          /* 0x038C */
    GPU_PGRAPH_OUT_BACK_POLYGON_MODE,           /* 0x0390 */
    GPU_PGRAPH_OUT_ZCULL_ENABLE,                /* 0x1D84, the name is INFERRED */
    /* T462 TEXTURE group: stage s address, control 0 and filter, then the bump environment of stages 1 to 3 (matrix
     * 0 to 3, scale, offset). */
    GPU_PGRAPH_OUT_TEXTURE_ADDRESS,             /* 0x1B08 + 0x40 * s, four words */
    GPU_PGRAPH_OUT_TEXTURE_CONTROL0 = GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 4,   /* 0x1B0C + 0x40 * s */
    GPU_PGRAPH_OUT_TEXTURE_FILTER = GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 4,    /* 0x1B14 + 0x40 * s */
    GPU_PGRAPH_OUT_TEXTURE_BUMP = GPU_PGRAPH_OUT_TEXTURE_FILTER + 4,        /* 0x1B28 + 0x40 * s + 4 * n, s 1..3, n 0..5 */
    GPU_PGRAPH_OUT_COUNT = GPU_PGRAPH_OUT_TEXTURE_BUMP + 18
} gpu_pgraph_output_word;

/* Measured method numbers. */
#define GPU_PGRAPH_VIEWPORT_OFFSET 0x0A20u
#define GPU_PGRAPH_VIEWPORT_SCALE 0x0AF0u
#define GPU_PGRAPH_PROGRAM_DATA 0x0B00u
#define GPU_PGRAPH_PROGRAM_DATA_END 0x0B80u
#define GPU_PGRAPH_CONSTANT_DATA 0x0B80u
#define GPU_PGRAPH_CONSTANT_DATA_END 0x0C00u
#define GPU_PGRAPH_ARRAY_OFFSET 0x1720u
#define GPU_PGRAPH_ARRAY_FORMAT 0x1760u
#define GPU_PGRAPH_BEGIN_END 0x17FCu
#define GPU_PGRAPH_ARRAY_ELEMENT16 0x1800u
#define GPU_PGRAPH_ARRAY_ELEMENT32 0x1808u
#define GPU_PGRAPH_DRAW_ARRAYS 0x1810u
#define GPU_PGRAPH_EXECUTION_MODE 0x1E94u
#define GPU_PGRAPH_CXT_WRITE_EN 0x1E98u
#define GPU_PGRAPH_PROGRAM_LOAD 0x1E9Cu
#define GPU_PGRAPH_PROGRAM_START 0x1EA0u
#define GPU_PGRAPH_CONSTANT_LOAD 0x1EA4u
/* T462. NO_OPERATION and WAIT_FOR_IDLE (the SURFACE group, value 0 only), and SET_VERTEX_DATA2F_M, two dwords per attribute
 * slot (x then y, 16 slots), inside a BEGIN_END bracket (the IMMEDIATE group). */
#define GPU_PGRAPH_NO_OPERATION 0x0100u
#define GPU_PGRAPH_WAIT_FOR_IDLE 0x0110u
#define GPU_PGRAPH_VERTEX_DATA2F 0x1880u
#define GPU_PGRAPH_VERTEX_DATA2F_END 0x1900u
#define GPU_PGRAPH_MAX_INLINE_VERTICES 4096u

/* SET_BEGIN_END operations (nxdk and xemu headers agree). The library writes the title's number
 * unchanged (d3d8_draw_vertices), so these are the hardware's numbers, not PC Direct3D's (T84c: the title's own
 * translators 0x18EA0, 0x18FF0, 0x1E770 emit 5 for a triangle list and 6 for a strip, tools/primtypes). */
#define GPU_PGRAPH_OP_END 0u
#define GPU_PGRAPH_OP_POINTS 1u
#define GPU_PGRAPH_OP_LINES 2u
#define GPU_PGRAPH_OP_LINE_LOOP 3u
#define GPU_PGRAPH_OP_LINE_STRIP 4u
#define GPU_PGRAPH_OP_TRIANGLES 5u
#define GPU_PGRAPH_OP_TRIANGLE_STRIP 6u
#define GPU_PGRAPH_OP_TRIANGLE_FAN 7u
#define GPU_PGRAPH_OP_QUADS 8u
#define GPU_PGRAPH_OP_QUAD_STRIP 9u
#define GPU_PGRAPH_OP_POLYGON 10u

/* Vertex array element types (low nibble of the format word). */
#define GPU_PGRAPH_TYPE_UB_D3D 0u
#define GPU_PGRAPH_TYPE_S1 1u
#define GPU_PGRAPH_TYPE_F 2u
#define GPU_PGRAPH_TYPE_UB_OGL 4u
#define GPU_PGRAPH_TYPE_S32K 5u
#define GPU_PGRAPH_TYPE_CMP 6u

/* Execution mode word: low two bits 2 is "program" (the title writes 6, 6 & 3 == 2). */
#define GPU_PGRAPH_EXECUTION_MODE_PROGRAM 2u

typedef enum {
    GPU_PGRAPH_OK = 0,
    GPU_PGRAPH_ERR_ARGUMENT,
    GPU_PGRAPH_ERR_UNMEASURED, /* a shape or value no measured emitter produces: refused */
    GPU_PGRAPH_ERR_MALFORMED,  /* a measured method used impossibly (past a file, unbalanced) */
    GPU_PGRAPH_ERR_FULL,       /* a bound was reached (draws, snapshots, indices) */
    GPU_PGRAPH_ERR_MEMORY,
    GPU_PGRAPH_ERR_DEVICE      /* the Vulkan side failed (replay only) */
} gpu_pgraph_result;

const char *gpu_pgraph_result_string(gpu_pgraph_result result);

/* Guest memory read: fills `bytes` at the guest `address` and returns false when it cannot. */
typedef bool (*gpu_pgraph_read_fn)(void *context, uint32_t address, void *out, size_t bytes);

/* The 3D subchannel's (method, data) pairs. A recording's commands on other subchannels (header bits 13-15,
 * T391) never enter this array: the caller routes each one through gpu_pgraph_decode_other_subchannel at its
 * place in the order, so the pair counter stays aligned with the recording. */
typedef struct {
    uint32_t method;
    uint32_t data;
} gpu_pgraph_command;

/* T391: the fence packet 0x003D67B0 writes (docs/d3d8-draw-cascade.md). Its software method 0x310 on subchannel
 * 5 is counted and not replayed, any other command on a subchannel other than 0 is REFUSED (nothing measured
 * says what it would do), and its SEMAPHORE_RELEASE (0x1D70, subchannel 0) is a handled no-op: it orders the
 * GPU against the CPU and draws nothing. The packet's last two commands, 0x1D90 with 0 (the colour clear
 * value), follow the software method and are consumed as part of the packet when the clear group is off, since
 * refusing them would refuse every frame that holds a fence. With the clear group on they are applied as the
 * writes they are (the library resets the clear colour to 0 at every fence). */
#define GPU_PGRAPH_SUBCHANNEL_3D 0u
#define GPU_PGRAPH_SUBCHANNEL_SOFTWARE 5u
#define GPU_PGRAPH_SOFTWARE_FENCE_NOTIFY 0x0310u

/* T578. The 2D engine's two subchannels, MEASURED in the original CreateDevice init (0x003DA407, docs/d3d8-copy-composition.md
 * section "The 2D engine blit"): SET_OBJECT binds handle 0x10 to subchannel 2 (class 0x9F, the class table at 0x003DAEFE pushes
 * `0x9F` for handle 0x10) and handle 0x11 to subchannel 3 (class 0x62). The method NAMES are the xemu headers' (INFERRED). */
#define GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT 2u
#define GPU_PGRAPH_SUBCHANNEL_SURFACES_2D 3u
#define GPU_PGRAPH_BLIT_HANDLE 0x10u            /* subchannel 2 object, class 0x9F NV_IMAGE_BLIT */
#define GPU_PGRAPH_SURFACES_2D_HANDLE 0x11u     /* subchannel 3 object, class 0x62 NV_CONTEXT_SURFACES_2D */
#define GPU_PGRAPH_SET_OBJECT 0x0000u           /* both subchannels */
#define GPU_PGRAPH_S2D_DMA_SOURCE 0x0184u       /* subchannel 3, measured 3 (CreateDevice init) */
#define GPU_PGRAPH_S2D_DMA_DESTINATION 0x0188u  /* subchannel 3, measured 0xB */
#define GPU_PGRAPH_S2D_COLOR_FORMAT 0x0300u     /* subchannel 3 */
#define GPU_PGRAPH_S2D_PITCH 0x0304u            /* subchannel 3, source pitch low 16, destination pitch high 16 */
#define GPU_PGRAPH_S2D_OFFSET_SOURCE 0x0308u    /* subchannel 3, the surface header's Data word */
#define GPU_PGRAPH_S2D_OFFSET_DESTINATION 0x030Cu
#define GPU_PGRAPH_BLIT_CONTEXT_FIRST 0x0184u   /* subchannel 2: colour key, clip, pattern, rop, beta1, beta4, all 0x19 */
#define GPU_PGRAPH_BLIT_CONTEXT_SURFACES 0x019Cu /* subchannel 2, the surfaces object handle, measured 0x11 */
#define GPU_PGRAPH_BLIT_OPERATION 0x02FCu       /* subchannel 2, measured 3 (SRCCOPY) */
#define GPU_PGRAPH_BLIT_POINT_IN 0x0300u        /* subchannel 2, x | y << 16 */
#define GPU_PGRAPH_BLIT_POINT_OUT 0x0304u
#define GPU_PGRAPH_BLIT_SIZE 0x0308u            /* subchannel 2, width | height << 16, the write RUNS the blit */
#define GPU_PGRAPH_BLIT_OPERATION_SRCCOPY 3u
#define GPU_PGRAPH_BLIT_FORMAT_Y8 0x01u         /* 1 byte per pixel (the byte path of CopyRects) */
#define GPU_PGRAPH_BLIT_FORMAT_R5G6B5 0x04u
#define GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0 0x06u  /* T832, HQ58: alpha forced 0 (the replay copy only, the decoder refuses it) */
#define GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF 0x07u /* T832, HQ58: alpha forced 0xFF */
#define GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 0x0Au
#define GPU_PGRAPH_SEMAPHORE_RELEASE 0x1D70u

/* The vertex-stage state a draw sees, copied when the draw ends. */
typedef struct {
    float constants[GPU_PGRAPH_CONSTANT_ROWS * 4u]; /* hardware rows, D3D8 c[n] is row n + 96 */
    bool constant_written[GPU_PGRAPH_CONSTANT_ROWS];
    uint32_t program[GPU_PGRAPH_PROGRAM_SLOTS * 4u];
    bool slot_written[GPU_PGRAPH_PROGRAM_SLOTS];
    bool program_start_set;
    uint32_t program_start;
    bool execution_mode_set;
    uint32_t execution_mode;
    bool cxt_write_en_set; /* T521: 0x1E98 was written (value 0, the only one decoded) */
    uint32_t cxt_write_en;
    bool viewport_offset_set;
    bool viewport_scale_set;
    float viewport_offset[4];
    float viewport_scale[4];
    /* The combiner shadow (OPT-IN). `combiner_captured` says the model was decoding these methods:
     * when false, `combiner` is all zero and means nothing. `combiner_written[i]` is true once the
     * stream wrote word i. */
    bool combiner_captured;
    uint32_t combiner[GPU_PGRAPH_COMBINER_WORDS];
    bool combiner_written[GPU_PGRAPH_COMBINER_WORDS];
    /* The output-state shadow (T267, OPT-IN). `output_groups` is the set of groups the model was
     * decoding: a word of a group that is not in it was never captured and means nothing. */
    uint32_t output_groups;
    uint32_t output[GPU_PGRAPH_OUT_COUNT];
    bool output_written[GPU_PGRAPH_OUT_COUNT];
} gpu_pgraph_state;

typedef struct {
    bool address_set;
    bool format_set;
    uint32_t address; /* a guest address, as recorded (no mask applied) */
    uint32_t format;  /* stride << 8 | size << 4 | type */
} gpu_pgraph_array;

typedef struct {
    uint32_t type;
    uint32_t size;   /* components, 0 means the slot is disabled */
    uint32_t stride; /* bytes */
} gpu_pgraph_format;

gpu_pgraph_format gpu_pgraph_decode_format(uint32_t format_word);

/* Bytes one element of a vertex array occupies for the shapes the replay converts (F sizes 1 to 4,
 * UB_D3D size 4, S32K size 2, T84d), 0 for every other type and size (refused by the replay, never
 * read, so never snapshotted). The ONE definition both the snapshot and the vertex fetch use. */
uint32_t gpu_pgraph_element_bytes(uint32_t type, uint32_t size);

/* T262. The vertex bytes of one array slot of one draw, copied out of guest memory when the draw's
 * BEGIN_END bracket ended in the decoder: guest bytes [address, address + bytes), held at `offset` in
 * the model's snapshot pool. Read them through gpu_pgraph_draw_vertex_bytes. */
typedef struct {
    bool captured;
    uint32_t address;
    uint32_t bytes;
    size_t offset;
} gpu_pgraph_vertex_range;

typedef struct {
    uint32_t primitive;         /* GPU_PGRAPH_OP_* */
    uint32_t first_command;     /* index in the whole decoded stream of the BEGIN */
    uint32_t first_index;       /* into gpu_pgraph_indices */
    uint32_t index_count;
    uint32_t snapshot;          /* gpu_pgraph_snapshot index: the state at END */
    gpu_pgraph_array arrays[GPU_PGRAPH_ATTRIBUTES];
    /* T262: true when the vertex bytes the draw reads were snapshotted (gpu_pgraph_set_vertex_capture
     * was on and the draw is replayable), and `vertices[n]` then says where slot n's bytes are. When
     * false the replay reads guest memory itself, at the replay, as before. */
    bool vertices_captured;
    gpu_pgraph_vertex_range vertices[GPU_PGRAPH_ATTRIBUTES];
    /* T462: the vertices came from SET_VERTEX_DATA2F_M writes inside the bracket, not from a guest array. Every slot
     * that was written holds `index_count` (x, y) pairs of float32 in the model's inline pool (`vertices[slot]`,
     * `arrays[slot]` describes them as stride 8, size 2, type F at guest address 0), the others are disabled. The
     * vertex bytes are ALWAYS held (`vertices_captured`), whatever the guest snapshot setting. */
    bool inline_vertices;
} gpu_pgraph_draw;

/* T267, the CLEAR group: one CLEAR_SURFACE event with the clear words as they stood when it was written. */
typedef struct {
    uint32_t before_draw;  /* draws of the frame decoded before it: it precedes draw `before_draw` */
    uint32_t command;      /* index in the whole decoded stream of the CLEAR_SURFACE pair */
    uint32_t flags;        /* the CLEAR_SURFACE value: Z 1, stencil 2, R 0x10, G 0x20, B 0x40, A 0x80 */
    uint32_t zstencil;
    uint32_t color;
    uint32_t rect_horizontal;
    uint32_t rect_vertical;
    bool zstencil_written;
    bool color_written;
    bool rect_horizontal_written;
    bool rect_vertical_written;
    /* T848: the surface words (0x0208 format, 0x020C pitch, 0x0210 colour offset) as they stood at the CLEAR_SURFACE, so a clear
     * names its own render target (a frame with no draw, or a SetRenderTarget between the clear and the next draw, has no
     * draw snapshot that says it). `surface_written` is true only when all three were written. */
    uint32_t surface_format;
    uint32_t surface_pitch;
    uint32_t surface_color_offset;
    bool surface_written;
} gpu_pgraph_clear;

/* T578, the BLIT group: one image blit as it stood when the SIZE word was written. `source_offset` and
 * `destination_offset` are the surface headers' Data words, the colour format and pitches as written, every field refers to
 * the writes the stream made (a blit with any of the five surface words or either point never written is refused). The operation
 * is the measured SRCCOPY (3) written once by CreateDevice, which the recorded stream does not carry. */
typedef struct {
    uint32_t before_draw;  /* draws of the frame decoded before it: it follows draw `before_draw` - 1 */
    uint32_t before_clear; /* clear events of the frame decoded before it */
    uint32_t command;      /* index in the whole decoded stream of the SIZE pair */
    uint32_t source_offset;
    uint32_t destination_offset;
    uint32_t color_format;
    uint32_t source_pitch;
    uint32_t destination_pitch;
    uint32_t in_x;
    uint32_t in_y;
    uint32_t out_x;
    uint32_t out_y;
    uint32_t width;
    uint32_t height;
    uint32_t operation;
} gpu_pgraph_copy;

/* T998: ordered query methods, opt-in until the renderer supplies real counts. */
#define GPU_PGRAPH_MAX_QUERY_EVENTS 4096u
typedef struct {
    uint32_t before_draw;
    uint64_t command;
    uint32_t method;
    uint32_t data;
    uint32_t report_address;
    uint64_t report_generation;
} gpu_pgraph_query;
typedef void (*gpu_pgraph_report_identity_fn)(void *context, gpu_pgraph_query *event);

typedef struct gpu_pgraph gpu_pgraph;

/* Create an empty model, or NULL when memory is refused. */
gpu_pgraph *gpu_pgraph_create(void);
void gpu_pgraph_destroy(gpu_pgraph *pgraph);
/* Forget the state, the draws and the counters. Strictness is kept. */
void gpu_pgraph_reset(gpu_pgraph *pgraph);
/* Opt in to decoding the register-combiner methods (T75) into gpu_pgraph_state.combiner. Off by
 * default: the methods are then unhandled, counted and (strict) refused as before. Kept across
 * gpu_pgraph_reset, like strictness. */
void gpu_pgraph_set_combiner(gpu_pgraph *pgraph, bool enabled);
/* The render-state index of a combiner method (0..56), or -1 when the method is not one. */
int gpu_pgraph_combiner_index(uint32_t method);
/* T267. Opt in to decoding the output-state groups in `groups` (GPU_PGRAPH_OUTPUT_*) into
 * gpu_pgraph_state.output. Off (0) by default: those methods are then unhandled, counted and (strict)
 * refused as before. Bits that are not a group are ignored. Kept across gpu_pgraph_reset, like
 * strictness. Refused inside a BEGIN_END bracket, like the combiner words: no measured emitter writes
 * them there, and the snapshot at END would otherwise describe the wrong state. */
void gpu_pgraph_set_output_groups(gpu_pgraph *pgraph, uint32_t groups);
/* The word of an output-state method, or -1 when the method is not one. `group` (when not NULL) gets
 * the group's GPU_PGRAPH_OUTPUT_* bit. */
int gpu_pgraph_output_index(uint32_t method, uint32_t *group);
/* The group bit of word `word` (a gpu_pgraph_output_word), 0 for a value outside the enum. */
uint32_t gpu_pgraph_output_group(uint32_t word);
/* Strict: a method that is not in the measured list is refused instead of counted. */
void gpu_pgraph_set_strict(gpu_pgraph *pgraph, bool strict);
/* Default off, preserved across reset. Does not fabricate report completion. */
void gpu_pgraph_set_visibility(gpu_pgraph *pgraph, bool enabled);
void gpu_pgraph_set_report_identity(gpu_pgraph *pgraph, gpu_pgraph_report_identity_fn capture, void *context);
size_t gpu_pgraph_query_count(const gpu_pgraph *pgraph);
const gpu_pgraph_query *gpu_pgraph_query_at(const gpu_pgraph *pgraph, size_t index);

/* T262, OPT-IN, default off. Snapshot each draw's vertex bytes AT THE MOMENT THE DECODER SEES ITS
 * BEGIN_END(0), through `read`. The moment is the decoder's, which is whenever the caller feeds it the
 * pair: the swap replay feeds it at the instantaneous GPU's kick (d3d8_swap_replay.h), so the bytes are
 * the guest memory as it stood when the draw was handed to the GPU, and a buffer the title rewrites
 * afterwards (or an earlier render target pass reuses) cannot change what a recorded draw replays.
 *
 * What is copied, per draw and array slot: the guest bytes from the first to the last vertex the replay
 * will fetch (the indices after primitive expansion, so a dropped remainder is not read), one element
 * wide at the end. Ranges of one draw that overlap are read once. A slot of a type the replay does not
 * convert, a draw whose primitive or size the replay refuses, and a slot with no address are not copied
 * (the replay refuses them before it reads a byte).
 *
 * BOUNDED: `budget_bytes` is the most the snapshot pool may hold for one frame (gpu_pgraph_begin_frame
 * and gpu_pgraph_reset empty it). A draw that would pass it is REFUSED with GPU_PGRAPH_ERR_FULL and a
 * message naming the draw, the slot, the bytes it needed and the bytes held; nothing is truncated. A
 * failed `read` is GPU_PGRAPH_ERR_MALFORMED at the same point. `read` NULL or `budget_bytes` 0 turns it
 * off. Kept across gpu_pgraph_reset, like strictness. */
void gpu_pgraph_set_vertex_capture(gpu_pgraph *pgraph, gpu_pgraph_read_fn read, void *context,
                                   size_t budget_bytes);
bool gpu_pgraph_vertex_capture_enabled(const gpu_pgraph *pgraph);
/* Bytes the snapshot pool holds for the frame now. */
size_t gpu_pgraph_vertex_bytes_held(const gpu_pgraph *pgraph);
/* The snapshot of `slot` of draw `draw_index`: a pointer to its first byte (guest address *address,
 * *length bytes), or NULL when the slot was not copied. Valid until the model decodes again. */
const uint8_t *gpu_pgraph_draw_vertex_bytes(const gpu_pgraph *pgraph, size_t draw_index,
                                            uint32_t slot, uint32_t *address, uint32_t *length);
/* True while a BEGIN_END bracket is open, so a caller that decodes in kicks can tell a draw is half
 * decoded. */
bool gpu_pgraph_in_bracket(const gpu_pgraph *pgraph);

/* Start a new frame: forget the draw list, its indices and the snapshots, KEEP the state (the
 * program and constant files, the arrays, the viewport, the cursors) because hardware state
 * persists across a present. MALFORMED when a BEGIN_END bracket is open: the indices of the open
 * bracket would be lost. The statistics and the unhandled table keep accumulating. */
gpu_pgraph_result gpu_pgraph_begin_frame(gpu_pgraph *pgraph);

/* Decode `count` pairs. The state carries over between calls (a stream arrives in kicks). On a
 * refusal, decoding stops AT the offending pair, everything before it stays decoded, and
 * gpu_pgraph_error says why. A model that refused must be reset before reuse. */
gpu_pgraph_result gpu_pgraph_decode(gpu_pgraph *pgraph, const gpu_pgraph_command *commands,
                                    size_t count);

/* The message of the last refusal, "" when there was none. Names the method and pair index. */
const char *gpu_pgraph_error(const gpu_pgraph *pgraph);

/* T1246: copy the frame `src` holds (draws, indices, snapshots, vertex pools, clears, copies, queries, state) into `dst`, a model
 * made by gpu_pgraph_create that only ever serves as a clone target (its buffers are reused). `dst` is then drawn from another
 * thread while `src` decodes the next frame. False on out of memory (dst is then empty). */
bool gpu_pgraph_clone_frame(gpu_pgraph *dst, const gpu_pgraph *src);
/* T1246: every draw of the frame has its vertex bytes in the model, so drawing a clone reads no guest memory through it. */
bool gpu_pgraph_frame_self_contained(const gpu_pgraph *pgraph);
size_t gpu_pgraph_draw_count(const gpu_pgraph *pgraph);
/* T267: the clear events of the frame, in stream order (empty unless GPU_PGRAPH_OUTPUT_CLEAR is on). The frame
 * ends with gpu_pgraph_begin_frame, which forgets them with the draws. */
size_t gpu_pgraph_clear_count(const gpu_pgraph *pgraph);
const gpu_pgraph_clear *gpu_pgraph_clear_at(const gpu_pgraph *pgraph, size_t index);
const gpu_pgraph_draw *gpu_pgraph_draw_at(const gpu_pgraph *pgraph, size_t index);
/* T578: the blits of the frame in stream order (empty unless GPU_PGRAPH_OUTPUT_BLIT is on), forgotten by begin_frame like the
 * draws and the clears. The blit state words (surface offsets, format, pitches, points) persist across frames. */
size_t gpu_pgraph_copy_count(const gpu_pgraph *pgraph);
const gpu_pgraph_copy *gpu_pgraph_copy_at(const gpu_pgraph *pgraph, size_t index);
const uint32_t *gpu_pgraph_indices(const gpu_pgraph *pgraph);
size_t gpu_pgraph_snapshot_count(const gpu_pgraph *pgraph);
const gpu_pgraph_state *gpu_pgraph_snapshot(const gpu_pgraph *pgraph, size_t index);
/* The live state, after the last decoded pair. */
const gpu_pgraph_state *gpu_pgraph_state_now(const gpu_pgraph *pgraph);

/* Counters, every one counted by the decoder and not derived. */
typedef struct {
    uint64_t pairs;            /* all pairs decoded */
    uint64_t pairs_handled;    /* pairs a measured method consumed */
    uint64_t draws;            /* BEGIN..END brackets that submitted at least one vertex */
    uint64_t empty_brackets;   /* BEGIN..END with no vertex submitted (not a draw) */
    uint64_t program_dwords;
    uint64_t constant_dwords;
    uint64_t vertex_draws_captured;  /* T262: draws whose vertex bytes were snapshotted */
    uint64_t vertex_bytes_captured;  /* T262: pool bytes copied out of guest memory, all frames */
    uint64_t vertex_budget_refusals; /* T262: draws refused for passing the budget */
    uint64_t software_methods;       /* T391 fences and T1176 software control-register writes */
    uint64_t semaphore_releases;     /* T391: 0x1D70 on the 3D subchannel, a handled no-op */
    uint64_t pairs_ignored;          /* T502: pairs of the IGNORED output group (dither, specular params), decoded and skipped on purpose */
    uint64_t cxt_write_en_pairs;     /* T521: 0x1E98 pairs (value 0), the execution-mode packet's second dword, a decoded no-op */
    uint64_t fence_clear_values;     /* T391: the fence packet's two zero colour clear values, consumed with the group off */
    uint64_t sync_pairs;             /* T462: NO_OPERATION and WAIT_FOR_IDLE pairs (value 0), the SURFACE group, decoded no-ops */
    uint64_t inline_vertices;        /* T462: vertices emitted by SET_VERTEX_DATA2F_M (the write of attribute 0 emits one) */
    uint64_t inline_draws;           /* T462: draws made of them */
    uint64_t blit_state_pairs;       /* T578: subchannel 2 and 3 words decoded (binds, state, points), BLIT group */
    uint64_t copies;                 /* T578: blits the size write ran, counted in the event list (empty ones too) */
    uint64_t copies_empty;           /* T578: of them, a zero width or height (nothing moves, INFERRED from xemu) */
    uint64_t snapshots_peak;         /* T1268: most state snapshots one frame held, all frames (capacity GPU_PGRAPH_MAX_SNAPSHOTS) */
    uint64_t draws_peak;             /* T1268: most draws one frame held (capacity GPU_PGRAPH_MAX_DRAWS) */
    uint64_t indices_peak;           /* T1268: most indices one frame held (capacity GPU_PGRAPH_MAX_INDICES) */
} gpu_pgraph_stats;

gpu_pgraph_stats gpu_pgraph_get_stats(const gpu_pgraph *pgraph);

/**
 * One recorded command on a subchannel other than 0 (T391). The fence's subchannel-5 0x310 is counted
 * (`software_methods`) and takes its place in the pair count without touching any state. EVERYTHING ELSE
 * is refused, GPU_PGRAPH_ERR_UNMEASURED with a message naming the subchannel, in strict mode or not:
 * nothing measured says what such a command does, and replaying it as a 3D method would be wrong.
 * Subchannel 0 is not accepted here (use gpu_pgraph_decode). With the BLIT group (T578) subchannels 2 and 3 are decoded
 * (see GPU_PGRAPH_OUTPUT_BLIT), every other word on them is still refused.
 */
gpu_pgraph_result gpu_pgraph_decode_other_subchannel(gpu_pgraph *pgraph, uint32_t subchannel,
                                                     uint32_t method, uint32_t data);

/* Methods seen and not interpreted. Returns how many distinct methods are held and, for index <
 * that, the method and how many pairs carried it. `overflow` pairs belong to methods that did not
 * fit the table. Sorted by first appearance. */
size_t gpu_pgraph_unhandled_count(const gpu_pgraph *pgraph);
void gpu_pgraph_unhandled_at(const gpu_pgraph *pgraph, size_t index, uint32_t *method,
                             uint64_t *pairs);
uint64_t gpu_pgraph_unhandled_overflow(const gpu_pgraph *pgraph);

#endif
