/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_DEVICES_H
#define TSFP_INPUT_XINPUT_DEVICES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Explicit EMPTY backend initialization, not an emulation of original OHCI,
 * kernel allocations, interrupt objects or enumeration event delivery. */
uint32_t xinput_devices_init_empty(uint32_t count, uint32_t declarations);
/* Exact guest 12-byte current/changed/previous table contracts. Peek outputs
 * are optional; Changes outputs are required. Unproven aliases are refused. */
uint32_t xinput_devices_get(uint32_t type);
uint32_t xinput_devices_peek(uint32_t type, uint32_t previous, uint32_t reconnected);
uint32_t xinput_devices_changes(uint32_t type, uint32_t insertions, uint32_t removals);
uint32_t xinput_devices_enumeration_status(void);
size_t xinput_devices_register(void);
/* Reset only this adapter's session/latch; preserves xinput_hle registry, counters,
 * layout, writer and explicit synthetic-pad policy. Call before a fresh session. */
void xinput_devices_reset(void);
/* T717: the opt-in FABRICATED synthetic pad (--synthetic-pad). Default off. Enabling needs the HLE port 0 to be
 * synthetic (xinput_hle_attach_synthetic_pad(0)); the pad is then reported inserted once at init and the five
 * open-path functions are answered. Contracts from the original bytes with a USB gamepad model
 * (tests/test_xinput_pad_oracle.py). Unknown shapes (type, port, slot, polling, handle, null or unmapped
 * buffers, event in the feedback header, double open) are refused through the fatal handler. The enable flag
 * is policy and survives xinput_devices_reset. */
void xinput_devices_enable_synthetic_pad(bool enabled);
/* Explicit host multiport mode; legacy mode and oracle restrictions remain default.
 * Calls must be serialized with guest input access by the provider. */
void xinput_devices_enable_multiport(bool enabled);
/* Provider ordering: acquire device serialization before any presenter/backend
 * job that subsequently reconciles lifecycle. SDL jobs must not acquire devices. */
void xinput_devices_run_locked(void (*job)(void *), void *user);
/* MU provider only: explicit attachment presence, bits port + 16*slot.
 * Call under run_locked across the complete lifecycle operation. Policy survives reset. */
void xinput_devices_set_mu_mask(uint32_t mask);
bool xinput_pad_connect(unsigned port);
bool xinput_pad_disconnect(unsigned port);
bool xinput_devices_synthetic_pad_enabled(void);
size_t xinput_devices_register_pad(void);
/* Open returns 0 (NULL) for a known non-gamepad table, a port but 0, slot 1, a second open and a removed pad
 * (T723, measured). Unknown tables stay refused. A polling block (T731) is accepted only with fAutoPoll set and non-zero intervals. */
uint32_t xinput_pad_open(uint32_t type, uint32_t port, uint32_t slot, uint32_t polling);
/* Hot unplug of the synthetic pad (T723): the table reports one removal, an open handle answers 0x48F until
 * closed. Refused while a feedback transfer is pending. Nothing in the host calls this yet. */
void xinput_pad_remove(void);
/* T731: reinsert a removed pad (new, at rest, old handle closed). Refused while the old handle is open. */
void xinput_pad_insert(void);
/* T731: FABRICATED removal source, unplug after the n-th GetState (0 = never, the default). Policy: survives reset. */
void xinput_pad_remove_after_polls(uint32_t polls);
void xinput_pad_close(uint32_t handle);
uint32_t xinput_pad_capabilities(uint32_t handle, uint32_t out);
uint32_t xinput_pad_state_read(uint32_t handle, uint32_t out);
uint32_t xinput_pad_feedback(uint32_t handle, uint32_t feedback);
/* The last rumble output report (6 bytes: 0, 6, left LE, right LE) and how many were sent. */
bool xinput_pad_last_output(uint8_t out[6]);
uint64_t xinput_pad_output_count(void);
typedef void (*xinput_devices_fatal_fn)(uint32_t address, const char *message);
void xinput_devices_set_fatal(xinput_devices_fatal_fn handler);
#endif
