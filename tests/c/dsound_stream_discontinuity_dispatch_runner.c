/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real xdk_thunk indirect dispatch. TLS/boundary-readiness are explicit fixture
 * inputs: this does not certify a generated full-host build's compiled stops. */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#include "host_runtime.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_surface.h"
#include "xdk_thunk.h"
#include <sys/mman.h>
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
static uint32_t missing_stop, missing_dispatch;
static bool irql(uint8_t *out) { *out = 0u; return true; }
static void fatal(uint32_t address, const char *reason)
{ host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, reason); }
int recomp_has_stop_boundary(uint32_t address)
{
    static const uint32_t stops[] = {0x384859u, 0x40686Cu, 0x406879u, 0x40688Au,
        0x40723Fu, 0x407286u, 0x4072D4u, 0x40733Bu, 0x407388u, 0x4073D3u,
        0x407424u, 0x407883u, 0x40788Du, 0x4093ADu};
    if (address == missing_stop) return 0;
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++)
        if (address == stops[i]) return 1;
    return 0;
}
int recomp_has_dispatch_boundary(uint32_t address)
{
    if (address == missing_dispatch) return 0;
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++)
        if (address == xdk_surface[i].address &&
            xdk_module_for_section(xdk_surface[i].section) == XDK_MODULE_DSOUND) return 1;
    return 0;
}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV); dsound_hle_init();
    map_fixed(0x412000u, 4096u); map_fixed(0x4A1000u, 4096u); map_fixed(0x4B9000u, 4096u);
    for (unsigned i = 0; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    const uint32_t device = load(SCRATCH_DATA) - 8u;
    const uint32_t desc = SCRATCH_DATA + 256u, format = SCRATCH_DATA + 320u;
    const uint32_t descriptor[6] = {16u, 3u, format, 0u, 0u, 0u};
    const uint16_t first[2] = {0x69u, 1u}, last[4] = {36u, 4u, 2u, 64u};
    const uint32_t rates[2] = {44100u, 24806u}; uint8_t fmt[20];
    memcpy(fmt, first, 4u); memcpy(fmt + 4u, rates, 8u); memcpy(fmt + 12u, last, 8u);
    CHECK(kernel_guest_write_bytes(desc, descriptor, sizeof(descriptor)));
    CHECK(kernel_guest_write_bytes(format, fmt, sizeof(fmt)));
    dsound_stream_set_enabled(true); dsound_stream_set_irql_provider(irql);
    dsound_stream_set_fatal(fatal);
    CHECK_EQ_U32(dsound_stream_create(desc, SCRATCH_DATA + 128u), 0u);
    const uint32_t stream = load(SCRATCH_DATA + 128u), params = SCRATCH_DATA + 512u;
    const uint32_t parameters[9] = {0, 0, 0xFFFFF448u, 0, 0, 0, 0, 0, 0};
    const uint32_t curve[4] = {0x3F000000u, 0x3E800000u, 0x3E000000u, 0};
    CHECK(kernel_guest_write_bytes(params, parameters, sizeof(parameters)));
    CHECK(kernel_guest_write_bytes(0x4B914Cu, curve, sizeof(curve)));
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream, params, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream, 0x3F800000u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream, 0x4B914Cu, 4u, 0u), 0u);
    xdk_dispatch_entry rows[XDK_SURFACE_COUNT];
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++)
        rows[i] = (xdk_dispatch_entry){xdk_surface[i].address, xdk_surface[i].name,
                                      xdk_module_for_section(xdk_surface[i].section)};
    CHECK(xdk_thunk_init(rows, XDK_SURFACE_COUNT));
    CHECK(recomp_lookup_manual(0x40733Bu) == NULL);
    missing_stop = 0x40733Bu;
    CHECK(!xdk_thunk_set_stream_virtual_handler(dsound_stream_cache_discontinuity));
    CHECK(recomp_lookup_manual(0x40733Bu) == NULL);
    missing_stop = 0; missing_dispatch = 0x40967Cu;
    CHECK(!xdk_thunk_set_stream_virtual_handler(dsound_stream_cache_discontinuity));
    CHECK(recomp_lookup_manual(0x40733Bu) == NULL);
    missing_dispatch = 0;
    CHECK(xdk_thunk_set_stream_virtual_handler(dsound_stream_cache_discontinuity));
    uint8_t header[40], parent[44];
    CHECK(kernel_guest_read_bytes(stream, header, sizeof(header)));
    CHECK(kernel_guest_read_bytes(device, parent, sizeof(parent)));
    const uint32_t stack = SCRATCH_DATA + 0x700u;
    for (unsigned phase = 0; phase < 8u; phase++) {
        if (phase == 1) CHECK_EQ_U32(dsound_stream_cache_pause(stream, 1u), 0u);
        if (phase == 2) CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream, 0u, 0u, 1u), 0u);
        if (phase == 6) { dsound_stream_set_completion(true, NULL); CHECK_EQ_U32(dsound_stream_cache_pause(stream, 0u), 0u); } /* T1198: a resumed stream, the pump caller */
        const bool success = phase == 2 || phase == 3 || phase == 6;
        const uint32_t words[4] = {0xAABBCCDDu, phase == 4 ? 0x29B73u : phase == 6 ? 0x2A1A5u : phase == 7 ? 0x2A1A6u : 0x29B72u,
                                  phase == 5 ? stream + 4u : stream, 0xBBCCDDEEu};
        CHECK(kernel_guest_write_bytes(stack - 4u, words, sizeof(words)));
        dsound_stream_snapshot before; CHECK(dsound_stream_get_snapshot(stream, &before));
        g_eax = 1; g_ecx = 2; g_edx = 3; g_ebx = 4; g_esi = 5; g_edi = 6; g_ebp = 7;
        g_fs_base = 8; g_esp = stack; thunk_trace_reset();
        recomp_func_t fn = recomp_lookup_manual(0x40733Bu); CHECK(fn != NULL);
        bool stopped = false;
        if (sigsetjmp(*host_run_jmp(), 1) == 0) { host_run_arm(); fn(); }
        else stopped = true;
        host_run_disarm(); CHECK(stopped == !success);
        CHECK_EQ_U32(g_eax, success ? 0u : 1u); CHECK_EQ_U32(g_esp, success ? stack + 8u : stack);
        if (stopped) CHECK(host_run_result()->guest_address == 0x40733Bu);
        CHECK(g_ecx == 2 && g_edx == 3 && g_ebx == 4 && g_esi == 5 && g_edi == 6 && g_ebp == 7 && g_fs_base == 8);
        uint32_t after_words[4]; CHECK(kernel_guest_read_bytes(stack - 4u, after_words, sizeof(after_words)));
        CHECK(memcmp(words, after_words, sizeof(words)) == 0);
        dsound_stream_snapshot after; CHECK(dsound_stream_get_snapshot(stream, &after));
        if (success) before.discontinuity_seen = true;
        CHECK(memcmp(&before, &after, sizeof(after)) == 0);
        uint8_t actual[44]; CHECK(kernel_guest_read_bytes(stream, actual, 40u));
        CHECK(memcmp(actual, header, 40u) == 0); CHECK(kernel_guest_read_bytes(device, actual, 44u));
        CHECK(memcmp(actual, parent, 44u) == 0);
        size_t count; const thunk_trace_entry *trace = thunk_trace_entries(&count);
        CHECK(count == 1u && trace[0].address == 0x40733Bu && trace[0].return_address == words[1]);
        CHECK(trace[0].implemented && trace[0].result_known == success);
        CHECK(xdk_thunk_stream_virtual_pending_count() == 0u && xdk_thunk_stream_virtual_active_count() == 0u);
    }
    CHECK(xdk_thunk_set_stream_virtual_handler(NULL)); CHECK(recomp_lookup_manual(0x40733Bu) == NULL);
    CHECK(dsound_stream_reset_checked()); CHECK(dsound_device_reset_checked());
    xdk_thunk_shutdown(); environment_end();
    printf("spatial Discontinuity indirect dispatch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
