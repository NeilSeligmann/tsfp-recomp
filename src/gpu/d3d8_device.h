/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The D3D8 device bring-up the title performs in `InitD3D` (0x00023930), as handlers for the
 * library addresses it calls, in the order it calls them.
 *
 *   0x003D9000  Direct3DCreate8        eax = 1, nothing else (8 bytes: `mov eax,1; ret 4`)
 *   0x003D9010  GetAdapterModeCount    d3d8_display.c
 *   0x003D90B0  EnumAdapterModes       d3d8_display.c
 *   0x003D9210  SetPushBufferSize      two stores into the library's BSS
 *   0x003D9230  Direct3D_CreateDevice  this file, the large one
 *   0x003D6C90  the pushbuffer primitive  d3d8_pushbuffer.c
 *   0x003D3A80  GetBackBuffer2         this file
 *
 * WHAT CREATEDEVICE REPRODUCES, AND WHAT IT DELIBERATELY DOES NOT. The original is the whole
 * NV2A bring-up: it maps registers, builds DMA objects in instance memory, wires a vertical
 * blank interrupt, programs the display and writes 678 dwords of initial state into
 * the pushbuffer. Run as the original machine code under an x86 emulator against a model of
 * the kernel (tools/d3dscan/oracle.py), it changes 687 dwords of D3D8's own state. This port
 * reproduces the part the TITLE CAN OBSERVE or that a later handler reads, and the document
 * `docs/d3d8-usage.md` section 13 lists the rest by region with the instruction that wrote it
 * and the reader (if any) that would notice:
 *
 *   - the return value and the out-pointer (the title never reads the out-pointer: its only
 *     reference to the destination, 0x00563030, is the address pushed at 0x00023B2B);
 *   - the device pointer slot and the library globals the other handlers key off;
 *   - the dirty mask, which the game OR-s bits into (every reference a read-modify-write);
 *   - the pushbuffer ring, because BeginPush hands the title a pointer into it;
 *   - the three surface headers (back, front, depth) and their sizes, because the title
 *     copies five dwords of each into its own texture header;
 *   - the display-mode choice, because a request no row serves fails the call.
 *
 * PARTIALLY REPRODUCED: measured CreateDevice packets for the initial render target and the two
 * default FILL render states 0x8B/0x8C. Most other commands the library writes into the pushbuffer,
 * NV2A
 * objects, the vertical-blank interrupt, gamma ramps, and the default values of the
 * other render-state, sampler and constant shadows. The game reads only three words of D3D8's BSS.
 */

#ifndef TSFP_GPU_D3D8_DEVICE_H
#define TSFP_GPU_D3D8_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The two HRESULTs CreateDevice can return that this port produces. */
#define D3D8_E_OUTOFMEMORY 0x8007000Eu

/** Where the NV2A register window is. MEASURED: a literal in the library, `mov [ecx], 0xFD000000`
 * at 0x003DCC51, stored as the display object's register base. AvSendTVEncoderOption is later
 * handed it as `RegisterBase`. */
#define D3D8_NV2A_REGISTER_BASE 0xFD000000u

/**
 * 0x003D9210: store the pushbuffer size and kickoff size. Returns the first argument, as the
 * original does (it leaves it in eax).
 */
uint32_t d3d8_set_push_buffer_size(uint32_t size, uint32_t kickoff);

/**
 * 0x003D9230: create the device.
 *
 * `present_parameters` is a guest pointer to the 17-dword D3DPRESENT_PARAMETERS. Only the
 * configuration the title uses is ported, and any other is fatal: one back buffer, no
 * multisampling (0 or 0x11), swap effect 3, no caller-supplied surfaces, linear formats.
 * `out_device` may be 0.
 *
 * Returns 0 and stores the device address through `out_device`, or an HRESULT: E_FAIL
 * when no display mode serves the request (the device state is then cleared as the original
 * clears it), E_OUTOFMEMORY when the pushbuffer or a surface cannot be allocated.
 */
uint32_t d3d8_create_device(uint32_t behavior_flags, uint32_t present_parameters,
                            uint32_t out_device);

/**
 * 0x003D3580 and 0x003D35D0: the flicker filter and the soft display filter, which CreateDevice
 * ends by setting to 5 and 0. Each remembers what it last sent (in D3D8 globals, 0x003E2B98 and
 * 0x003E2BA0 for the flicker filter, 0x003E2B94 and 0x003E2B9C for the soft filter, the second
 * stored as a boolean) and calls AvSendTVEncoderOption (option 0xB, option 0xE) only when the
 * value changes or nothing was sent yet, with the display object's register base as the first
 * argument. Not in the measured surface: nothing in the title's `.text` calls them, so they have
 * no handler of their own.
 */
void d3d8_set_flicker_filter(uint32_t value);
void d3d8_set_soft_display_filter(uint32_t value);

/**
 * 0x003D6660 BeginPush(Count), stdcall, ret 4 (T854): flush the deferred state like DrawVertices does
 * (0x003DEE00 with its argument zero: the dirty cascade and the stream work, ported in d3d8_resource.c), then reserve
 * Count + 1 dwords (0x003D6B30, a refill when the limit is near) and return the cursor, a raw pointer into the ring the
 * title writes its block through. Nothing is published until EndPush. Every refusal of the flush comes before the first
 * write; a reservation the ring cannot hold, or one over unmapped memory, is refused by name.
 */
uint32_t d3d8_begin_push(uint32_t count);

/**
 * 0x003D6680 EndPush(p), stdcall, ret 4 (T854): store p as the cursor (device+0) and return it. The original stores
 * whatever it is given. The port refuses a pointer that is not a dword position inside the block the last BeginPush
 * reserved (or with no block open).
 */
uint32_t d3d8_end_push(uint32_t pointer);

/**
 * 0x003D3A80: the surface header for a back buffer, with a reference added. -1 selects the
 * front buffer slot, 0 the first back buffer, and any other value the third slot, exactly
 * as the original's `neg / sbb / and 2` indexes it. Fatal when the slot is empty.
 */
uint32_t d3d8_get_back_buffer(int32_t index);

/**
 * Register every handler above with the D3D8 dispatcher. Call after the surface is adopted
 * (`d3d8_surface_adopt` re-initialises the table and drops handlers). Returns how many
 * were registered. An address absent from the adopted surface is skipped, not an error: a
 * different build of the title has different addresses.
 */
size_t d3d8_device_register(void);

#endif /* TSFP_GPU_D3D8_DEVICE_H */
