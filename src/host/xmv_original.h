/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_XMV_ORIGINAL_H
#define TSFP_HOST_XMV_ORIGINAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T92/T234: the retained original XMV library, opt in (--native-xmv). The seven exports
 * the title calls are replaced in the normal lift by measured trampolines. This module
 * routes exactly those seven addresses to separately translated bodies of the original
 * bytes, so the title's own decoder runs on the user's own disc data. Everything the
 * bodies call (XAPI file I/O, DSOUND, D3D8) still goes through the normal seams and
 * stops where the host has nothing honest to answer.
 *
 * Capability comes from trusted compiled generation (tools.gen_xmv_original_bodies),
 * never from guest pointers. A missing chunk fails closed. Default off. Setup and
 * teardown are quiescent, changes while an original executes are refused. */
#define XMV_ORIGINAL_EXPORT_COUNT 7u

/* True when the compiled chunk has the exact profile identity and all seven bodies. */
bool xmv_original_ready(void);

/* Default disabled. Failure keeps the previous configuration. */
bool xmv_original_configure(bool enabled);

/* One line per export call and return on stderr (entry arguments and EAX), no state
 * change. Default off. */
void xmv_original_set_trace(bool enabled);
/* FABRICATED startup-only termination request; requires configured original exports. */
bool xmv_original_set_skip_intro(bool enabled);

/* T394: with the trace on, every GetNextFrame that reports a frame (result 1) writes the decoder's
 * macroblock aligned Y, U and V planes to "<dir>/frame_NNNNN.yuv" (a debugging aid for comparing
 * against an independent decoder, the files are disc data and never belong in the repository).
 * NULL turns it off, `maximum` (0 is unlimited) caps the files written while the hash lines go on. The directory is not created, a failed write is reported on stderr. */
void xmv_original_set_frame_dump(const char *directory, uint32_t maximum);

/* T538: the codec wrapper whose entry state the capture writes, and the cooperative call hook that
 * feeds it. `ranges` are inclusive 0 based entry numbers counted over the run (`count` of them, at
 * most 32, ascending). Every selected entry writes "<dir>/entry_NNNNN.bin" (format TS538A01, read by
 * tools/diagnostics/replay_xmv_capture.py): the entry registers, MMX, XMM and virtual x87 state and the
 * complete host mappings that hold the image, the stack and everything the decoder object points at.
 * A capture that exceeds its byte bound or cannot be written is reported on stderr and skipped, the
 * run goes on. The files are disc data and never belong in the repository. NULL turns it off. */
#define XMV_CAPTURE_ENTRY 0x00447E5Eu
#define XMV_CAPTURE_RANGE_MAX 32u
bool xmv_original_set_entry_capture(const char *directory, const uint32_t *first, const uint32_t *last,
                                    size_t count);
/* Called from the cooperative safepoint before every guest callee on the calling guest thread. */
void xmv_original_note_call(uint32_t callee);

/* T611: the seeded run. At wrapper entry number `entry` (0 based, counted over the run, after any capture of the
 * same entry) the host applies the `patches` (xmv_seed_patch.h grammar, at most XMV_SEED_PATCH_MAX), runs the
 * lifted wrapper itself, writes the pages it changed to `result` and exits the process. Returns false for a
 * malformed patch or missing result path. `enabled` false clears it. */
#define XMV_SEED_PATCH_MAX 32u
bool xmv_original_set_seed(bool enabled, uint32_t entry, const char *result, const char *const *patches,
                           size_t count);

/* T394: how the frame dump finds the converted picture. A surface header's Data word is not a guest
 * address, the resolver (the D3D8 resource registry) gives the guest virtual address of the registered
 * allocation with that Data word, or 0. Without one the dump writes the decoder planes only. */
typedef uint32_t (*xmv_data_resolver)(uint32_t data);
void xmv_original_set_data_resolver(xmv_data_resolver resolver);

/* T394: "FROM=TO". CreateDecoderForFile asked for the base name FROM opens TO in the same directory
 * instead, by rewriting the path string in the title's own buffer before the original runs. FABRICATED,
 * announced by the host, default off. Names are 1 to 32 letters, digits or underscores. NULL clears it.
 * Returns false (and changes nothing) for a malformed spec. */
bool xmv_original_set_substitute(const char *spec);

/* False when disabled or not one of the seven. True after the original returned, which
 * owns all register, stack and return value effects. Caller must arm host_run first.
 * Stops and faults release the bookkeeping and rethrow unchanged. */
bool xmv_original_dispatch(uint32_t address);

/* The seven export addresses in table order, for tests and reports. */
uint32_t xmv_original_address(size_t index);
#endif
