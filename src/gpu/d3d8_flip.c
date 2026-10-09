/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_flip.h for the model, what lands in guest memory and what is held as a record.
 */
#include "d3d8_flip.h"

#include <pthread.h>
#include <string.h>

#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_surface.h"
#include "d3d8_vblank_effects.h"

#define FLIP_PROCESSOR 0x003DC200u
#define FLIP_SOFTWARE_METHOD 0x003DC5E0u
#define SWAP_PREPARE 0x003D8B10u

#define DEV_FRONT_LIST 0x1A18u
#define GLOBAL_PRESENT_INTERVAL 0x003E3EBCu

#define MODE_BIT_FIELD_SELECT 0x01000000u
#define MODE_BIT_FIELD_PITCH 0x00200000u
#define MODE_FLAGS_TOP_MASK 0xC0000000u
#define MODE_FLAGS_TOP_FIELD 0x80000000u
#define PGRAPH_FLIP_READ_INCREMENT 2u
#define INTERVAL_SIGN_BIT 0x80000000u

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool model_enabled;
static d3d8_flip_hardware hardware;
static uint64_t seen_reset_count;
static bool seen_reset_valid;

void d3d8_flip_lock(void)
{
    pthread_mutex_lock(&lock);
}

void d3d8_flip_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

static void reset_locked(void)
{
    const uint64_t next_serial = hardware.reset_serial + 1u;
    memset(&hardware, 0, sizeof(hardware));
    hardware.reset_serial = next_serial;
}

void d3d8_flip_reset(void)
{
    pthread_mutex_lock(&lock);
    reset_locked();
    pthread_mutex_unlock(&lock);
}

d3d8_flip_hardware d3d8_flip_hardware_get(void)
{
    pthread_mutex_lock(&lock);
    const d3d8_flip_hardware copy = hardware;
    pthread_mutex_unlock(&lock);
    return copy;
}

/* The record belongs to one device: CreateDevice resets the GPU model and so starts it over. */
static void follow_device_locked(void)
{
    const uint64_t current = d3d8_gpu_reset_count();
    if (!seen_reset_valid || current != seen_reset_count) {
        reset_locked();
        seen_reset_count = current;
        seen_reset_valid = true;
    }
}

static void log_event_locked(d3d8_flip_hw_kind kind, uint32_t value)
{
    if (hardware.event_count < D3D8_FLIP_HW_EVENTS) {
        hardware.events[hardware.event_count].kind = (uint32_t)kind;
        hardware.events[hardware.event_count].value = value;
        hardware.event_count++;
    } else {
        hardware.events_dropped++;
    }
}

/* --- 0x003D99F3: the display start register ---------------------------------------------- */

static void program_display_start_locked(uint32_t value)
{
    const bool field_zero = d3d8_device_load32(D3D8_FLIP_DEV_FIELD_STATUS) == 0u;
    if (d3d8_device_load32(D3D8_FLIP_DEV_DISPLAY_START_OFF) != 0u) {
        return;
    }
    const uint32_t mode = d3d8_device_load32(D3D8_FLIP_DEV_DISPLAY_MODE);
    const uint32_t flags = d3d8_device_load32(D3D8_FLIP_DEV_MODE_WORD);
    const uint32_t pitch = d3d8_device_load32(D3D8_FLIP_DEV_PITCH);
    const bool top_is_field = (flags & MODE_FLAGS_TOP_MASK) == MODE_FLAGS_TOP_FIELD;
    uint32_t programmed = value;
    if ((mode & MODE_BIT_FIELD_SELECT) != 0u && !top_is_field && field_zero) {
        programmed = value - (pitch >> 1);
    } else if (top_is_field && (mode & MODE_BIT_FIELD_PITCH) != 0u && field_zero) {
        programmed = value - pitch;
    }
    hardware.display_start = programmed;
    hardware.display_start_writes++;
    log_event_locked(D3D8_FLIP_HW_DISPLAY_START, programmed);
}

/* --- 0x003D9A75: the gamma ramp upload --------------------------------------------------- */

static void upload_gamma_ramp_locked(uint32_t index)
{
    const uint32_t ramp = D3D8_FLIP_DEV_GAMMA_RAMPS + index * D3D8_FLIP_GAMMA_RAMP_BYTES;
    /* The original reads one byte of each channel per step (and a register every eighth step to
     * flush), writing the three to PRAMDAC 0x6813C9 in turn: red, green, blue interleaved. */
    for (uint32_t entry = 0u; entry < D3D8_FLIP_GAMMA_CHANNEL_BYTES; entry++) {
        for (uint32_t channel = 0u; channel < 3u; channel++) {
            const uint32_t address = ramp + channel * D3D8_FLIP_GAMMA_CHANNEL_BYTES + entry;
            const uint32_t word = d3d8_device_load32(address & ~3u);
            hardware.gamma[entry * 3u + channel] = (uint8_t)(word >> ((address & 3u) * 8u));
        }
    }
    hardware.gamma_uploads++;
    log_event_locked(D3D8_FLIP_HW_GAMMA_RAMP, index);
}

/* --- 0x003DC200: the flip processor ------------------------------------------------------ */

static uint32_t slot_address(uint32_t consumer)
{
    return D3D8_FLIP_DEV_SLOT0 + (consumer & 1u) * D3D8_FLIP_SLOT_BYTES;
}

unsigned d3d8_flip_process_locked(void)
{
    follow_device_locked();
    unsigned processed = 0u;
    uint32_t slot = slot_address(d3d8_device_load32(D3D8_FLIP_DEV_CONSUMER));
    while (d3d8_device_load32(slot) != 0u) {
        if (d3d8_device_load32(D3D8_FLIP_DEV_COUNT) != d3d8_device_load32(slot + 4u)) {
            break;
        }
        d3d8_device_store32(slot, 0u);
        const uint32_t value = d3d8_device_load32(slot + 8u);
        d3d8_guest_store32(D3D8_FLIP_GLOBAL_VALUE, value);
        program_display_start_locked(value);
        const uint32_t ramp = d3d8_device_load32(D3D8_FLIP_DEV_CONSUMER) & 1u;
        if (d3d8_device_load32(D3D8_FLIP_DEV_GAMMA_PENDING + ramp * 4u) == 1u) {
            upload_gamma_ramp_locked(ramp);
            d3d8_device_store32(D3D8_FLIP_DEV_GAMMA_PENDING + ramp * 4u, 0u);
        }
        hardware.pgraph_increments++;
        log_event_locked(D3D8_FLIP_HW_PGRAPH_INCREMENT, PGRAPH_FLIP_READ_INCREMENT);
        const uint32_t consumer = d3d8_device_load32(D3D8_FLIP_DEV_CONSUMER) + 1u;
        d3d8_device_store32(D3D8_FLIP_DEV_CONSUMER, consumer);
        slot = slot_address(consumer);
        hardware.flips++;
        processed++;
    }
    return processed;
}

/* --- 0x003DC5E0: the software method handler, type 1 ------------------------------------- */

const char *d3d8_flip_queue_refusal_locked(uint32_t method_data)
{
    /* Type 11 jumps directly to the return epilogue at 0x003DC713. It does
     * not inspect the flip callback or producer slot. */
    if ((method_data & 0x1Fu) == 11u) {
        return NULL;
    }
    if ((method_data & 0x1Fu) != D3D8_FLIP_METHOD_TYPE_FLIP) {
        return "a PGRAPH software method other than the flip (type 1) is not ported, the "
               "other jump table entries of 0x003DC5E0 touch the interrupt event, two "
               "callbacks and PGRAPH registers";
    }
    if (d3d8_device_load32(D3D8_FLIP_DEV_CALLBACK) != 0u) {
        return "a flip callback is set (+0x18C): the original calls it with a record built from "
               "rdtsc, not ported";
    }
    const uint32_t producer = d3d8_device_load32(D3D8_FLIP_DEV_PRODUCER);
    if (d3d8_device_load32(slot_address(producer)) != 0u) {
        return "the producer slot is still pending: on hardware FLIP_STALL (method 0x130) keeps "
               "the GPU from running this flip until the first was processed, the "
               "instantaneous GPU has no stall";
    }
    return NULL;
}

unsigned d3d8_flip_queue_locked(uint32_t method_data)
{
    /* The original type-11 software method has no queue or hardware effects. */
    if ((method_data & 0x1Fu) == 11u) {
        return 0u;
    }
    follow_device_locked();
    const uint32_t shifted = method_data >> 5;
    const uint32_t interval = shifted & 7u;
    const uint32_t address = shifted & 0xFFFFFFF0u;
    const uint32_t count = d3d8_device_load32(D3D8_FLIP_DEV_COUNT);

    uint32_t target = d3d8_device_load32(D3D8_FLIP_DEV_LAST_TARGET) + interval;
    if ((int32_t)(target - count) <= 0) {
        /* The slot the interval asks for has passed: show it at the next blank, or at once when
         * the immediate bit is set. */
        target = (shifted & D3D8_FLIP_INTERVAL_IMMEDIATE_BIT) != 0u ? count : count + 1u;
    }
    d3d8_device_store32(D3D8_FLIP_DEV_THRESHOLD, target + interval);
    d3d8_device_store32(D3D8_FLIP_DEV_LAST_TARGET, target);

    const uint32_t producer = d3d8_device_load32(D3D8_FLIP_DEV_PRODUCER);
    const uint32_t slot = slot_address(producer);
    d3d8_device_store32(slot, 1u);
    d3d8_device_store32(slot + 4u, target);
    d3d8_device_store32(slot + 8u, address);
    d3d8_device_store32(D3D8_FLIP_DEV_PRODUCER, producer + 1u);
    hardware.queued++;
    return d3d8_flip_process_locked();
}

/* --- The Swap's flip, 0x003D8B10 ---------------------------------------------------------- */

uint32_t d3d8_flip_swap_method_data(uint32_t front_data_address, uint32_t present_interval)
{
    uint32_t bits = front_data_address;
    uint32_t interval = present_interval == 0u ? 1u : present_interval;
    if ((interval & 1u) != 0u) {
        bits |= 1u;
    }
    if ((interval & 2u) != 0u) {
        bits |= 2u;
    }
    if ((interval & 4u) != 0u) {
        bits |= 3u;
    }
    if ((interval & INTERVAL_SIGN_BIT) != 0u) {
        bits |= D3D8_FLIP_INTERVAL_IMMEDIATE_BIT;
    }
    return (bits << 5) | D3D8_FLIP_METHOD_TYPE_FLIP;
}

void d3d8_flip_configure(bool enabled)
{
    pthread_mutex_lock(&lock);
    model_enabled = enabled;
    pthread_mutex_unlock(&lock);
}

bool d3d8_flip_enabled(void)
{
    pthread_mutex_lock(&lock);
    const bool value = model_enabled;
    pthread_mutex_unlock(&lock);
    return value;
}

void d3d8_flip_queue_for_swap(void)
{
    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {
        return;
    }
    const uint32_t front = d3d8_device_load32(DEV_FRONT_LIST);
    const uint32_t data = d3d8_flip_swap_method_data(
        d3d8_guest_load32(front + D3D8_SURFACE_DATA), d3d8_guest_load32(GLOBAL_PRESENT_INTERVAL));
    d3d8_flip_lock();
    const char *refusal = d3d8_flip_queue_refusal_locked(data);
    if (refusal == NULL) {
        (void)d3d8_flip_queue_locked(data);
    }
    d3d8_flip_unlock();
    if (refusal != NULL) {
        d3d8_hle_fatal(FLIP_SOFTWARE_METHOD, "flip queue: %s", refusal);
    }
    d3d8_hle_note_unmodelled(
        SWAP_PREPARE,
        "the flip's own commands (WAIT_FOR_IDLE, the software NOP, FLIP_INCREMENT_WRITE, "
        "FLIP_STALL) are not written and the flip is queued by the model at this point, its "
        "display start, gamma ramp and PGRAPH increment go to d3d8_flip_hardware, not to a "
        "register file");
}
