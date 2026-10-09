/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_XBOX_KERNEL_EVENT_HANDLE_H
#define TSFP_XBOX_KERNEL_EVENT_HANDLE_H

/* Registers NtCreateEvent (189) and NtSetEvent (225, T8c; see kernel_event_handle.c: stdcall
 * two arguments, only a NULL PreviousState, records the signal state, no waiter exists).
 * NtCreateEvent is stdcall four arguments. The measured
 * startup shape is unnamed, Type 1, initially clear at PASSIVE_LEVEL.
 * This creates an opaque handle with recorded metadata, not a mapped KEVENT:
 * no signaling, waiting, APC or I/O completion is provided by this module.
 * Publication probes output permission by writing its existing bytes back;
 * refusal preserves output contents, not an absence of attempted writes.
 * Guest mappings and contents must remain quiescent throughout publication.
 * Object-table reset is the existing quiescent session invalidation boundary.
 */
unsigned kernel_event_handle_register(void);

#endif
