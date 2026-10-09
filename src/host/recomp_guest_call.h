/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_RECOMP_GUEST_CALL_H
#define TSFP_RECOMP_GUEST_CALL_H
#include <stdbool.h>
#include <stdint.h>
/* T392: call a lifted guest stdcall function from host code on the CALLING guest thread's own
 * stack, as a nested call from inside an HLE handler (for example the XMV audio completion
 * callback run from DirectSoundDoWork). Not an interrupt or DPC emulation: the frame goes just
 * below the current guest stack pointer and the integer registers are restored afterwards.
 *
 * Returns false and leaves every register as it was when the function does not exist, the
 * frame is not writable stack, or the callee did not pop exactly its frame (a wrong argument
 * count). A guest stop or fault inside the callee propagates through host_run_stop as usual.
 * At most GUEST_CALL_MAX_ARGS arguments. Registers outside eax..ebp are not saved: only an
 * integer callee is admitted, and FPU, MMX and SSE state is left to it. */
#define GUEST_CALL_MAX_ARGS 4u
bool recomp_guest_call_stdcall(uint32_t address, const uint32_t *arguments, unsigned count);
#endif
