/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replay runner for the TRANSPLANTED multi-step case fixtures (T382, salvaged from the
 * scratch/d3d-frame dual-run harness): the Python side runs the ORIGINAL bytes from the state
 * CreateDevice leaves under tools.d3dscan.oracle, captures every guest window the case touches
 * (the D3D region, the fence control block page, the second-history page, the arena of
 * case-built headers), and hands this runner the same windows. The runner maps them at the
 * oracle's own addresses, so both sides share ONE address space and the comparison is
 * byte-for-byte with no pointer normalisation. Each step is the C port function the registered
 * handler calls, invoked directly in the scissor-runner style.
 *
 * Addresses this runner serves (tests/test_d3d8_present_oracle.py,
 * tests/test_d3d8_gpu_fence_oracle.py, tests/test_d3d8_bind_oracle.py):
 *   0x003D8E50 Swap            0x003D8B10 present prepare   0x003D8890 present save
 *   0x003D8920 present restore 0x003D8E10 present finish    0x003D8450 present set-mode
 *   0x003D67B0 fence insert    0x003D6870 fence wait        0x003D6690 kick
 *   0x003D3550 wait vblank     0x003D3800 SetRenderTarget   0x003D4070 SetTexture
 *   0x003D7220 render-target flag
 *   0x003DC2E0 vblank helper (T372): the device-state effects, args[0] the timestamp low 32 bits
 *   0x003D9990 EnableOverlay   0x003D9810 UpdateOverlay (5 arguments)   0x003D97F0 overlay status
 *              (T393 part B, tests/test_d3d8_overlay_oracle.py)
 *
 * Input:  u32 step_count, then per step {address, argc, args[5]};
 *         u32 segment_count, then per segment {base, bytes}; then the segments' raw bytes;
 *         then u32 preset_count and per preset {offset, value} for the overlay shadow, which the
 *         hardware owns (an absent preset block counts as none).
 * Output: u32 results[step_count] (0 for the void entries), u32 fatal_step (0xFFFFFFFF when no
 *         step stopped through the guarded fatal hook), then the segments' raw bytes back, then
 *         the overlay shadow: u32 write-log count, 64 {offset, value} log pairs (zero padded) and
 *         the shadowed dwords from offset 0x8100 up to and including 0x8B00.
 */
#include "test_d3d8_support.h"

#include "d3d8_bind.h"
#include "d3d8_gpu.h"
#include "d3d8_overlay.h"
#include "d3d8_present.h"
#include "d3d8_vblank_effects.h"

#define MAX_STEPS 8u
#define MAX_SEGMENTS 8u
#define NO_FATAL 0xFFFFFFFFu
#define STEP_WORDS 7u
#define STEP_ARGS 5u
#define SHADOW_DWORDS ((D3D8_OVERLAY_LAST_REGISTER - D3D8_OVERLAY_FIRST_REGISTER) / 4u + 1u)

static uint32_t run_step(uint32_t address, const uint32_t *args)
{
    switch (address) {
    case D3D8_OVERLAY_ENABLE:
        return d3d8_overlay_enable();
    case D3D8_OVERLAY_UPDATE:
        return d3d8_overlay_update(args[0], args[1], args[2], args[3], args[4]);
    case D3D8_OVERLAY_STATUS:
        return d3d8_overlay_update_status();
    case 0x003D8E50u:
        return d3d8_swap(args[0]);
    case 0x003D8B10u:
        d3d8_present_prepare(args[0]);
        return 0u;
    case 0x003D8890u:
        d3d8_present_save_state();
        return 0u;
    case 0x003D8920u:
        d3d8_present_restore_state();
        return 0u;
    case 0x003D8E10u:
        d3d8_present_finish();
        return 0u;
    case 0x003D8450u:
        d3d8_present_set_mode();
        return 0u;
    case 0x003D67B0u:
        return d3d8_gpu_fence_insert(args[0]);
    case 0x003D6870u:
        d3d8_gpu_fence_wait(args[0], args[1]);
        return 0u;
    case 0x003D6690u:
        d3d8_gpu_kick();
        return 0u;
    case 0x003D3550u:
        d3d8_gpu_wait_vblank();
        return 0u;
    case 0x003D3800u:
        d3d8_set_render_target(args[0], args[1]);
        return 0u;
    case 0x003D4070u:
        d3d8_set_texture(args[0], args[1]);
        return 0u;
    case 0x003D7220u:
        d3d8_set_render_target_flag(args[0]);
        return 0u;
    case 0x003DC2E0u:
        (void)d3d8_vblank_effects_apply(args[0]);
        return 0u;
    default:
        printf("FATAL unknown case address %#x\n", (unsigned)address);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t step_count = 0u;
    uint32_t steps[MAX_STEPS][STEP_WORDS];
    uint32_t segment_count = 0u;
    uint32_t segments[MAX_SEGMENTS][2];
    static uint8_t bytes[MAX_SEGMENTS][0x30000];
    if (!in || !out || fread(&step_count, 4, 1, in) != 1 || step_count == 0u ||
        step_count > MAX_STEPS) {
        return 3;
    }
    for (uint32_t step = 0u; step < step_count; step++) {
        if (fread(steps[step], sizeof(steps[step]), 1, in) != 1) {
            return 3;
        }
    }
    if (fread(&segment_count, 4, 1, in) != 1 || segment_count == 0u ||
        segment_count > MAX_SEGMENTS) {
        return 3;
    }
    for (uint32_t index = 0u; index < segment_count; index++) {
        if (fread(segments[index], sizeof(segments[index]), 1, in) != 1 ||
            segments[index][1] > sizeof(bytes[index])) {
            return 3;
        }
    }
    for (uint32_t index = 0u; index < segment_count; index++) {
        if (fread(bytes[index], segments[index][1], 1, in) != 1) {
            return 3;
        }
    }

    uint32_t preset_count = 0u;
    uint32_t presets[16][2];
    if (fread(&preset_count, 4, 1, in) != 1) {
        preset_count = 0u;
    }
    if (preset_count > sizeof(presets) / sizeof(presets[0]) ||
        (preset_count != 0u && fread(presets, sizeof(presets[0]), preset_count, in) != preset_count)) {
        return 3;
    }

    environment_begin(KERNEL_AV_PACK_HDTV);
    d3d8_overlay_reset();
    for (uint32_t index = 0u; index < preset_count; index++) {
        if (!d3d8_overlay_hardware_write(presets[index][0], presets[index][1])) {
            return 3;
        }
    }
    for (uint32_t index = 0u; index < segment_count; index++) {
        const uint32_t base = segments[index][0];
        const uint32_t size = segments[index][1];
        /* The D3D and .rdata regions are already mapped by environment_begin. */
        if (!(base >= D3D_REGION_BASE && base + size <= D3D_REGION_BASE + D3D_REGION_BYTES) &&
            !(base >= RDATA_REGION_BASE && base + size <= RDATA_REGION_BASE + RDATA_REGION_BYTES)) {
            map_fixed(base, size);
        }
        memcpy(kernel_guest_at(base, size), bytes[index], size);
    }

    uint32_t results[MAX_STEPS] = {0u};
    uint32_t fatal_step = NO_FATAL;
    for (uint32_t step = 0u; step < step_count; step++) {
        uint32_t result = 0u;
        RUN_EXPECTING_FATAL(result = run_step(steps[step][0], &steps[step][2]));
        if (fatal_seen) {
            fatal_step = step;
            fprintf(stderr, "native fatal at %#x in step %u: %s\n", (unsigned)fatal_address, (unsigned)step,
                    fatal_text);
            break;
        }
        results[step] = result;
    }

    if (fwrite(results, sizeof(uint32_t), step_count, out) != step_count ||
        fwrite(&fatal_step, 4, 1, out) != 1) {
        return 4;
    }
    for (uint32_t index = 0u; index < segment_count; index++) {
        if (fwrite(kernel_guest_at(segments[index][0], segments[index][1]), segments[index][1], 1,
                   out) != 1) {
            return 4;
        }
    }
    d3d8_overlay_write log[D3D8_OVERLAY_WRITE_LOG_CAPACITY];
    memset(log, 0, sizeof(log));
    const uint32_t log_count = (uint32_t)d3d8_overlay_write_log(log, D3D8_OVERLAY_WRITE_LOG_CAPACITY);
    uint32_t shadow[SHADOW_DWORDS];
    for (uint32_t index = 0u; index < SHADOW_DWORDS; index++) {
        (void)d3d8_overlay_register_value(D3D8_OVERLAY_FIRST_REGISTER + index * 4u, &shadow[index]);
    }
    if (fwrite(&log_count, 4, 1, out) != 1 || fwrite(log, sizeof(log), 1, out) != 1 ||
        fwrite(shadow, sizeof(shadow), 1, out) != 1) {
        return 4;
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
