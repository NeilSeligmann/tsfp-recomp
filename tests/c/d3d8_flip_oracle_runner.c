/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T407: replay runner for the flip queue and flip processing (src/gpu/d3d8_flip.c) against the
 * ORIGINAL bytes. The Python side runs 0x003DC200 and 0x003DC5E0 under tools.d3dscan.oracle and
 * hands this runner the same D3D region and the same per case seed words. The runner maps the
 * region at the oracle's address, restores it and applies the seeds before each case, runs the
 * port functions and reports every dword the case changed plus the hardware record.
 *
 * Input:  u32 case_count, the D3D region (0x30000 bytes), then per case
 *           u32 seed_count, {u32 address, u32 value} per seed,
 *           u32 step_count, {u32 kind, u32 data, u32 data2} per step (kind 0 process, 1 queue, 2 set the count,
 *           3 the Swap flip data word for front address `data` and present interval `data2`,
 *           returned in processed[]).
 * Output: per case
 *           u32 refused_step (0xFFFFFFFF none), u32 processed[step_count] (zero padded to 4),
 *           u32 changed_count, {u32 address, u32 value} per changed dword,
 *           the hardware record: u32 display_start, u32 display_start_writes, u32 gamma_uploads,
 *           u32 pgraph_increments, u32 flips, u32 queued, u32 event_count,
 *           {u32 kind, u32 value} x 64, u8 gamma[768].
 */
#include "test_d3d8_support.h"

#include "d3d8_flip.h"
#include "d3d8_gpu.h"

#define MAX_SEEDS 4096u
#define MAX_STEPS 16u
#define NO_REFUSAL 0xFFFFFFFFu

int main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    uint32_t case_count = 0u;
    static uint8_t base[D3D_REGION_BYTES];
    if (!in || !out || fread(&case_count, 4, 1, in) != 1 || fread(base, sizeof(base), 1, in) != 1) {
        return 3;
    }
    environment_begin(KERNEL_AV_PACK_HDTV);
    uint8_t *region = kernel_guest_at(D3D_REGION_BASE, D3D_REGION_BYTES);
    for (uint32_t index = 0u; index < case_count; index++) {
        uint32_t seeds[MAX_SEEDS][2];
        uint32_t steps[MAX_STEPS][3];
        uint32_t seed_count = 0u;
        uint32_t step_count = 0u;
        if (fread(&seed_count, 4, 1, in) != 1 || seed_count > MAX_SEEDS ||
            fread(seeds, sizeof(seeds[0]), seed_count, in) != seed_count ||
            fread(&step_count, 4, 1, in) != 1 || step_count > MAX_STEPS ||
            fread(steps, sizeof(steps[0]), step_count, in) != step_count) {
            return 3;
        }
        memcpy(region, base, sizeof(base));
        d3d8_flip_reset();
        for (uint32_t seed = 0u; seed < seed_count; seed++) {
            d3d8_guest_store32(seeds[seed][0], seeds[seed][1]);
        }
        uint32_t refused_step = NO_REFUSAL;
        uint32_t processed[MAX_STEPS] = {0u};
        for (uint32_t step = 0u; step < step_count; step++) {
            if (steps[step][0] == 3u) {
                processed[step] = d3d8_flip_swap_method_data(steps[step][1], steps[step][2]);
                continue;
            }
            if (steps[step][0] == 2u) {
                d3d8_device_store32(D3D8_FLIP_DEV_COUNT, steps[step][1]); /* blanks pass */
                continue;
            }
            d3d8_flip_lock();
            if (steps[step][0] == 0u) {
                processed[step] = d3d8_flip_process_locked();
            } else {
                const char *refusal = d3d8_flip_queue_refusal_locked(steps[step][1]);
                if (refusal != NULL) {
                    refused_step = step;
                    d3d8_flip_unlock();
                    break;
                }
                processed[step] = d3d8_flip_queue_locked(steps[step][1]);
            }
            d3d8_flip_unlock();
        }
        fwrite(&refused_step, 4, 1, out);
        fwrite(processed, sizeof(processed), 1, out);
        uint32_t changed[D3D_REGION_BYTES / 4u][2];
        uint32_t changed_count = 0u;
        for (uint32_t offset = 0u; offset < D3D_REGION_BYTES; offset += 4u) {
            uint32_t was, now;
            memcpy(&was, base + offset, 4);
            memcpy(&now, region + offset, 4);
            if (was != now) {
                changed[changed_count][0] = D3D_REGION_BASE + offset;
                changed[changed_count][1] = now;
                changed_count++;
            }
        }
        fwrite(&changed_count, 4, 1, out);
        fwrite(changed, sizeof(changed[0]), changed_count, out);
        const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
        const uint32_t summary[7] = {hardware.display_start,
                                     (uint32_t)hardware.display_start_writes,
                                     (uint32_t)hardware.gamma_uploads,
                                     (uint32_t)hardware.pgraph_increments,
                                     (uint32_t)hardware.flips,
                                     (uint32_t)hardware.queued,
                                     (uint32_t)hardware.event_count};
        fwrite(summary, sizeof(summary), 1, out);
        uint32_t events[D3D8_FLIP_HW_EVENTS][2] = {{0u}};
        for (size_t event = 0u; event < hardware.event_count; event++) {
            events[event][0] = hardware.events[event].kind;
            events[event][1] = hardware.events[event].value;
        }
        fwrite(events, sizeof(events), 1, out);
        fwrite(hardware.gamma, sizeof(hardware.gamma), 1, out);
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
