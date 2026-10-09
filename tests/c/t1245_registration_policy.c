/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1245 registration-policy fixture for the gpu/audio/input static audit.
 * This is a constructed policy snapshot, not retail acceptance.
 */
#include <stdio.h>
#include <string.h>

#include "d3d8_device.h"
#include "d3d8_hle.h"
#include "d3d8_surface_adapter.h"
#include "d3d8_target_query.h"
#include "dsound_hle.h"
#include "dsound_device.h"
#include "dsound_stream.h"
#include "dsound_buffer.h"
#include "dsound_listener.h"
#include "dsound_effects_binding.h"
#include "dsound_hrtf.h"
#include "dsound_completion.h"
#include "dsound_mixbin_headroom.h"
#include "recomp_abi.h"
#include "xdk_registration_report.h"
#include "xdk_surface.h"
#include "xinput_hle.h"
#include "xinput_devices.h"
#include "xvoice_media.h"
#include "mu_device.h"
#include "xgrph_hle.h"
#include "xgrph_object_lifetime.h"
#include "xgrph_shader_query.h"
#include "xgrph_texture.h"
#include "xgrph_swizzle.h"
#include "xnet_hle.h"
#include "xonline_hle.h"
#include "xonline_offline.h"
#include "xnet_offline.h"
#include "xnet_random.h"

/* Registration-only fixture using the live runtime control word and real completion
 * consumers. No guest handler is called, no packet/state is fabricated, and the
 * report makes no full-implementation or retail-admission claim. */
static bool control_word(uint16_t *out)
{
    *out = g_fp_control_word;
    return true;
}
int main(void)
{
    dsound_hle_init();
    dsound_listener_reset();
    dsound_buffer_reset();
    dsound_stream_reset();
    dsound_effects_binding_reset();
    dsound_device_reset();
    (void)dsound_device_register();
    (void)dsound_device_register_movie();
    (void)dsound_hrtf_register();
    dsound_effects_binding_set_enabled(true);
    if (!dsound_effects_binding_set_gp_enabled(true)) {
        fputs("registration fixture: compiled GP policy unavailable\n", stderr);
        return 1;
    }
    (void)dsound_effects_binding_register();
    dsound_completion_set_enabled(true);
    dsound_buffer_set_enabled(true);
    dsound_buffer_set_completion(true, dsound_completion_buffer_started);
    (void)dsound_completion_register();
    dsound_stream_set_enabled(true);
    dsound_stream_set_routing_note(dsound_completion_stream_routing);
    dsound_stream_set_frequency_note(dsound_completion_note_frequency, control_word);
    dsound_mixbin_headroom_configure(dsound_completion_mixbin_headroom,
        dsound_completion_bind_mixbin_headroom, NULL, NULL);
    (void)dsound_mixbin_headroom_register();
    (void)dsound_stream_register();
    (void)dsound_buffer_register();
    (void)dsound_listener_register();
    xinput_hle_init();
    xinput_devices_reset();
    xinput_devices_enable_synthetic_pad(true);
    (void)xinput_devices_register();
    (void)xinput_devices_register_pad();
    (void)xvoice_media_register(); /* T1086: registered partial, disconnected-device path only. */
    (void)mu_register();
    /* Distinct struct types are copied field by field, without aliasing casts. */
    d3d8_xdk_row rows[XDK_SURFACE_COUNT];
    for (size_t i = 0u; i < XDK_SURFACE_COUNT; i++) {
        rows[i].address = xdk_surface[i].address;
        rows[i].section = xdk_surface[i].section;
        rows[i].name = xdk_surface[i].name;
        rows[i].sites = xdk_surface[i].sites;
    }
    if (!d3d8_surface_adopt(rows, XDK_SURFACE_COUNT, D3D8_SECTION_D3D)) {
        fputs("xdk_report: could not adopt the measured D3D surface\n", stderr);
        return 1;
    }
    (void)d3d8_device_register();
    (void)d3d8_target_query_register();
    if (!xgrph_hle_adopt((const xgrph_xdk_row *)xdk_surface, XDK_SURFACE_COUNT)) {
        fputs("xdk_report: could not adopt the measured XGRPH surface\n", stderr);
        d3d8_hle_shutdown();
        return 1;
    }
    (void)xgrph_texture_register();
    (void)xgrph_object_lifetime_register();
    (void)xgrph_swizzle_register();
    (void)xgrph_shader_query_register();
    if (!xnet_hle_adopt((const xnet_xdk_row *)xdk_surface, XDK_SURFACE_COUNT)) {
        fputs("xdk_report: could not adopt the measured XNET surface\n", stderr);
        d3d8_hle_shutdown();
        xgrph_hle_shutdown();
        return 1;
    }
    /* Networking is outside the audited gpu/audio/input source scope. */
    /* T1071: XONLINE handlers exist only under the opt-in --xonline-offline policy. */
    if (!xonline_hle_adopt((const xonline_xdk_row *)xdk_surface, XDK_SURFACE_COUNT)) {
        fputs("xdk_report: could not adopt the measured XONLINE surface\n", stderr);
        d3d8_hle_shutdown();
        xgrph_hle_shutdown();
        xnet_hle_shutdown();
        return 1;
    }
    xonline_offline_reset();
    const bool okay = xdk_registration_report_write(
        stdout, (const xdk_registration_row *)xdk_surface, XDK_SURFACE_COUNT);
    d3d8_hle_shutdown();
    xgrph_hle_shutdown();
    xnet_hle_shutdown();
    xonline_hle_shutdown();
    if (!okay) {
        fputs("xdk_report: surface/registry mismatch or output failure\n", stderr);
        return 1;
    }
    return 0;
}
