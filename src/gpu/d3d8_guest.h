/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * D3D8's own state, as it lives in guest memory, and the accessors the init handlers use.
 *
 * D3D8.lib is statically linked into the title, so the device is NOT an allocation. It is a
 * static in the library's zero-filled tail (the `D3D` section's BSS, 0x003E2B88 to
 * 0x003E6454), at a fixed address, and a few of its words are read or written by the game's
 * own `.text` without any call. That is why this state is kept in guest memory at these
 * exact addresses and not in a host struct: a host copy would be invisible to the title.
 *
 * EVERY ADDRESS BELOW IS MEASURED, from the retail `default.xbe`, and the comment says by
 * which instruction. "game reads" means an instruction in `.text` (not in the D3D section)
 * reads it, found by decoding all of `.text`: of the whole D3D BSS the game reads exactly
 * three words, the dirty mask and two render-state shadow words. Everything else in this
 * file is read only by D3D8's own code, which this port replaces.
 */

#ifndef TSFP_GPU_D3D8_GUEST_H
#define TSFP_GPU_D3D8_GUEST_H

#include <stdbool.h>
#include <stdint.h>

/* --- the device, and the words that find it -------------------------------------------- */

/* The device pointer. Direct3D_CreateDevice (0x003D9230) stores the constant 0x003E3F60
 * here at 0x003D9273, and every D3D8 function loads it first (`mov esi, [0x3E3F58]`).
 * The game never touches it (zero references from `.text`). */
#define D3D8_DEVICE_POINTER_SLOT 0x003E3F58u
#define D3D8_DEVICE_BASE 0x003E3F60u

/* Device-relative offsets. The first three are also reachable as absolute addresses,
 * because the library's primitive at 0x003D6C90 addresses the cursor as `[0x3E3F60]` and
 * the limit as `[0x3E3F64]` directly. */
#define D3D8_DEV_CURSOR 0x0000u      /* pushbuffer put pointer, written by every emitter */
#define D3D8_DEV_LIMIT 0x0004u       /* roll over when the cursor reaches it */
#define D3D8_DEV_FLAGS 0x0008u       /* behaviour and mode flags, see d3d8_device.c */
#define D3D8_DEV_PB_BASE 0x0024u     /* first byte of the pushbuffer, 0x003D6375 */
#define D3D8_DEV_PB_END 0x0028u      /* one past the last byte, 0x003D638D */
#define D3D8_DEV_WRAP_COUNT 0x0040u  /* increments each time the ring wraps, 0x003D6A51 */
#define D3D8_DEV_WRAP_DELTA 0x0044u  /* cursor minus base at the last wrap, 0x003D6A5F */

/* --- globals the library keeps beside the device ---------------------------------------- */

/* SetPushBufferSize (0x003D9210) writes both. CreateDevice defaults a zero to 0x80000 and
 * 0x8000, and the roll-over helper 0x003D6B20 reads the second. */
#define D3D8_GLOBAL_PUSHBUFFER_SIZE 0x003E6404u
#define D3D8_GLOBAL_KICKOFF_SIZE 0x003E6400u

/* The cached AV capability word. 0x003DBDA5 asks the kernel once (AvSendTVEncoderOption
 * option 6) and caches the answer here, and a cached zero is asked again. */
#define D3D8_GLOBAL_AV_CAPABILITIES 0x003E3AA8u

/* The display-mode table: 12-byte rows, 184 of them and then a terminator row of
 * 0xFFFFFFFF. 0x003DBDC4 walks it with the bound 0x8AC bytes (185 rows). */
#define D3D8_MODE_TABLE 0x003E2000u
#define D3D8_MODE_TABLE_ROW_BYTES 12u
#define D3D8_MODE_TABLE_ROWS 185u

/* The deferred render-state dirty mask. THE GAME READS AND WRITES IT: 39 references from
 * `.text` (the first at 0x000211F2), every one a read-modify-write that ORs bits in, and no
 * instruction branches on the value read. CreateDevice ORs 0xFF7F7F into it at 0x003DACFD. */
#define D3D8_GLOBAL_DIRTY_MASK 0x003E3AB8u

/* Render-state shadow, one dword per state from state 0. THE GAME READS TWO OF THEM:
 * `mov esi, [0x3E3CE0]` at 0x00022589 and `mov edi, [0x3E3CE4]` at 0x000225F1, which are
 * states 8 and 9, then ANDs a mask over them and hands the result to 0x003D6C90. */
#define D3D8_GLOBAL_RS_SHADOW 0x003E3CC0u

/* --- guest memory ------------------------------------------------------------------------ */

/** Read a guest dword. Fatal (see d3d8_hle_fatal) for an address that cannot be read. */
uint32_t d3d8_guest_load32(uint32_t address);

/** Write a guest dword. Fatal for an address that cannot be written. */
void d3d8_guest_store32(uint32_t address, uint32_t value);

/** Read a guest byte. Fatal for an address that cannot be read. */
uint8_t d3d8_guest_load8(uint32_t address);

/** Write a guest byte. Fatal for an address that cannot be written. */
void d3d8_guest_store8(uint32_t address, uint8_t value);

/** Device-relative accessors: `D3D8_DEVICE_BASE + offset`. */
uint32_t d3d8_device_load32(uint32_t offset);
void d3d8_device_store32(uint32_t offset, uint32_t value);

/**
 * Call a kernel export the way the real library's `call dword ptr [thunk]` would, with
 * `argc` stdcall stack arguments, and return its eax.
 *
 * The library reaches the kernel through the import thunk table, so a port that wants the
 * same kernel state change (and the same log line and counters) has to go through the same
 * handler rather than reach into the module behind it. The frame lives in a scratch guest
 * region allocated on first use. Fatal if that cannot be allocated or `argc` is above 8.
 */
uint32_t d3d8_kernel_call(unsigned ordinal, const uint32_t *args, unsigned argc);

/**
 * A guest address with room for a few dwords, for the result pointer a kernel call is handed.
 * (The original passes a stack local.) Valid until `d3d8_guest_reset`. Fatal if memory is short.
 */
uint32_t d3d8_guest_scratch(void);

/** Forget the scratch region. For tests, after the guest allocator was reset under it. */
void d3d8_guest_reset(void);

/**
 * Observe every kernel call a handler makes through `d3d8_kernel_call`, with the arguments it
 * pushed and the value it got back. These are the calls the ORIGINAL library makes through its
 * import thunks, so the sequence is comparable with the oracle's log (tools/d3dscan/oracle.py).
 * NULL removes the tap. The measurement harness is the only user.
 */
typedef void (*d3d8_kernel_tap)(unsigned ordinal, const uint32_t *args, unsigned argc,
                                uint32_t result);
void d3d8_guest_set_kernel_tap(d3d8_kernel_tap tap);

#endif /* TSFP_GPU_D3D8_GUEST_H */
