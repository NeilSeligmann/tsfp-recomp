/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Binding resources to the device: SetRenderTarget (0x003D3800), SetTexture (0x003D4070), the
 * reference counting around them and the part of 0x003D7220 the render target runs.
 *
 * WHAT IS GUEST-VISIBLE HERE, AND REPRODUCED. A resource header's first dword packs two counts: the
 * reference count in bits 0-15 and a COUNT OF DEVICE BINDINGS in bits 19-22 (0x80000 per binding,
 * which a render target and a texture stage both add). The third dword, Lock, is stamped with the
 * device's fence counter (`device+0x2C`) whenever a binding is dropped, because the GPU may still be
 * reading the surface until that fence passes. The title copies five dwords of a header
 * (Common, Data, Lock, Format, Size) into its own structure, so these words reach it. The device's
 * bindings (`+0x1A04` target, `+0x1A08` depth, `+0xF88 + 4 * stage` textures) are what GetRenderTarget2
 * and the Swap path read back.
 *
 * WHAT IS REPRODUCED SINCE T440. The words SetRenderTarget's recompute (0x003D7B80) and the viewport
 * reset (0x003D3E40) leave, for the one mode this port creates (no multisampling): the target size at
 * +0x954 and +0x958, scale 1.0 at +0x95C, +0x960 and +0x964, the whole target as the viewport at +0xEE0
 * to +0xEEC and the depth range 0 to 1 at +0xEF0 and +0xEF4. Clear, the viewport emitter and the scissor
 * read them. MEASURED against the original for linear and swizzled targets of several sizes. Other
 * modes are refused.
 *
 * WHAT IS NOT REPRODUCED, AND ANNOUNCED. The commands both functions emit, and for SetRenderTarget
 * the viewport matrix at device+0x980 (0x003D6E50, ported in d3d8_viewport_matrix.c and not called
 * here). The dirty mask (the one guest-visible global among them) is OR-ed by the parts ported here and
 * by none of the skipped ones, so after SetRenderTarget it can lack bits the original sets (0x10F and
 * 0x100).
 */

#ifndef TSFP_GPU_D3D8_BIND_H
#define TSFP_GPU_D3D8_BIND_H

#include <stddef.h>
#include <stdint.h>

/** 0x003D3800. A null `target` keeps the current one. `depth` may be 0. */
void d3d8_set_render_target(uint32_t target, uint32_t depth);

/** 0x003D4070. A null `texture` unbinds the stage. Fatal for a stage past 3. */
void d3d8_set_texture(uint32_t stage, uint32_t texture);

/** 0x003D7220: the multisample-dependent bit of the device flags and the global at 0x003E3F3C. */
void d3d8_set_render_target_flag(uint32_t value);

/** 0x003D4C90, D3DResource_Release: drop a reference. Returns the new count. */
uint32_t d3d8_resource_release(uint32_t header);

/** 0x003D4DA0: drop a device binding (0x80000). */
void d3d8_resource_release_binding(uint32_t header);

/** Register this file's handlers; returns how many. */
size_t d3d8_bind_register(void);

#endif /* TSFP_GPU_D3D8_BIND_H */
