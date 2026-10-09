/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_vblank_effects.h for the model, the refusals and what is left out.
 */
#include "d3d8_vblank_effects.h"

#include "d3d8_flip.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"

#include <stdio.h>
#include <string.h>

#define HELPER 0x003DC2E0u
#define MODE_FIELD_OBSERVABLE 0x01200000u
#define MODE_FIELD_PORT_READ_OBSERVABLE 0x00200000u

static bool enabled;
static uint64_t applied;
static d3d8_vblank_record last_record;
static d3d8_vblank_input last_input;

void d3d8_vblank_effects_configure(bool enable)
{
    d3d8_flip_lock();
    enabled = enable;
    applied = 0u;
    last_record = (d3d8_vblank_record){0u, 0u, 0u};
    memset(&last_input, 0, sizeof last_input);
    d3d8_flip_unlock();
}

bool d3d8_vblank_effects_enabled(void)
{
    d3d8_flip_lock();
    const bool value = enabled;
    d3d8_flip_unlock();
    return value;
}

uint64_t d3d8_vblank_effects_applied(void)
{
    d3d8_flip_lock();
    const uint64_t value = applied;
    d3d8_flip_unlock();
    return value;
}

d3d8_vblank_record d3d8_vblank_effects_last_record(void)
{
    d3d8_flip_lock();
    const d3d8_vblank_record value = last_record;
    d3d8_flip_unlock();
    return value;
}

d3d8_vblank_input d3d8_vblank_effects_last_input(void)
{
    d3d8_flip_lock();
    const d3d8_vblank_input value = last_input;
    d3d8_flip_unlock();
    return value;
}

void d3d8_vblank_effects_trace(char *out, size_t size)
{
    if (out == NULL || size == 0u) {
        return;
    }
    out[0] = '\0';
    if (!d3d8_flip_enabled() || !d3d8_vblank_effects_enabled()) {
        return;
    }
    d3d8_flip_lock();
    const d3d8_vblank_input in = last_input;
    const d3d8_vblank_record record = last_record;
    const uint32_t threshold = d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD);
    d3d8_flip_unlock();
    (void)snprintf(out, size,
                   " flip-in=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u flip-out=%u,%u,%u,%u",
                   in.callback, in.mode, in.display_start_off, in.consumer, in.count, in.threshold,
                   in.last_target, in.producer, in.slot[0], in.slot[1], in.slot[2], in.slot[3], in.slot[4],
                   in.slot[5], in.gamma_pending[0], in.gamma_pending[1], record.count, record.flip_index,
                   threshold, record.flags);
}

d3d8_vblank_record d3d8_vblank_effects_apply(uint32_t timestamp_low)
{
    d3d8_vblank_record record = {0u, 0u, 0u};
    const char *refusal = NULL;
    d3d8_flip_lock();

    const uint32_t mode = d3d8_device_load32(D3D8_VBLANK_DEV_MODE);
    if ((mode & MODE_FIELD_OBSERVABLE) == MODE_FIELD_PORT_READ_OBSERVABLE) {
        refusal = "the display mode lets GetDisplayFieldStatus observe the field status the "
                  "helper reads from port 0x80C0: hardware input, not modelled";
    }
    if (refusal == NULL) {
        last_input = (d3d8_vblank_input){
            d3d8_device_load32(D3D8_VBLANK_DEV_CALLBACK), mode, d3d8_device_load32(D3D8_FLIP_DEV_DISPLAY_START_OFF),
            d3d8_device_load32(D3D8_VBLANK_DEV_FLIP_INDEX), d3d8_device_load32(D3D8_VBLANK_DEV_COUNT),
            d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD), d3d8_device_load32(D3D8_FLIP_DEV_LAST_TARGET),
            d3d8_device_load32(D3D8_FLIP_DEV_PRODUCER),
            {d3d8_device_load32(D3D8_FLIP_DEV_SLOT0), d3d8_device_load32(D3D8_FLIP_DEV_SLOT0 + 4u),
             d3d8_device_load32(D3D8_FLIP_DEV_SLOT0 + 8u), d3d8_device_load32(D3D8_FLIP_DEV_SLOT1),
             d3d8_device_load32(D3D8_FLIP_DEV_SLOT1 + 4u), d3d8_device_load32(D3D8_FLIP_DEV_SLOT1 + 8u)},
            {d3d8_device_load32(D3D8_FLIP_DEV_GAMMA_PENDING), d3d8_device_load32(D3D8_FLIP_DEV_GAMMA_PENDING + 4u)}};
        const uint32_t last = d3d8_device_load32(D3D8_VBLANK_DEV_TIMESTAMP_LAST);
        if (last != 0u) {
            d3d8_device_store32(D3D8_VBLANK_DEV_TIMESTAMP_DELTA, timestamp_low - last);
        }
        d3d8_device_store32(D3D8_VBLANK_DEV_TIMESTAMP_LAST, timestamp_low);

        const uint32_t count = d3d8_device_load32(D3D8_VBLANK_DEV_COUNT) + 1u;
        d3d8_device_store32(D3D8_VBLANK_DEV_COUNT, count);

        /* 0x003DC200 (T407): a processed flip gives flags 1 and no threshold update. The
         * record carries the consumer index AFTER the flips, as the original builds it. */
        const unsigned flips = d3d8_flip_process_locked();
        record.count = count;
        record.flip_index = d3d8_device_load32(D3D8_VBLANK_DEV_FLIP_INDEX);
        if (flips != 0u) {
            record.flags = 1u;
        } else {
            const uint32_t threshold = d3d8_device_load32(D3D8_VBLANK_DEV_THRESHOLD);
            if (count == threshold) {
                d3d8_device_store32(D3D8_VBLANK_DEV_THRESHOLD, threshold + 1u);
                record.flags = 2u;
            }
        }
        applied++;
        last_record = record;
    }
    d3d8_flip_unlock();
    if (refusal != NULL) {
        d3d8_hle_fatal(HELPER, "vblank helper effects: %s", refusal);
    }
    return record;
}
