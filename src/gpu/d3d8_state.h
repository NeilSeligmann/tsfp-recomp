/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The per-state helpers the title's inlined SetRenderState calls for states 0x88 and up, and
 * the constant and shader-mode setters `InitD3D` calls after it, as handlers.
 *
 * HOW THE TITLE REACHES THEM. The game's copy of SetRenderState (0x000211C0) is a compare
 * chain: below 0x5C it emits one header and one value through 0x003D6C90, below 0x88 it ORs a
 * dirty bit and stores a shadow, and from 0x88 it `call`s one function per state. `InitD3D`
 * runs 35 records of its own state block through it right after CreateDevice, and ten of the
 * records land here:
 *
 *   state  0x8F 0x003D7EE0   0x90 0x003D7F70   0x91 0x003D8010   0x93 0x003D7060
 *          0x94 0x003D7150   0x95 0x003D72A0   0x9A 0x003D81F0   0xA1 0x003D80B0
 *          0xA3 0x003D8190   0xA4 0x003D81B0
 *
 * WHAT EACH ONE DOES, MEASURED FROM ITS BODY. Every helper stores its argument into one D3D8
 * global (the CURRENT value of that state), then reads other globals to decide what to emit
 * into the pushbuffer and emits it. The game reads none of those globals (an exhaustive scan of
 * `.text` finds exactly three D3D8 BSS words it reads, none of them these), so the stores
 * are kept at the library's own addresses for the code that does read them, and the emission
 * is NOT reproduced: see d3d8_device.h for why the library's own command stream is deferred.
 *
 * TWO CASCADES ARE ANNOUNCED AND NOT MODELLED, each taken only when the original takes it:
 *   0x003D7EE0 into 0x003D6E50, 0x003D5C50, 0x003D7A50 and 0x003D7860 when the state enters or
 *   leaves value 2 (the title sets it to 1);
 * and `0x003D5AF0` into its twelve-constant upload when the mode has no bits beyond 0x10.
 * 0x003D81F0 into 0x003D7B80 (the render target is the first back buffer, which after
 * CreateDevice it is) is NOT one of them any more: T598 runs it, the scaled viewport and clip
 * recomputation and SetViewport(NULL) with their packets (d3d8_scaled_viewport.h, T533).
 * The two are logged once by d3d8_hle_note_unmodelled. None changes anything the game can read.
 */

#ifndef TSFP_GPU_D3D8_STATE_H
#define TSFP_GPU_D3D8_STATE_H

#include <stddef.h>
#include <stdint.h>

/* The current-value globals the helpers store into, named by the state index that stores
 * them. No D3DRS_ name is claimed: the binary carries none and no clean source settles them
 * (docs/d3d8-usage.md section 12). */
#define D3D8_STATE_90 0x003E3F00u
#define D3D8_STATE_91 0x003E3F04u
#define D3D8_STATE_93 0x003E3F0Cu
#define D3D8_STATE_94 0x003E3F10u
#define D3D8_STATE_95 0x003E3F14u
#define D3D8_STATE_9A 0x003E3F28u
#define D3D8_STATE_A1 0x003E3F44u
#define D3D8_STATE_A3 0x003E3F4Cu
#define D3D8_STATE_A4 0x003E3F50u
#define D3D8_STATE_8F 0x003E3EFCu

/** The vertex shader constant file's shadow: 16 bytes per register from register 0. */
#define D3D8_CONSTANT_SHADOW 0x003E2EA0u
#define D3D8_CONSTANT_REGISTERS 192u

/** 0x003D7060: state 0x93. */
void d3d8_state_set_93(uint32_t value);
/** 0x003D7F70: state 0x90. */
void d3d8_state_set_90(uint32_t value);
/** 0x003D8010: state 0x91. */
void d3d8_state_set_91(uint32_t value);
/** 0x003D7150: state 0x94. */
void d3d8_state_set_94(uint32_t value);
/** 0x003D80B0: state 0xA1. */
void d3d8_state_set_a1(uint32_t value);
/** 0x003D8190: state 0xA3, then the control words (0x003D7AB0). */
void d3d8_state_set_a3(uint32_t value);
/** 0x003D81B0: state 0xA4, then the control words (0x003D7AB0). */
void d3d8_state_set_a4(uint32_t value);
/** 0x003D7EE0: state 0x8F. */
void d3d8_state_set_8f(uint32_t value);
/** 0x003D81F0: state 0x9A, and at the first back buffer 0x003D7B80 (T598, d3d8_state_library_set_9a). */
void d3d8_state_set_9a(uint32_t value);
/** 0x003D72A0: state 0x95, a depth bias. Also sets render states 0x4D to 0x51 in the shadow. */
void d3d8_state_set_95(uint32_t value);

/**
 * 0x003D7AB0: recompute the two control words at device+0x2428 and device+0x242C from three
 * state globals. Bit 3 of the first follows [0x3E3F54], bit 20 of the second follows
 * [0x3E3F4C] and bit 27 follows [0x3E3F50]; the other bits are preserved or cleared as the
 * original's masks do (0xFFFFFFF7 and 0xE7EFFFFF).
 */
void d3d8_state_update_control_words(void);

/**
 * 0x003D5AF0: the shader-constant mode. Sets bit 9 (0x200) of the device flags from bit 4 of
 * `mode`, stores `mode` without bit 4 at device+0x1928, and when that leaves zero ORs 0x1600
 * into the dirty mask and emits the original 372-byte constant/matrix/fog/eye packet.
 * One entry preamble, sequential guest loads/stores, captured-device cursor commit;
 * the registered handler returns the mode without bit4 (zero on the upload path).
 */
void d3d8_state_set_constant_mode(uint32_t mode);

/**
 * 0x003D5670: store one vertex shader constant (four dwords at `source`) into the shadow at
 * register `index`. The original does not bound `index`; a value from 192 on would write past
 * the shadow, so this is fatal there instead.
 */
void d3d8_state_set_vertex_shader_constant(uint32_t index, uint32_t source);

/*
 * THE LIBRARY'S OWN HELPERS (T443). The copy composition of Swap (docs/d3d8-copy-composition.md)
 * drives them through 0x003D6CC0, and unlike the ten above, the title never calls these, so they
 * are not registered handlers. Each EMITS the commands of its original into the pushbuffer, in the
 * original's order and with its reservation preamble, and then stores its state global, so a
 * differential run against the original compares the ring byte for byte.
 *
 *   0x88 0x003D6C60 PSTextureModes   0x8B 0x003D7380 FillMode      0x8C 0x003D73D0 BackFillMode
 *   0x96 0x003D7320 LogicOp          0x97 0x003D6F90 EdgeAntiAlias 0x99 0x003D82D0 MultiSampleMask
 *   0x9B 0x003D8220 MultiSampleRenderTargetMode                    0xA0 0x003D8080 YuvEnable
 */
#define D3D8_STATE_88 0x003E3EE0u
#define D3D8_STATE_92 0x003E3F08u
#define D3D8_STATE_A2 0x003E3F48u
#define D3D8_STATE_8B 0x003E3EECu
#define D3D8_STATE_8C 0x003E3EF0u
#define D3D8_STATE_8D 0x003E3EF4u
#define D3D8_STATE_96 0x003E3F18u
#define D3D8_STATE_97 0x003E3F1Cu
#define D3D8_STATE_98 0x003E3F20u
#define D3D8_STATE_99 0x003E3F24u
#define D3D8_STATE_9B 0x003E3F2Cu
#define D3D8_STATE_9E 0x003E3F38u
#define D3D8_STATE_A0 0x003E3F40u

void d3d8_state_set_88(uint32_t value);
void d3d8_state_set_8b(uint32_t value);
void d3d8_state_set_8c(uint32_t value);
void d3d8_state_set_96(uint32_t value);
void d3d8_state_set_97(uint32_t value);
void d3d8_state_set_99(uint32_t value);
/** 0x003D8220. A render target other than the first back buffer runs 0x003D7B80 (the scaled
 * viewport and clip recomputation) and SetViewport(NULL), ported by T533 (d3d8_scaled_viewport.h). */
void d3d8_state_set_9b(uint32_t value);
void d3d8_state_set_a0(uint32_t value);

/*
 * The EMITTING variants of four helpers the title also reaches through the stores-only ports above
 * (d3d8_state_set_90, 93, 94, 95). The title's own calls keep their documented deferred-emission
 * behaviour, so these exist for the library's own path (0x003D6CC0, T443) and write what the
 * original writes. 0x94 only writes while no pixel shader is bound.
 */
void d3d8_state_library_set_90(uint32_t value);
void d3d8_state_library_set_93(uint32_t value);
void d3d8_state_library_set_94(uint32_t value);
void d3d8_state_library_set_95(uint32_t value);

/*
 * T461: the emitting library variants of the six helpers the title's own ports store without
 * emitting (0x8F 0x91 0x9A 0xA1 0xA3 0xA4), driven by the library's own SetRenderState. T533: 0x9A with
 * the render target at the first back buffer (and 0x9B away from it) runs 0x003D7B80 and
 * SetViewport(NULL) (d3d8_scaled_viewport.h), the one piece SetRenderTarget shares. T598: the title's own
 * 0x003D81F0 (d3d8_state_set_9a) calls the same function.
 */
void d3d8_state_library_set_8f(uint32_t value);
void d3d8_state_library_set_91(uint32_t value);
void d3d8_state_library_set_9a(uint32_t value);
void d3d8_state_library_set_a1(uint32_t value);
void d3d8_state_library_set_a3(uint32_t value);
void d3d8_state_library_set_a4(uint32_t value);
/*
 * The rest of the library's helper table, 0x89 0x8A 0x8D 0x8E 0x92 0x98 0x9C 0x9D 0x9E 0x9F 0xA2 0xA5. With
 * them every state from 0x88 to 0xA5 has an emitting port. 0x98 refuses a programmable declaration (the
 * vertex program helper 0x003D5C50) before any write, as 0x8F does.
 */
void d3d8_state_library_set_89(uint32_t value);
void d3d8_state_library_set_8a(uint32_t value);
void d3d8_state_library_set_8d(uint32_t value);
void d3d8_state_library_set_8e(uint32_t value);
void d3d8_state_library_set_92(uint32_t value);
void d3d8_state_library_set_98(uint32_t value);
void d3d8_state_library_set_9c(uint32_t value);
void d3d8_state_library_set_9d(uint32_t value);
void d3d8_state_library_set_9e(uint32_t value);
void d3d8_state_library_set_9f(uint32_t value);
void d3d8_state_library_set_a2(uint32_t value);
void d3d8_state_library_set_a5(uint32_t value);

/** 0x003D7A50: the 0x40290 packet (yuv enable, state 0x8F value 2, depth format). Returns the
 * cursor after it, as the original returns it in eax. */
uint32_t d3d8_state_emit_surface_control(uint32_t cursor);

/** Register this file's handlers; returns how many. */
size_t d3d8_state_register(void);

#endif /* TSFP_GPU_D3D8_STATE_H */
