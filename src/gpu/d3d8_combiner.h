/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The fixed-function pixel pipeline selection of the draw dirty cascade (T443): 0x003E11D0, which
 * 0x003DED80 runs when the dirty mask has bit 0x800 and the device has no pixel shader bound
 * (device+0x784 is zero). With a shader bound it returns the incoming mask and writes nothing, which
 * the draw cascade already models (docs/d3d8-draw-cascade.md).
 *
 * WHAT IT DOES. It walks the texture stages from stage 0 (or stage 3 while render state 0x76 is set)
 * until a stage whose colour operation (texture state 0xC) is 1, DISABLE, or stage 4. Each stage runs
 * twice through a 26 way jump table (0x003E1728) indexed by the operation minus one, once for colour
 * from states 0xC..0xF and once for alpha from states 0x10..0x13, and 0x003E1130 turns an argument
 * selector into a combiner input word. The words go into four eight slot packets, the colour input
 * words (header 0x200AC0), colour output words (0x201E40), alpha input words (0x200260) and alpha output
 * words (0x200AA0), unused slots zero, and one reservation writes the stage count (0x41E60), the four
 * packets and, when device flag 0x40 changed against the entry value and render state 0x67 is zero, the
 * 0x403B8 pair. The function also clears device flags 0x10040 at entry, argument selector 4 sets 0x40
 * and operations 25 and 26 set 0x10000, and a final 0x10000 sets 0x400F in the returned dirty mask.
 *
 * THE PORT is a transliteration of the 450 instructions with the original's registers and stack slots as
 * locals and its jumps as gotos, verified against the original over every operation and selector
 * (tests/test_d3d8_combiner_oracle.py). A texture state outside the table (operation 0 or above 26, an
 * argument selector from 6 up) sends the original through a garbage jump, so the port refuses it.
 */

#ifndef TSFP_GPU_D3D8_COMBINER_H
#define TSFP_GPU_D3D8_COMBINER_H

#include <stdint.h>

#include "d3d8_pushbuffer.h"

/** 0x003E11D0 with the pixel shader unbound: emit and return the dirty mask the cascade continues
 * with. Refuses before any write. */
uint32_t d3d8_emit_fixed_function_combiner(uint32_t dirty);

/** Read-only twin: refuse what the emitter would refuse, advance the simulated writer through the
 * emitter's reservation and return the mask the cascade continues with. */
uint32_t d3d8_plan_fixed_function_combiner(uint32_t dirty, d3d8_pushbuffer_sim *sim);

#endif /* TSFP_GPU_D3D8_COMBINER_H */
