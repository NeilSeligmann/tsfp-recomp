/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The 4x4 inverse of the lit and blended matrices of 0x003DEB80 (T533): 0x003D9DB0 and the square root helper
 * 0x003D9D00 it calls.
 *
 * WHAT IT COMPUTES. The sixteen cofactors of the matrix (the transposed adjugate, in the order of the original's
 * stores), in x87 EXTENDED precision with a float store at each place the original has an `fstp dword`, then the
 * determinant (a float store too). A zero determinant returns -1 without writing anything. Otherwise the output
 * is the cofactors scaled by `sign(det) * rsqrt(det^2)` (0x003D9D00, one Newton step of an integer-seeded
 * estimate, so about 1/|det| with the sign of det) when the flag is non-zero, or only the cofactors with their
 * sign flipped by the determinant's sign (no scale) when it is zero. The original's flag is `[0x003E3EF8] == 0`.
 *
 * HOW IT IS PORTED. The cofactor sequence (0x003D9DB7 to 0x003DA0FB, 267 instructions) is the original's own
 * instructions as inline asm, one line per instruction with its address, over a frame in host memory standing for
 * the original's stack frame (rbx for esp). The generated text assembles byte for byte to the original's bytes
 * (checked when it was generated, see docs/d3d8-copy-composition.md section T533). The rest is C over long
 * double. Needs x86 and the x87 control word the other matrix ports need (extended precision, round to nearest,
 * masked exceptions).
 *
 * REFUSED BY NAME, before any write: a NaN word in the matrix (the physical x87 quiets a signalling NaN, the
 * oracle's emulator does not), a NaN determinant or output (an infinity met an infinity: payload handling), and,
 * for the emitter that calls it, a zero determinant (the original then copies 48 bytes of uninitialised stack).
 */

#ifndef TSFP_GPU_D3D8_MATRIX_INVERSE_H
#define TSFP_GPU_D3D8_MATRIX_INVERSE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * The pure core: `in` the 16 words of the matrix, `normalize` the original's flag argument, `out` the 16 words the
 * original would store. False (and `out` untouched) when the determinant is zero. Fatal at 0x003D9DB0 on a NaN
 * word of `in`, a NaN determinant or a NaN output. Reads the float constants of the original from guest memory
 * (0x475CAC zero, 0x475CD4, 0x475D18, 0x549720, 0x549724) and refuses a NaN among them.
 */
bool d3d8_matrix_inverse_compute(const uint32_t in[16], bool normalize, uint32_t out[16]);

/** 0x003D9DB0, stdcall (out, in, flag), ret 12. Returns 0, or 0xFFFFFFFF with `out` untouched (singular). */
uint32_t d3d8_matrix_inverse(uint32_t out, uint32_t in, uint32_t flag);

#endif /* TSFP_GPU_D3D8_MATRIX_INVERSE_H */
