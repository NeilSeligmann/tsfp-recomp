/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * D3D8's display-mode enumeration, ported from the library's own machine code.
 *
 * WHAT THE TITLE SEES. `InitD3D` (0x00023930) asks `Direct3D_GetAdapterModeCount`
 * (0x003D9010) how many modes there are and calls `EnumAdapterModes` (0x003D90B0) once per
 * mode, discarding the results. The count is therefore the one number that decides how many
 * more XDK dispatches the boot makes here: 36 under the default `--av-pack hdtv`.
 *
 * WHERE THE ANSWER COMES FROM. Not from the console. The library asks the kernel for a
 * capability word (AvSendTVEncoderOption option 6, which this host FABRICATES and announces,
 * see docs/av-policy.md), then filters a 185-row table in its own `.data` at 0x003E2000
 * against that word. This port does the same filtering over the same guest-memory table, so
 * the numbers come from the title's own data and the operator's `--av-pack` choice, and
 * nothing here invents a mode.
 *
 * MEASURED. Each function below was read from the retail image and, separately, run as the
 * original machine code under an x86 emulator against the same table (tools/d3dscan/
 * oracle.py): under the default capability word 0x00480104 the original code counts 36 modes
 * and this port counts 36, and the suite pins the mode list row by row.
 */

#ifndef TSFP_GPU_D3D8_DISPLAY_H
#define TSFP_GPU_D3D8_DISPLAY_H

#include <stdint.h>

/* The HRESULT 0x003D90B0 returns for a mode index past the end. D3DERR_INVALIDCALL. */
#define D3D8_E_INVALIDCALL 0x8876086Cu

/**
 * 0x003DBDA5: the AV capability word, cached at 0x003E3AA8.
 *
 * Asks the kernel (ordinal 2, option 6, result pointer 0x003E3AA8) only while the cache is
 * zero, exactly as the original does, so a zero answer is asked again on every call.
 */
uint32_t d3d8_display_capabilities(void);

/**
 * 0x003DBDC4: the guest address of the first row of the display-mode block that matches the
 * capability word. Finds the first row of the requested video standard (bits 8-15), then the
 * first row after it whose pack (low byte) is the requested pack or is 0, the fallback block.
 * A standard with no rows lands on the terminator row.
 */
uint32_t d3d8_display_mode_block(void);

/**
 * 0x003DBD65: a table row's flags word as the D3DPRESENTFLAG bits the mode reports.
 * Widescreen 0x10, then interlaced 0x20 or field 0xA0 or progressive 0x40, then the 10:11
 * pixel-aspect bit 0x100.
 */
uint32_t d3d8_display_present_flags(uint32_t row_flags);

/**
 * 0x003D9010 Direct3D_GetAdapterModeCount: four formats per accepted row of the block.
 * The adapter argument is ignored by the original and by this.
 */
uint32_t d3d8_adapter_mode_count(void);

/**
 * 0x003D90B0 Direct3D_EnumAdapterModes. Writes five dwords at `out_address`: width, height,
 * refresh rate (60 if the row carries the 60 Hz class bit 0x400000, else 50), the present
 * flags, and the format (0x1E, 0x11, 0x1C or 0x12 for `mode_index & 3`). Returns 0, or
 * D3D8_E_INVALIDCALL when `mode_index >> 2` is past the accepted rows, writing nothing.
 */
uint32_t d3d8_adapter_enum_mode(uint32_t mode_index, uint32_t out_address);

/* The HRESULT a request that matches no row fails with. E_FAIL. */
#define D3D8_E_FAIL 0x80004005u

/* The seven arguments of 0x003DBE3D, in its order. */
typedef struct {
    uint32_t width;
    uint32_t height;
    /* 60 or 50 selects the refresh class the row must carry, any other value leaves the
     * capability word's own class in force. */
    uint32_t refresh;
    /* The presentation flags: 0x10 widescreen, 0x20 interlaced, 0x40 progressive, 0x80 field,
     * 0x100 10:11 pixel aspect, 0x200 force the other refresh class. */
    uint32_t flags;
    uint32_t format;
    uint32_t interval;
    /* The back buffer pitch, stored in the display object. */
    uint32_t pitch;
} d3d8_mode_request;

/**
 * 0x003DBE3D: pick the display-mode row that serves a presentation request, or fail.
 *
 * Walks the block `d3d8_display_mode_block` found for a row whose size, widescreen, field and
 * pixel-aspect bits agree with the request, whose refresh class and scan type fit, and which
 * the capability word admits. THE FIRST ROW THAT AGREES ON SIZE AND NOTHING ELSE can still
 * fail the whole call, as in the original: once a row matches geometry and flags, a zero
 * mode word or an unmet widescreen request returns E_FAIL instead of trying later rows.
 *
 * On success stores into the display object at `hw_object` (device+0x1C28): +0x0C the
 * normalised format, +0x04 the pitch, +0x08 the row's mode word (0 when the capability
 * pack is 0), +0x1B4 the row's flags and +0x1B8 1, and sets the entry at
 * `hw_object + 0x7DC + 4 * [hw_object + 0x7E4]` to 1. Returns 0, or D3D8_E_FAIL with
 * nothing stored.
 *
 * NOT MODELLED, and fatal if reached: the branch that reprograms the CRTC timing registers
 * when the request is for 50 or 60 Hz AND flag 0x200 is set AND the capability word carries
 * the other refresh class (PAL60 and the like). The title never sets 0x200.
 */
uint32_t d3d8_display_match_mode(uint32_t hw_object, const d3d8_mode_request *request);

#endif /* TSFP_GPU_D3D8_DISPLAY_H */
