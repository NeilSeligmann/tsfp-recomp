/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The library's out-of-line SetRenderState and SetTextureStageState (T443), the two dispatchers
 * the copy composition of Swap drives (docs/d3d8-copy-composition.md). The title's own states go
 * through its inlined copy of the first (d3d8_render_state.h), these are the library's entry points
 * the SetPixelShader, swap and shader code call for state they save and restore.
 *
 *   0x003D6CC0 SetRenderStateNotInline(state, value)
 *     below 0x5C   emit the header from the table at 0x475B08 and the value (0x003D6C90, which rolls
 *                  the ring over and retries when it reaches the limit), store the shadow at
 *                  0x3E3CC0 + state * 4
 *     below 0x88   OR the dirty bits of the table at 0x4758E8 into 0x3E3AB8, store the shadow
 *     from  0x88   call the per-state helper of the table at 0x3E1948. Only the helpers that are
 *                  ported (d3d8_state.h) are reachable, any other is a named refusal, and so is a
 *                  negative state (the original's signed compare would index off the table)
 *
 *   0x003D7700 SetTextureStageStateNotInline(stage, state, value)
 *     below 0xC    OR bit `stage` into the dirty mask, store the value at 0x3E3AC0 + (stage*32+state)*4
 *     0xC          the colour operation: dirty 0x800 when the value is below 0x19, else 0x480F, and the
 *                  value goes to the same array
 *     below 0x16   OR the dirty bits of the table at 0x475A00, store
 *     0x1C         TexCoordIndex (0x003D7500): emits the transform and wrap packets
 *     0x16..0x1B 0x1D 0x1E  the BumpEnv, BorderColor and ColorKeyColor helpers are NOT ported, refused
 *     above 0x1E   nothing (the original returns)
 *   A stage of 4 or more would index past the four stage arrays, and is refused.
 */

#ifndef TSFP_GPU_D3D8_SET_STATE_H
#define TSFP_GPU_D3D8_SET_STATE_H

#include <stdint.h>

#define D3D8_RS_HEADER_TABLE 0x00475B08u /* below 0x5C: the NV2A header of each immediate state */
#define D3D8_RS_DIRTY_TABLE 0x004758E8u  /* from 0x5C: the dirty bits OR'd for a deferred state */
#define D3D8_RS_HELPER_TABLE 0x003E1948u /* from 0x88: the per-state helper address */
#define D3D8_TS_DIRTY_TABLE 0x00475A00u  /* TSS states 0xD to 0x15: the dirty bits */
#define D3D8_TS_SHADOW 0x003E3AC0u       /* stage 0 state 0, 32 dwords per stage */
#define D3D8_TS_STAGE_DWORDS 32u
#define D3D8_TS_STAGE_COUNT 4u
#define D3D8_TS_COLOR_OP 0xCu
#define D3D8_TS_TEXCOORD_INDEX 0x1Cu
#define D3D8_TEXCOORD_MAP 0x005496A9u    /* one byte per stage, the vertex attribute the stage reads */

/** 0x003D6CC0. */
void d3d8_set_render_state_notinline(int32_t state, uint32_t value);
/** 0x003D7700. */
void d3d8_set_texture_stage_state_notinline(uint32_t stage, int32_t state, uint32_t value);
/** 0x003D7500. */
void d3d8_set_texcoord_index(uint32_t stage, uint32_t value);

#endif /* TSFP_GPU_D3D8_SET_STATE_H */
