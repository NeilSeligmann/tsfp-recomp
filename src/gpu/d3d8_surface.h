/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * D3D8 surface headers and the format words in them, ported from the library's machine code.
 *
 * THE TITLE READS THESE WORDS. `InitD3D` asks `GetBackBuffer2` (0x003D3A80) for the front and
 * back buffers and copies five dwords of each header (Common, Data, Lock, Format, Size) into
 * its own larger texture header, which it then edits and uses to render to a texture. So the
 * values are not private to the library: a wrong Format or Size word reaches the title.
 *
 * LAYOUT, MEASURED. A surface header is six dwords: Common (reference count in the low 16
 * bits, resource type in bits 16-18, render-target references in bits 19-22), Data (the
 * PHYSICAL address of the pixels, masked to 28 bits), Lock, Format, Size and Parent. The
 * Format word is the NV2A texture format assembled by 0x003DBB70, and the "Size" word packs
 * `(pitch / 64 - 1) << 24 | (height - 1) << 12 | (width - 1)` for a linear surface.
 *
 * ONLY THE LINEAR CASE IS PORTED. Swizzled and compressed formats take another path in
 * 0x003DBB70 that device creation never uses, so a request for one is fatal rather than
 * answered with a guess. The suite pins the linear arithmetic against values the original
 * produced under the emulated oracle (640x480 A8R8G8B8: Format 0x00011229, Size 0x271DF27F).
 */

#ifndef TSFP_GPU_D3D8_SURFACE_H
#define TSFP_GPU_D3D8_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

/* Header field offsets. */
#define D3D8_SURFACE_COMMON 0x00u
#define D3D8_SURFACE_DATA 0x04u
#define D3D8_SURFACE_LOCK 0x08u
#define D3D8_SURFACE_FORMAT 0x0Cu
#define D3D8_SURFACE_SIZE 0x10u
#define D3D8_SURFACE_PARENT 0x14u
#define D3D8_SURFACE_HEADER_BYTES 0x18u

/** 0x003DB3B0: the public D3DFORMAT as the hardware format number. Unlisted values pass
 * through unchanged. MEASURED from the jump table: 2 to 0x10, 3 to 0x1C, 5 to 0x11, 6 to
 * 0x12, 7 to 0x1E, 0x2A to 0x2E, 0x2B to 0x2F, 0x2C to 0x30, 0x2D to 0x31. */
uint32_t d3d8_surface_normalise_format(uint32_t format);

/** 0x003DB4C0: the pitch the hardware will accept for `width` pixels of `format`. The row
 * bytes, rounded up to 64, then the smallest entry of the legal-pitch table at 0x003E17C0
 * that is not smaller. Returns the rounded row bytes unchanged when none is large enough. */
uint32_t d3d8_surface_pitch_for_width(uint32_t width, uint32_t format);

/** The pitch 0x003DBB70 computes itself when it is handed a pitch of 0: the row bytes
 * rounded up to 64, with no reference to the legal-pitch table. */
uint32_t d3d8_surface_aligned_row_bytes(uint32_t width, uint32_t format);

/**
 * 0x003DBB70, linear path, for a single-level 2-D surface: assembles the Format and Size
 * words and returns the byte size (height * pitch). `pitch` must be non-zero. Fatal for a
 * format the original sends down the swizzled or compressed path.
 */
uint32_t d3d8_surface_linear_words(uint32_t width, uint32_t height, uint32_t format,
                                   uint32_t pitch, uint32_t *format_word, uint32_t *size_word);

/** 0x003D4A70: initialise a header. Common 0x01050001, Data is `data` masked to 28 bits,
 * Lock and Parent zero. The original is handed the contiguous allocation's virtual address
 * and keeps its low 28 bits, which on a console IS the physical address. Here the caller
 * passes the physical address the kernel module reports for the allocation, so Data keeps
 * its meaning (the number the hardware would be given) rather than its derivation. */
void d3d8_surface_init_header(uint32_t header, uint32_t format_word, uint32_t size_word,
                              uint32_t data);

/** 0x003D3640: the pitch of an existing surface header, from its Size word when it has one
 * and from its Format word otherwise. */
uint32_t d3d8_surface_header_pitch(uint32_t header);

/**
 * The pixel size of a surface header, or false when it cannot be told. A header with a Size word
 * is linear: width - 1 in bits 0-11 and height - 1 in bits 12-23 (MEASURED, the decode
 * d3d8_frame_record uses and the suite pins against 640x480). Without one it is swizzled and the
 * Format word carries log2 width in bits 20-23 (MEASURED, d3d8_surface_header_pitch) and log2 height
 * in bits 24-27 (INFERRED from 0x003DB51B, which forwards exactly those bits as the height exponent
 * of the surface-format word). Both exponents zero is a 1x1 or an unset header: false.
 */
bool d3d8_surface_dimensions(uint32_t header, uint32_t *width, uint32_t *height);

/** 0x003D4C50: add a reference, and one to the parent first when this is a surface whose own
 * count is zero. Returns the new count. */
uint32_t d3d8_resource_add_ref(uint32_t header);

/** 0x003D4C10: a render-target reference (0x80000 in Common), and one on the parent first
 * when no render-target reference exists yet and this is a surface with a parent. */
void d3d8_resource_add_render_target_ref(uint32_t header);

/**
 * 0x003DB500: the NV2A surface-format word for a render target and an optional depth buffer
 * (what `SET_SURFACE_FORMAT` carries), stored at device+0x1A0C by SetRenderTarget. The colour
 * format picks a class (a base value, plus 0x100 for the linear ones, and for the swizzled
 * ones the width and height exponents of the Format word), and the depth format then adds
 * 0x20 or 0x10. Fatal for a colour format the original has no entry for. `depth_header` may
 * be 0.
 */
uint32_t d3d8_surface_format_word(uint32_t render_target_header, uint32_t depth_header);

/** 0x003DB620: the largest depth value as a float, by the depth format's hardware number
 * 0x2E..0x31 (and the same four as 0x2A..0x2D). Fatal for any other format. */
uint32_t d3d8_surface_max_depth_bits(uint32_t format);

#endif /* TSFP_GPU_D3D8_SURFACE_H */
