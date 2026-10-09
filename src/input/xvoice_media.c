/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xvoice_media.h"

#include <stdlib.h>

#include "kernel_call.h"
#include "kernel_hle.h"
#include "xinput_hle.h"

#define XVOICE_CREATE_EX 0x004754FDu
#define XVOICE_MICROPHONE_TABLE 0x0046C894u
#define XVOICE_HEADPHONE_TABLE 0x0046C8A0u
#define XVOICE_TITLE_CALLBACK 0x00387674u
#define XVOICE_DISCONNECTED_HRESULT 0x8007048Fu

static uint32_t argument(const kernel_call_frame *frame, unsigned index)
{
    uint32_t value;
    if (!kernel_frame_arg(frame, index, &value)) {
        kernel_hle_fatal(XVOICE_CREATE_EX, "unreadable stdcall argument %u", index);
        abort();
    }
    return value;
}

static void refuse(const char *reason)
{
    kernel_hle_fatal(XVOICE_CREATE_EX, "disconnected-only voice handler refused: %s", reason);
    abort();
}

static bool measured_title_rate(uint32_t rate)
{
    switch (rate) {
    case 8000u:
    case 11025u:
    case 16000u:
    case 22050u:
    case 24000u:
    case 32000u:
    case 44100u:
    case 48000u:
        return true;
    default:
        return false;
    }
}

uint32_t xvoice_media_create_ex(void *context)
{
    const kernel_call_frame *frame = context;
    const uint32_t type_table = argument(frame, 0u);
    const uint32_t port = argument(frame, 1u);
    const uint32_t packet_count = argument(frame, 2u);
    const uint32_t format = argument(frame, 3u);
    const uint32_t callback = argument(frame, 4u);
    const uint32_t callback_context = argument(frame, 5u);
    const uint32_t output = argument(frame, 6u);
    uint32_t sample_rate;

    if (type_table != XVOICE_MICROPHONE_TABLE && type_table != XVOICE_HEADPHONE_TABLE) {
        refuse("type table is outside the two measured title callers");
    }
    if (port >= 4u || packet_count != 2u) {
        refuse("port/packet count is outside measured title calls");
    }
    if (format == 0u || format > UINT32_MAX - 4u ||
        !kernel_guest_read_u32(format + 4u, &sample_rate) || !measured_title_rate(sample_rate)) {
        refuse("format/sample rate is outside original-oracle controls");
    }
    if (callback != XVOICE_TITLE_CALLBACK || callback_context == 0u || output == 0u) {
        refuse("callback/context/output is outside measured title call shape");
    }
    if (!kernel_guest_write_u32(output, 0u)) {
        refuse("output pointer is not writable");
    }

    /* No host path can currently attach an isochronous voice endpoint. The
     * original XBE returns this HRESULT, clears the output and leaves its slot
     * empty in the matching no-voice case. Do not publish a fake object. */
    return XVOICE_DISCONNECTED_HRESULT;
}

size_t xvoice_media_register(void)
{
    return xinput_hle_register(XVOICE_CREATE_EX, xvoice_media_create_ex) ? 1u : 0u;
}
