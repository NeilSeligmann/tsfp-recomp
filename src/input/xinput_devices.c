/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_devices.h"
#include "xinput_source.h"
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "kernel_call.h"
#include "xinput_hle.h"

#define INIT_ENTRY 0x0046DBCDu
#define GET_ENTRY 0x0046DBD2u
#define PEEK_ENTRY 0x0046DB91u
#define CHANGES_ENTRY 0x0046DBF4u
#define STATUS_ENTRY 0x0046F3A8u
#define STATUS_COUNT 0x00771454u
#define STATUS_FLAG 0x00771370u
static const uint32_t types[] = {
    0x0046C6E0u, 0x0046C75Cu, 0x0046C7C0u,
    0x0046C894u, 0x0046C8A0u, 0x0046C8ACu
};
#define OPEN_ENTRY 0x0046E133u
#define CLOSE_ENTRY 0x0046E189u
#define CAPS_ENTRY 0x0046E195u
#define STATE_ENTRY 0x0046E36Du
#define SETSTATE_ENTRY 0x0046E3E0u
#define GAMEPAD_TYPE 0x0046C75Cu
/* Fabricated opaque token for the one open pad. The original returns an XPP heap pointer the title only
 * stores and passes back, so only its non-zero identity matters. */
#define PAD_HANDLE 0x58504430u
#define ERROR_IO_PENDING 997u
#define ERROR_DEVICE_NOT_CONNECTED 0x48Fu
#define FEEDBACK_BYTES 0x46u
#define XINPUT_PAD_ANALOG_THRESHOLD 0x20u
/* Serialize guest tables/handles and lifecycle. Recursive because the sampled
 * host provider may reconcile hotplug from inside a GetState callback. */
static pthread_mutex_t devices_mutex;
static pthread_once_t devices_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned devices_depth;
static void devices_mutex_init(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) abort();
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) abort();
    if (pthread_mutex_init(&devices_mutex, &attr) != 0) abort();
    (void)pthread_mutexattr_destroy(&attr);
}
static void devices_lock(void)
{
    if (pthread_once(&devices_once, devices_mutex_init) != 0 || pthread_mutex_lock(&devices_mutex) != 0) abort();
    devices_depth++;
}
static void devices_unlock(void)
{
    devices_depth--;
    if (pthread_mutex_unlock(&devices_mutex) != 0) abort();
}
static bool ready;
static uint32_t mu_presence;
static bool announced;
static bool pad_enabled;
static bool multiport_enabled;
typedef struct {
    uint32_t token;
    unsigned port;
    bool removed;
    uint32_t report_base;
    uint32_t pending_feedback;
} host_handle;
#define HOST_HANDLES 64u
static host_handle host_handles[HOST_HANDLES];
static uint32_t host_token_sequence;
static uint32_t host_active[XINPUT_PORT_COUNT];
static void host_reset(void);
static uint32_t host_open(uint32_t type, uint32_t port, uint32_t slot, uint32_t polling);
static void host_close(uint32_t handle);
static uint32_t host_capabilities(uint32_t handle, uint32_t out);
static uint32_t host_read(uint32_t handle, uint32_t out);
static uint32_t host_feedback(uint32_t handle, uint32_t feedback);
static bool pad_open;
static bool pad_removed;
static xinput_pad_state pad_snapshot;
static bool pad_inserted_announced;
static uint32_t pad_packet;
static uint32_t pad_report_base;
static uint32_t pad_remove_after_polls;
static uint32_t pad_polls;
static uint32_t pad_pending_feedback;
static uint8_t pad_output[6];
static uint64_t pad_output_count;
static xinput_devices_fatal_fn fatal_handler;

static void refuse(uint32_t entry, const char *message) __attribute__((noreturn));
static void refuse(uint32_t entry, const char *message)
{
    xinput_hle_log()("xinput EMPTY adapter: %#x refused: %s\n", entry, message);
    while (devices_depth != 0u) devices_unlock();
    if (fatal_handler != NULL) fatal_handler(entry, message);
    abort();
}
static void locked_xinput_devices_set_fatal(xinput_devices_fatal_fn handler) { fatal_handler = handler; }
static void locked_xinput_devices_reset(void)
{
    host_reset();
    ready = false; announced = false; pad_open = false; pad_removed = false;
    pad_inserted_announced = false; memset(&pad_snapshot, 0, sizeof(pad_snapshot));
    pad_packet = 0u; pad_report_base = 0u; pad_polls = 0u; pad_pending_feedback = 0u; pad_output_count = 0u;
    memset(pad_output, 0, sizeof(pad_output));
}
static void locked_xinput_devices_enable_synthetic_pad(bool enabled) { pad_enabled = enabled; }
static void locked_xinput_devices_enable_multiport(bool enabled) { multiport_enabled = enabled; }
static bool locked_xinput_devices_synthetic_pad_enabled(void) { return pad_enabled; }
/* Pad mode is the explicit adapter policy AND exactly port 0 synthetic, or (after xinput_pad_remove, T723)
 * the removed pad with no device left. Anything else with a device is refused. */
static bool pad_mode(void)
{
    if (!pad_enabled) return false;
    if (multiport_enabled) return true;
    if (pad_removed) return xinput_hle_connected_count() == 0u;
    return xinput_hle_connected_count() == 1u && xinput_hle_port_state(0u) == XINPUT_PORT_SYNTHETIC;
}
static void require_empty(uint32_t entry)
{
    if (pad_mode()) return;
    if (pad_enabled)
        refuse(entry, "synthetic pad adapter requires exactly port 0 synthetic");
    if (xinput_hle_connected_count() != 0u)
        refuse(entry, "explicit synthetic pad requires the --synthetic-pad adapter");
}
static void require_ready(uint32_t entry)
{
    require_empty(entry);
    if (!ready) refuse(entry, "EMPTY backend is not initialized");
}
static bool overlaps(uint32_t a, uint32_t size_a, uint32_t b, uint32_t size_b)
{
    return (uint64_t)a < (uint64_t)b + size_b && (uint64_t)b < (uint64_t)a + size_a;
}
static void *table_at(uint32_t entry, uint32_t type)
{
    bool known = false;
    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); i++)
        if (types[i] == type) known = true;
    if (!known) refuse(entry, "unknown device-type table");
    void *table = kernel_guest_at(type, 12u);
    if (table == NULL) refuse(entry, "device-type table is unreadable");
    return table;
}
static void *output_at(uint32_t entry, uint32_t address, bool optional)
{
    if (address == 0u && optional) return NULL;
    void *output = kernel_guest_at(address, 4u);
    if (output == NULL) refuse(entry, "output is not mapped for four bytes");
    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); i++)
        if (overlaps(address, 4u, types[i], 12u))
            refuse(entry, "output alias with a device-type table is not recovered");
    return output;
}
static void output_pair(uint32_t entry, uint32_t first, uint32_t second,
    bool optional, void **first_at, void **second_at)
{
    *first_at = output_at(entry, first, optional);
    *second_at = output_at(entry, second, optional);
    if (*first_at != NULL && *second_at != NULL && overlaps(first, 4u, second, 4u))
        refuse(entry, "overlapping outputs are not recovered");
}
static uint32_t locked_xinput_devices_init_empty(uint32_t count, uint32_t declarations)
{
    require_empty(INIT_ENTRY);
    if (ready) refuse(INIT_ENTRY, "repeated initialization is not recovered");
    static const uint32_t expected[8] = {
        0x0046C6E0u,8u,0x0046C8A0u,4u,0x0046C894u,4u,0x0046C75Cu,4u
    };
    uint32_t words[8];
    const void *input = kernel_guest_at(declarations, sizeof(words));
    if (count != 4u || input == NULL) refuse(INIT_ENTRY, "expected four readable declarations");
    memcpy(words, input, sizeof(words));
    if (memcmp(words, expected, sizeof(words)) != 0)
        refuse(INIT_ENTRY, "only the measured declaration order/counts are recovered");
    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); i++) {
        uint32_t state[3];
        memcpy(state, table_at(INIT_ENTRY, types[i]), sizeof(state));
        if ((state[0] | state[1] | state[2]) != 0u)
            refuse(INIT_ENTRY, "cold EMPTY initialization requires zero device masks");
    }
    void *status_count = kernel_guest_at(STATUS_COUNT, 4u);
    void *status_flag = kernel_guest_at(STATUS_FLAG, 1u);
    if (status_count == NULL || status_flag == NULL)
        refuse(INIT_ENTRY, "enumeration status globals are unreadable");
    if (overlaps(declarations, 32u, STATUS_COUNT, 4u) ||
        overlaps(declarations, 32u, STATUS_FLAG, 1u))
        refuse(INIT_ENTRY, "declarations alias status globals");
    /* Original empty-model final values are both zero (also zero in this XBE).
     * Normalize these PUBLIC status inputs explicitly; no USB objects are fabricated. */
    const uint32_t zero = 0u;
    memcpy(status_count, &zero, sizeof(zero));
    memset(status_flag, 0, 1u);
    ready = true;
    if (pad_mode()) {
        /* FABRICATED: the gamepad table reads as the measured original after one pad enumerated on a root port
         * (tests/test_xinput_pad_oracle.py): current 1, changed 1, previous 0, so GetDeviceChanges reports the
         * insertion exactly once. Done once per init, which refuses repeats. */
        uint32_t mask = 1u;
        if (multiport_enabled) {
            mask = 0u;
            for (unsigned port = 0; port < XINPUT_PORT_COUNT; port++)
                if (xinput_hle_port_state(port) == XINPUT_PORT_SYNTHETIC) mask |= 1u << port;
        }
        const uint32_t inserted[3] = {mask, mask, 0u};
        memcpy(table_at(INIT_ENTRY, GAMEPAD_TYPE), inserted, sizeof(inserted));
        if (!multiport_enabled && !pad_inserted_announced) {
            xinput_hle_log()("xinput FABRICATED: synthetic gamepad reported inserted on port 0 exactly once "
                "(no USB, no host input; buttons and sticks at rest unless set)\n");
            pad_inserted_announced = true;
        }
    }
    const uint32_t mu_inserted[3] = {mu_presence, mu_presence, 0u};
    memcpy(table_at(INIT_ENTRY, 0x0046C6E0u), mu_inserted, sizeof(mu_inserted));
    if (!announced) {
        xinput_hle_log()(multiport_enabled ?
            "xinput: explicit four-port host adapter initialized; guest USB/presence model, no IRQ equivalence\n" : pad_mode() ?
            "xinput: initialized explicit backend with the FABRICATED synthetic pad on port 0, other ports empty; "
            "no host input, OHCI, IRQ or heap objects supplied\n" :
            "xinput: initialized explicit EMPTY gamepad backend; MU presence is explicit host policy, "
            "no host input, OHCI, IRQ, heap objects or enumeration events supplied\n");
        announced = true;
    }
    return 0u;
}
static uint32_t locked_xinput_devices_get(uint32_t type)
{
    require_ready(GET_ENTRY);
    void *table = table_at(GET_ENTRY, type);
    uint32_t state[3];
    memcpy(state, table, sizeof(state));
    state[1] = 0u;
    state[2] = state[0];
    memcpy(table, state, sizeof(state));
    return state[0];
}
static uint32_t locked_xinput_devices_peek(uint32_t type, uint32_t previous, uint32_t reconnected)
{
    require_ready(PEEK_ENTRY);
    const void *table = table_at(PEEK_ENTRY, type);
    void *previous_at, *reconnected_at;
    output_pair(PEEK_ENTRY, previous, reconnected, true, &previous_at, &reconnected_at);
    uint32_t state[3];
    memcpy(state, table, sizeof(state));
    const uint32_t reconnect = state[1] & state[2] & state[0];
    if (previous_at != NULL) memcpy(previous_at, &state[2], 4u);
    if (reconnected_at != NULL) memcpy(reconnected_at, &reconnect, 4u);
    return state[0];
}
static uint32_t locked_xinput_devices_changes(uint32_t type, uint32_t insertions, uint32_t removals)
{
    require_ready(CHANGES_ENTRY);
    void *table = table_at(CHANGES_ENTRY, type);
    void *insertions_at, *removals_at;
    output_pair(CHANGES_ENTRY, insertions, removals, false, &insertions_at, &removals_at);
    uint32_t state[3], added = 0u, removed = 0u;
    memcpy(state, table, sizeof(state));
    if (state[1] != 0u) {
        const uint32_t reconnect = state[1] & state[2] & state[0];
        added = (~state[2] & state[0]) | reconnect;
        removed = (~state[0] & state[2]) | reconnect;
        state[1] = 0u;
        state[2] = state[0];
    }
    memcpy(insertions_at, &added, 4u);
    memcpy(removals_at, &removed, 4u);
    memcpy(table, state, sizeof(state));
    return (added | removed) != 0u ? 1u : 0u;
}
static uint32_t locked_xinput_devices_enumeration_status(void)
{
    require_ready(STATUS_ENTRY);
    uint32_t count;
    uint8_t flag;
    if (!kernel_guest_read_u32(STATUS_COUNT, &count) ||
        !kernel_guest_read_u8(STATUS_FLAG, &flag))
        refuse(STATUS_ENTRY, "enumeration status globals are unreadable");
    return count != 0u || flag != 0u ? 1u : 0u;
}
static uint32_t argument(void *context, uint32_t entry, unsigned index)
{
    uint32_t value;
    if (!kernel_frame_arg(context, index, &value)) refuse(entry, "unreadable stack argument");
    return value;
}
static uint32_t init_handler(void *context)
{
    const uint32_t count = argument(context, INIT_ENTRY, 0u);
    const uint32_t declarations = argument(context, INIT_ENTRY, 1u);
    return xinput_devices_init_empty(count, declarations);
}
static uint32_t get_handler(void *context)
{ return xinput_devices_get(argument(context, GET_ENTRY, 0u)); }
static uint32_t peek_handler(void *context)
{
    const uint32_t type = argument(context, PEEK_ENTRY, 0u);
    const uint32_t previous = argument(context, PEEK_ENTRY, 1u);
    const uint32_t reconnected = argument(context, PEEK_ENTRY, 2u);
    return xinput_devices_peek(type, previous, reconnected);
}
static uint32_t changes_handler(void *context)
{
    const uint32_t type = argument(context, CHANGES_ENTRY, 0u);
    const uint32_t insertions = argument(context, CHANGES_ENTRY, 1u);
    const uint32_t removals = argument(context, CHANGES_ENTRY, 2u);
    return xinput_devices_changes(type, insertions, removals);
}
/* ---- synthetic pad open path (opt-in). Contracts measured against the original bytes with a USB gamepad model,
 * tests/test_xinput_pad_oracle.py. Every unmeasured shape is refused loudly. ---- */
static void require_pad(uint32_t entry)
{
    if (!pad_mode()) refuse(entry, "pad calls need the --synthetic-pad adapter on port 0");
    if (!ready) refuse(entry, "synthetic pad adapter is not initialized");
}
static void require_open(uint32_t entry, uint32_t handle)
{
    require_pad(entry);
    if (!pad_open || handle != PAD_HANDLE) refuse(entry, "handle is not the open synthetic pad");
}
static void complete_feedback(uint32_t entry)
{
    if (pad_pending_feedback == 0u) return;
    uint32_t status;
    if (!kernel_guest_read_u32(pad_pending_feedback, &status))
        refuse(entry, "pending feedback block became unreadable");
    if (status == ERROR_IO_PENDING && !kernel_guest_write_u32(pad_pending_feedback, 0u))
        refuse(entry, "pending feedback block became unwritable");
    pad_pending_feedback = 0u;
}
/* Sample the host pad: the packet number follows the raw report. Measured (T731): the original counts every report
 * change since the pad was opened (changes before the Open do not count, every Open starts again at 1). */
static xinput_pad_state sample_pad(void)
{
    const xinput_pad_state now = pad_removed ? pad_snapshot : xinput_hle_pad_state(0u);
    pad_packet = 1u + xinput_hle_synthetic_report_changes(0u) - pad_report_base;
    pad_snapshot = now;
    return now;
}
static bool known_type(uint32_t type)
{
    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); i++)
        if (types[i] == type) return true;
    return false;
}
/* T723, measured with the oracle (tests/test_xinput_pad_oracle.py): Open returns NULL, with no other effect, for a
 * known table that is not the gamepad's, for any port but 0, for slot 1, for a second open, and once the pad is
 * removed. Only slot 1 selects another slot, every other slot value opens like slot 0. The library's last-error
 * code (0x57, 0x48F or 0x20) is not modelled: nothing here writes the title's thread storage. */
/* T731, measured: a polling block is four bytes, flags (bit 0 fAutoPoll, bit 1 fInterruptOut), input interval,
 * output interval, reserved. With fAutoPoll set the original answers every scenario of the oracle test exactly
 * like the NULL block (intervals 1..255, with or without fInterruptOut). fAutoPoll clear changes the polling
 * model (GetState transcripts differ), an input interval of 0, or an output interval of 0 with fInterruptOut,
 * does not return in the oracle (a spin at 0x473120, so possibly the oracle's own gap): those are refused. */
static void check_polling(uint32_t polling)
{
    const uint8_t *block = kernel_guest_at(polling, 4u);
    if (block == NULL) refuse(OPEN_ENTRY, "polling parameter block is not mapped for four bytes");
    if ((block[0] & 0xFCu) != 0u || block[3] != 0u) refuse(OPEN_ENTRY, "reserved polling bits are not recovered");
    if ((block[0] & 1u) == 0u) refuse(OPEN_ENTRY, "polling without fAutoPoll is not recovered");
    if (block[1] == 0u) refuse(OPEN_ENTRY, "input interval 0 does not return in the oracle");
    if ((block[0] & 2u) != 0u && block[2] == 0u) refuse(OPEN_ENTRY, "output interval 0 does not return in the oracle");
}
static uint32_t locked_xinput_pad_open(uint32_t type, uint32_t port, uint32_t slot, uint32_t polling)
{
    if (multiport_enabled) return host_open(type, port, slot, polling);
    require_pad(OPEN_ENTRY);
    if (!known_type(type)) refuse(OPEN_ENTRY, "unknown device-type table");
    if (type != GAMEPAD_TYPE || port != 0u || slot == 1u || pad_open || pad_removed) {
        if (polling != 0u) refuse(OPEN_ENTRY, "polling parameters with an Open that returns NULL are not recovered");
        return 0u;
    }
    if (polling != 0u) check_polling(polling);
    pad_open = true;
    pad_report_base = xinput_hle_synthetic_report_changes(0u);
    return PAD_HANDLE;
}
static void locked_xinput_pad_close(uint32_t handle)
{
    if (multiport_enabled) { host_close(handle); return; }
    require_open(CLOSE_ENTRY, handle);
    pad_open = false;
    pad_pending_feedback = 0u;
    (void)xinput_feedback_send(0u, 0u, 0u);
}
/* T723 removal (hot unplug). Measured: the gamepad table goes to current 0 and changed 1 with previous kept, so
 * GetDeviceChanges reports the removal once. A pad still open keeps answering with 0x48F (below). A feedback
 * transfer in flight finishes or fails (status 0 or 0x1F) depending on the milliseconds left to it, which this
 * adapter has no clock for, so removal with one pending is refused. The last sample is taken here. Reinsertion
 * is not recovered. */
static void locked_xinput_pad_remove(void)
{
    require_pad(OPEN_ENTRY);
    if (pad_removed) refuse(OPEN_ENTRY, "removing an already removed pad");
    if (pad_pending_feedback != 0u) refuse(OPEN_ENTRY, "removal with a feedback transfer pending is not recovered");
    sample_pad();
    uint32_t state[3];
    void *table = table_at(OPEN_ENTRY, GAMEPAD_TYPE);
    memcpy(state, table, sizeof(state));
    state[0] = 0u;
    state[1] = 1u;
    memcpy(table, state, sizeof(state));
    pad_removed = true;
    (void)xinput_hle_detach_synthetic_pad(0u);
}
/* T731 reinsertion (measured with a fresh pad on the same root port): the table goes to current 1, changed 1 with
 * previous kept, so GetDeviceChanges reports the insertion once (and a removal too if the title never consumed
 * the removal). The new pad is at rest and an Open starts its packet number at 1. The old handle must be closed
 * first: an old handle left open answers 0x48F while a new Open succeeds beside it, not recovered. */
static void locked_xinput_pad_insert(void)
{
    require_pad(OPEN_ENTRY);
    if (!pad_removed) refuse(OPEN_ENTRY, "inserting a pad that is not removed");
    if (pad_open) refuse(OPEN_ENTRY, "reinsertion with the old handle still open is not recovered");
    (void)xinput_hle_attach_synthetic_pad(0u);
    uint32_t state[3];
    void *table = table_at(OPEN_ENTRY, GAMEPAD_TYPE);
    memcpy(state, table, sizeof(state));
    state[0] = 1u;
    state[1] = 1u;
    memcpy(table, state, sizeof(state));
    pad_removed = false;
    memset(&pad_snapshot, 0, sizeof(pad_snapshot));
}
/* FABRICATED host source for removal: after the n-th GetState the pad is unplugged (0 = never). It acts after that
 * read has completed any pending feedback and answered, so no transfer is pending (removal refuses one). */
static void locked_xinput_pad_remove_after_polls(uint32_t polls) { pad_remove_after_polls = polls; }
static uint32_t locked_xinput_pad_capabilities(uint32_t handle, uint32_t out)
{
    if (multiport_enabled) return host_capabilities(handle, out);
    require_open(CAPS_ENTRY, handle);
    uint8_t *at = kernel_guest_at(out, 25u);
    if (at == NULL) refuse(CAPS_ENTRY, "capabilities output is not mapped for 25 bytes");
    if (pad_removed) {
        /* Measured: a removed pad zeroes bytes 1 and 2 only and fails with 0x48F. */
        at[1] = 0u; at[2] = 0u;
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    at[0] = 1u; at[1] = 0u; at[2] = 0u;
    memset(at + 3u, 0xFF, 22u);
    return 0u;
}
static uint32_t locked_xinput_pad_state_read(uint32_t handle, uint32_t out)
{
    if (multiport_enabled) return host_read(handle, out);
    require_open(STATE_ENTRY, handle);
    uint8_t *at = kernel_guest_at(out, 22u);
    if (at == NULL) refuse(STATE_ENTRY, "state output is not mapped for 22 bytes");
    complete_feedback(STATE_ENTRY);
    /* T707: an installed host source (scripted, later keyboard or gamepad) updates the pad before this sample. */
    if (!pad_removed) (void)xinput_source_poll();
    const xinput_pad_state now = sample_pad();
    const uint32_t packet = pad_packet;
    memcpy(at, &packet, 4u);
    memcpy(at + 4u, &now.digital_buttons, 2u);
    /* Measured: the original zeroes every analog pressure byte below 0x20 (all eight indexes, 0..255 swept) and
     * passes the rest, buttons and thumbs unchanged. The packet number tracks the RAW report, not this view. */
    for (unsigned i = 0u; i < XINPUT_ANALOG_COUNT; i++)
        at[6u + i] = now.analog[i] < XINPUT_PAD_ANALOG_THRESHOLD ? 0u : now.analog[i];
    memcpy(at + 14u, &now.thumb_left_x, 2u);
    memcpy(at + 16u, &now.thumb_left_y, 2u);
    memcpy(at + 18u, &now.thumb_right_x, 2u);
    memcpy(at + 20u, &now.thumb_right_y, 2u);
    const uint32_t result = pad_removed ? ERROR_DEVICE_NOT_CONNECTED : 0u;
    if (!pad_removed && pad_remove_after_polls != 0u && ++pad_polls == pad_remove_after_polls) {
        xinput_hle_log()("xinput FABRICATED: the synthetic pad is unplugged after poll %u "
            "(--synthetic-pad-remove-after-polls)\n", pad_polls);
        xinput_pad_remove();
    }
    return result;
}
static uint32_t locked_xinput_pad_feedback(uint32_t handle, uint32_t feedback)
{
    if (multiport_enabled) return host_feedback(handle, feedback);
    require_open(SETSTATE_ENTRY, handle);
    uint8_t *at = kernel_guest_at(feedback, FEEDBACK_BYTES);
    if (at == NULL) refuse(SETSTATE_ENTRY, "feedback block is not mapped for 0x46 bytes");
    uint32_t event;
    memcpy(&event, at + 4u, 4u);
    if (event != 0u) refuse(SETSTATE_ENTRY, "a notification event in the feedback header is not recovered");
    if (pad_removed) {
        /* Measured: header status 0x48F, no output report. */
        const uint32_t failed = ERROR_DEVICE_NOT_CONNECTED;
        memcpy(at, &failed, 4u);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    complete_feedback(SETSTATE_ENTRY);
    /* Original output report: report id 0, length 6, left then right motor speed, little endian. */
    pad_output[0] = 0u; pad_output[1] = 6u;
    memcpy(pad_output + 2u, at + 0x42u, 4u);
    pad_output_count++;
    uint16_t left, right;
    memcpy(&left, at + 0x42u, 2u); memcpy(&right, at + 0x44u, 2u);
    (void)xinput_feedback_send(0u, left, right);
    const uint32_t pending = ERROR_IO_PENDING;
    memcpy(at, &pending, 4u);
    pad_pending_feedback = feedback;
    return ERROR_IO_PENDING;
}
static bool locked_xinput_pad_last_output(uint8_t out[6])
{
    memcpy(out, pad_output, sizeof(pad_output));
    return pad_output_count != 0u;
}
static uint64_t locked_xinput_pad_output_count(void) { return pad_output_count; }
/* Explicit host adapter: per-connection opaque handles, independent raw report
 * counters and feedback. This models guest contracts, not USB timing/interrupts. */
static host_handle *host_find(uint32_t token, uint32_t entry)
{
    require_pad(entry);
    for (unsigned i = 0; i < HOST_HANDLES; i++)
        if (host_handles[i].token == token && token != 0u) return &host_handles[i];
    refuse(entry, "handle is not an open host pad");
}
static void host_complete(host_handle *h, uint32_t entry, uint32_t status)
{
    if (h->pending_feedback == 0u) return;
    uint32_t value;
    if (!kernel_guest_read_u32(h->pending_feedback, &value)) refuse(entry, "pending feedback block is unreadable");
    if (value == ERROR_IO_PENDING && !kernel_guest_write_u32(h->pending_feedback, status))
        refuse(entry, "pending feedback block is unwritable");
    h->pending_feedback = 0u;
}
static void host_reset(void)
{
    for (unsigned port = 0; port < XINPUT_PORT_COUNT; port++) (void)xinput_feedback_send(port, 0u, 0u);
    /* Reset begins a fresh guest address space; never dereference old feedback. */
    memset(host_handles, 0, sizeof(host_handles));
    memset(host_active, 0, sizeof(host_active));
}
static void host_presence(unsigned port, bool connected)
{
    if (!ready) return;
    uint32_t state[3];
    void *table = table_at(OPEN_ENTRY, GAMEPAD_TYPE);
    memcpy(state, table, sizeof(state));
    const uint32_t bit = 1u << port;
    if (connected) state[0] |= bit; else state[0] &= ~bit;
    state[1] |= bit;
    memcpy(table, state, sizeof(state));
}
static bool locked_xinput_pad_connect(unsigned port)
{
    if (!multiport_enabled || !pad_enabled || port >= XINPUT_PORT_COUNT) return false;
    if (xinput_hle_port_state(port) == XINPUT_PORT_SYNTHETIC) return true;
    (void)xinput_hle_attach_synthetic_pad(port);
    host_presence(port, true);
    return true;
}
static bool locked_xinput_pad_disconnect(unsigned port)
{
    if (!multiport_enabled || !pad_enabled || port >= XINPUT_PORT_COUNT) return false;
    if (xinput_hle_port_state(port) == XINPUT_PORT_EMPTY) return true;
    for (unsigned i = 0; i < HOST_HANDLES; i++) {
        host_handle *h = &host_handles[i];
        if (h->token != 0u && h->port == port && !h->removed) {
            host_complete(h, SETSTATE_ENTRY, 0x1Fu);
            h->removed = true;
        }
    }
    (void)xinput_feedback_send(port, 0u, 0u);
    host_active[port] = 0u;
    (void)xinput_hle_detach_synthetic_pad(port);
    host_presence(port, false);
    return true;
}
static uint32_t host_open(uint32_t type, uint32_t port, uint32_t slot, uint32_t polling)
{
    require_pad(OPEN_ENTRY);
    if (!known_type(type)) refuse(OPEN_ENTRY, "unknown device-type table");
    if (type != GAMEPAD_TYPE || port >= XINPUT_PORT_COUNT || slot == 1u ||
        xinput_hle_port_state(port) != XINPUT_PORT_SYNTHETIC || host_active[port] != 0u) {
        if (polling != 0u) refuse(OPEN_ENTRY, "polling parameters with a failed Open are unsupported");
        return 0u;
    }
    if (polling != 0u) check_polling(polling);
    for (unsigned i = 0; i < HOST_HANDLES; i++) {
        if (host_handles[i].token != 0u) continue;
        if (host_token_sequence >= 0x7FFFFu) refuse(OPEN_ENTRY, "host handle generation exhausted");
        const uint32_t token = 0x58600000u | (++host_token_sequence << 2u) | port;
        host_handles[i] = (host_handle){token, port, false, xinput_hle_synthetic_report_changes(port), 0u};
        host_active[port] = token;
        return token;
    }
    refuse(OPEN_ENTRY, "too many outstanding host handles");
}
static void host_close(uint32_t token)
{
    host_handle *h = host_find(token, CLOSE_ENTRY);
    host_complete(h, CLOSE_ENTRY, 0x1Fu);
    if (host_active[h->port] == token) {
        host_active[h->port] = 0u;
        (void)xinput_feedback_send(h->port, 0u, 0u);
    }
    memset(h, 0, sizeof(*h));
}
static uint32_t host_capabilities(uint32_t token, uint32_t out)
{
    host_handle *h = host_find(token, CAPS_ENTRY);
    uint8_t *at = kernel_guest_at(out, 25u);
    if (at == NULL) refuse(CAPS_ENTRY, "capabilities output is not mapped for 25 bytes");
    if (h->removed) { at[1] = 0u; at[2] = 0u; return ERROR_DEVICE_NOT_CONNECTED; }
    at[0] = 1u; at[1] = 0u; at[2] = 0u; memset(at + 3u, 0xFF, 22u);
    return 0u;
}
static uint32_t host_read(uint32_t token, uint32_t out)
{
    host_handle *h = host_find(token, STATE_ENTRY);
    uint8_t *at = kernel_guest_at(out, 22u);
    if (at == NULL) refuse(STATE_ENTRY, "state output is not mapped for 22 bytes");
    host_complete(h, STATE_ENTRY, 0u);
    if (!h->removed) (void)xinput_source_poll_port(h->port);
    xinput_pad_state state = {0};
    uint32_t packet = 0u;
    if (!h->removed) {
        state = xinput_hle_pad_state(h->port);
        packet = 1u + xinput_hle_synthetic_report_changes(h->port) - h->report_base;
    }
    memcpy(at, &packet, 4u); memcpy(at + 4u, &state.digital_buttons, 2u);
    for (unsigned i = 0; i < XINPUT_ANALOG_COUNT; i++) at[6u + i] = state.analog[i] < XINPUT_PAD_ANALOG_THRESHOLD ? 0u : state.analog[i];
    memcpy(at + 14u, &state.thumb_left_x, 8u);
    return h->removed ? ERROR_DEVICE_NOT_CONNECTED : 0u;
}
static uint32_t host_feedback(uint32_t token, uint32_t feedback)
{
    host_handle *h = host_find(token, SETSTATE_ENTRY);
    uint8_t *at = kernel_guest_at(feedback, FEEDBACK_BYTES);
    if (at == NULL) refuse(SETSTATE_ENTRY, "feedback block is not mapped for 0x46 bytes");
    uint32_t event; memcpy(&event, at + 4u, 4u);
    if (event != 0u) refuse(SETSTATE_ENTRY, "notification event is unsupported");
    if (h->removed) { const uint32_t status = ERROR_DEVICE_NOT_CONNECTED; memcpy(at, &status, 4u); return status; }
    host_complete(h, SETSTATE_ENTRY, 0u);
    uint16_t left, right; memcpy(&left, at + 0x42u, 2u); memcpy(&right, at + 0x44u, 2u);
    (void)xinput_feedback_send(h->port, left, right);
    const uint32_t status = ERROR_IO_PENDING; memcpy(at, &status, 4u); h->pending_feedback = feedback;
    return status;
}

static uint32_t open_handler(void *context)
{
    const uint32_t type = argument(context, OPEN_ENTRY, 0u);
    const uint32_t port = argument(context, OPEN_ENTRY, 1u);
    const uint32_t slot = argument(context, OPEN_ENTRY, 2u);
    const uint32_t polling = argument(context, OPEN_ENTRY, 3u);
    return xinput_pad_open(type, port, slot, polling);
}
static uint32_t close_handler(void *context)
{ xinput_pad_close(argument(context, CLOSE_ENTRY, 0u)); return 0u; }
static uint32_t capabilities_handler(void *context)
{
    const uint32_t handle = argument(context, CAPS_ENTRY, 0u);
    return xinput_pad_capabilities(handle, argument(context, CAPS_ENTRY, 1u));
}
static uint32_t state_handler(void *context)
{
    const uint32_t handle = argument(context, STATE_ENTRY, 0u);
    return xinput_pad_state_read(handle, argument(context, STATE_ENTRY, 1u));
}
static uint32_t feedback_handler(void *context)
{
    const uint32_t handle = argument(context, SETSTATE_ENTRY, 0u);
    return xinput_pad_feedback(handle, argument(context, SETSTATE_ENTRY, 1u));
}
static size_t locked_xinput_devices_register_pad(void)
{
    size_t count = 0u;
    count += xinput_hle_register(OPEN_ENTRY, open_handler) ? 1u : 0u;
    count += xinput_hle_register(CLOSE_ENTRY, close_handler) ? 1u : 0u;
    count += xinput_hle_register(CAPS_ENTRY, capabilities_handler) ? 1u : 0u;
    count += xinput_hle_register(STATE_ENTRY, state_handler) ? 1u : 0u;
    count += xinput_hle_register(SETSTATE_ENTRY, feedback_handler) ? 1u : 0u;
    return count;
}
static uint32_t status_handler(void *context)
{ (void)context; return xinput_devices_enumeration_status(); }
static size_t locked_xinput_devices_register(void)
{
    size_t count = 0u;
    count += xinput_hle_register(INIT_ENTRY, init_handler) ? 1u : 0u;
    count += xinput_hle_register(GET_ENTRY, get_handler) ? 1u : 0u;
    count += xinput_hle_register(PEEK_ENTRY, peek_handler) ? 1u : 0u;
    count += xinput_hle_register(CHANGES_ENTRY, changes_handler) ? 1u : 0u;
    count += xinput_hle_register(STATUS_ENTRY, status_handler) ? 1u : 0u;
    return count;
}

/* Public serialized entry points. */
uint32_t xinput_devices_init_empty(uint32_t count, uint32_t declarations)
{ devices_lock(); uint32_t result = locked_xinput_devices_init_empty(count, declarations); devices_unlock(); return result; }
uint32_t xinput_devices_get(uint32_t type)
{ devices_lock(); uint32_t result = locked_xinput_devices_get(type); devices_unlock(); return result; }
uint32_t xinput_devices_peek(uint32_t type, uint32_t previous, uint32_t reconnected)
{ devices_lock(); uint32_t result = locked_xinput_devices_peek(type, previous, reconnected); devices_unlock(); return result; }
uint32_t xinput_devices_changes(uint32_t type, uint32_t insertions, uint32_t removals)
{ devices_lock(); uint32_t result = locked_xinput_devices_changes(type, insertions, removals); devices_unlock(); return result; }
uint32_t xinput_devices_enumeration_status(void)
{ devices_lock(); uint32_t result = locked_xinput_devices_enumeration_status(); devices_unlock(); return result; }
size_t xinput_devices_register(void)
{ devices_lock(); size_t result = locked_xinput_devices_register(); devices_unlock(); return result; }
void xinput_devices_reset(void)
{ devices_lock(); locked_xinput_devices_reset(); devices_unlock(); }
void xinput_devices_enable_synthetic_pad(bool enabled)
{ devices_lock(); locked_xinput_devices_enable_synthetic_pad(enabled); devices_unlock(); }
void xinput_devices_enable_multiport(bool enabled)
{ devices_lock(); locked_xinput_devices_enable_multiport(enabled); devices_unlock(); }
bool xinput_pad_connect(unsigned port)
{ devices_lock(); bool result = locked_xinput_pad_connect(port); devices_unlock(); return result; }
bool xinput_pad_disconnect(unsigned port)
{ devices_lock(); bool result = locked_xinput_pad_disconnect(port); devices_unlock(); return result; }
bool xinput_devices_synthetic_pad_enabled(void)
{ devices_lock(); bool result = locked_xinput_devices_synthetic_pad_enabled(); devices_unlock(); return result; }
size_t xinput_devices_register_pad(void)
{ devices_lock(); size_t result = locked_xinput_devices_register_pad(); devices_unlock(); return result; }
uint32_t xinput_pad_open(uint32_t type, uint32_t port, uint32_t slot, uint32_t polling)
{ devices_lock(); uint32_t result = locked_xinput_pad_open(type, port, slot, polling); devices_unlock(); return result; }
void xinput_pad_remove(void)
{ devices_lock(); locked_xinput_pad_remove(); devices_unlock(); }
void xinput_pad_insert(void)
{ devices_lock(); locked_xinput_pad_insert(); devices_unlock(); }
void xinput_pad_remove_after_polls(uint32_t polls)
{ devices_lock(); locked_xinput_pad_remove_after_polls(polls); devices_unlock(); }
void xinput_pad_close(uint32_t handle)
{ devices_lock(); locked_xinput_pad_close(handle); devices_unlock(); }
uint32_t xinput_pad_capabilities(uint32_t handle, uint32_t out)
{ devices_lock(); uint32_t result = locked_xinput_pad_capabilities(handle, out); devices_unlock(); return result; }
uint32_t xinput_pad_state_read(uint32_t handle, uint32_t out)
{ devices_lock(); uint32_t result = locked_xinput_pad_state_read(handle, out); devices_unlock(); return result; }
uint32_t xinput_pad_feedback(uint32_t handle, uint32_t feedback)
{ devices_lock(); uint32_t result = locked_xinput_pad_feedback(handle, feedback); devices_unlock(); return result; }
bool xinput_pad_last_output(uint8_t out[6])
{ devices_lock(); bool result = locked_xinput_pad_last_output(out); devices_unlock(); return result; }
uint64_t xinput_pad_output_count(void)
{ devices_lock(); uint64_t result = locked_xinput_pad_output_count(); devices_unlock(); return result; }
void xinput_devices_set_fatal(xinput_devices_fatal_fn handler)
{ devices_lock(); locked_xinput_devices_set_fatal(handler); devices_unlock(); }

void xinput_devices_run_locked(void (*job)(void *), void *user)
{
    devices_lock();
    if (job != NULL) job(user);
    devices_unlock();
}

/* Explicit host attachment policy; original title 002483A..002487B supplies
 * the eight bits. Device serialization precedes the MU lifecycle mutex. */
void xinput_devices_set_mu_mask(uint32_t mask)
{
    devices_lock();
    if ((mask & ~0x000F000Fu) != 0u) refuse(INIT_ENTRY, "invalid MU presence mask");
    const uint32_t changed = mu_presence ^ mask;
    mu_presence = mask;
    if (ready && changed != 0u) {
        uint32_t state[3];
        void *table = table_at(INIT_ENTRY, 0x0046C6E0u);
        memcpy(state, table, sizeof(state));
        state[0] = mask;
        state[1] |= changed; /* Preserve remove/reinsert until guest consumption. */
        memcpy(table, state, sizeof(state));
    }
    devices_unlock();
}
