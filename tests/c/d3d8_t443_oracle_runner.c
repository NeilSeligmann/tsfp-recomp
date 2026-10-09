/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Differential runner for the T443 copy composition pieces (tests/d3d8_t443_case.py). The Python
 * side runs the ORIGINAL bytes under tools.d3dscan.oracle from a seeded device and hands this runner
 * the same guest windows (the D3D region, .rdata, the pushbuffer ring, the game's data page and any
 * arena of case-built structures) at the oracle's own addresses, so both sides share one address
 * space and the comparison is byte for byte with no pointer normalisation. UNLIKE the replay case
 * runner (d3d8_replay_case_oracle_runner.c) the ring cursor, limit and every written command are
 * compared too, because the pieces here emit commands exactly as the original does.
 *
 * Input:  u32 step_count, then per step {address, arg_count, args[5]},
 *         u32 window_count, per window {base, bytes}, then the windows' raw bytes.
 * Output: u32 results[step_count], u32 fatal_step (0xFFFFFFFF when none stopped through the guarded
 *         fatal hook), u32 fatal_address, u32 roll_count, u32 roll_cursors[roll_count] (the cursor at
 *         each ring roll-over, at most 64), then the windows' raw bytes back.
 */
#include "test_d3d8_support.h"

#include "d3d8_combiner.h"
#include "d3d8_copy.h"
#include "d3d8_dirty.h"
#include "d3d8_immediate.h"
#include "d3d8_matrix_inverse.h"
#include "d3d8_present.h"
#include "d3d8_resource.h"
#include "d3d8_set_state.h"
#include "d3d8_shader.h"
#include "d3d8_state.h"

#define MAX_STEPS 512u
#define MAX_WINDOWS 160u
#define MAX_ROLLS 64u
#define NO_FATAL 0xFFFFFFFFu
#define STEP_ARGS 5u

static uint32_t roll_cursors[MAX_ROLLS];
static uint32_t roll_count;

static void log_roll(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    (void)begin;
    if (roll_count < MAX_ROLLS) {
        roll_cursors[roll_count] = end;
    }
    roll_count++;
}

/* The 0x170 byte frame the swap keeps on its stack lives in guest memory here, so the window
 * comparison covers it. */
static uint32_t *guest_frame(uint32_t address)
{
    uint32_t *frame = kernel_guest_at(address, D3D8_COPY_FRAME_DWORDS * 4u);
    if (frame == NULL) {
        printf("FATAL frame %#x is not mapped\n", (unsigned)address);
        exit(EXIT_FAILURE);
    }
    return frame;
}

/* Not an original address: the title's own handler for 0x003D81F0 (T598), dispatched through the D3D8 table with
 * a stdcall frame as xdk_thunk.c does, so the registered handler is what runs. */
#define TITLE_ENTRY_9A 0x003D81F1u

static uint32_t run_step(uint32_t address, const uint32_t *args)
{
    switch (address) {
    case TITLE_ENTRY_9A: {
        static bool registered;
        if (!registered) {
            (void)d3d8_state_register();
            registered = true;
        }
        return call_stdcall(0x003D81F0u, args, 1u);
    }
    case 0x003D6CC0u:
        d3d8_set_render_state_notinline((int32_t)args[0], args[1]);
        return 0u;
    case 0x003D7700u:
        d3d8_set_texture_stage_state_notinline(args[0], (int32_t)args[1], args[2]);
        return 0u;
    case 0x003D7500u:
        d3d8_set_texcoord_index(args[0], args[1]);
        return 0u;
    case 0x003D6C60u:
        d3d8_state_set_88(args[0]);
        return 0u;
    case 0x003D7380u:
        d3d8_state_set_8b(args[0]);
        return 0u;
    case 0x003D73D0u:
        d3d8_state_set_8c(args[0]);
        return 0u;
    case 0x003D7320u:
        d3d8_state_set_96(args[0]);
        return 0u;
    case 0x003D74B0u:
        d3d8_state_library_set_89(args[0]);
        return 0u;
    case 0x003D7010u:
        d3d8_state_library_set_8a(args[0]);
        return 0u;
    case 0x003D7430u:
        d3d8_state_library_set_8d(args[0]);
        return 0u;
    case 0x003D7110u:
        d3d8_state_library_set_8e(args[0]);
        return 0u;
    case 0x003D70D0u:
        d3d8_state_library_set_92(args[0]);
        return 0u;
    case 0x003D8250u:
        d3d8_state_library_set_98(args[0]);
        return 0u;
    case 0x003D6FD0u:
        d3d8_state_library_set_9c(args[0]);
        return 0u;
    case 0x003D71B0u:
        d3d8_state_library_set_9d(args[0]);
        return 0u;
    case 0x003D8320u:
        d3d8_state_library_set_9e(args[0]);
        return 0u;
    case 0x003D7220u:
        d3d8_state_library_set_9f(args[0]);
        return 0u;
    case 0x003D8120u:
        d3d8_state_library_set_a2(args[0]);
        return 0u;
    case 0x003D81D0u:
        d3d8_state_library_set_a5(args[0]);
        return 0u;
    case 0x003D7EE0u:
        d3d8_state_library_set_8f(args[0]);
        return 0u;
    case 0x003D7F70u:
        d3d8_state_library_set_90(args[0]);
        return 0u;
    case 0x003D8010u:
        d3d8_state_library_set_91(args[0]);
        return 0u;
    case 0x003D81F0u:
        d3d8_state_library_set_9a(args[0]);
        return 0u;
    case 0x003D80B0u:
        d3d8_state_library_set_a1(args[0]);
        return 0u;
    case 0x003D8190u:
        d3d8_state_library_set_a3(args[0]);
        return 0u;
    case 0x003D81B0u:
        d3d8_state_library_set_a4(args[0]);
        return 0u;
    case 0x003D7060u:
        d3d8_state_library_set_93(args[0]);
        return 0u;
    case 0x003D7150u:
        d3d8_state_library_set_94(args[0]);
        return 0u;
    case 0x003D72A0u:
        d3d8_state_library_set_95(args[0]);
        return 0u;
    case 0x003D92E0u:
        return d3d8_set_pixel_shader(args[0]);
    case 0x003D9320u:
        return d3d8_set_pixel_shader_v(args[0]);
    case 0x003DED80u:
        d3d8_run_dirty_cascade();
        return 0u;
    case 0x003E11D0u:
        return d3d8_emit_fixed_function_combiner(args[1]);
    case 0x003DE080u:
        d3d8_emit_texture_transforms();
        return 0u;
    case 0x003DEB80u:
        d3d8_emit_fixed_function_matrices(args[1]);
        return 0u;
    case 0x003D9DB0u:
        return d3d8_matrix_inverse(args[0], args[1], args[2]);
    case 0x003D5340u:
        d3d8_begin(args[0]);
        return 0u;
    case 0x003D52B0u:
        d3d8_set_vertex_data_2f(args[0], args[1], args[2]);
        return 0u;
    case 0x003D5380u:
        d3d8_end();
        return 0u;
    case 0x003D8370u:
        d3d8_copy_snapshot(guest_frame(args[0]));
        return 0u;
    case 0x003D85F0u:
        d3d8_copy_setup();
        return 0u;
    case 0x003D8990u:
        d3d8_copy_draw_triangle();
        return 0u;
    case 0x003D8710u:
        d3d8_copy_restore(guest_frame(args[0]));
        return 0u;
    case 0x003D8E50u:
        return d3d8_swap(args[0]);
    case 0x003D3A80u:
        return d3d8_get_back_buffer((int32_t)args[0]);
    case 0x003D3AD0u:
        return d3d8_copy_rects(args[0], args[1], args[2], args[3], args[4]);
    case 0x003D6F90u:
        d3d8_state_set_97(args[0]);
        return 0u;
    case 0x003D82D0u:
        d3d8_state_set_99(args[0]);
        return 0u;
    case 0x003D8220u:
        d3d8_state_set_9b(args[0]);
        return 0u;
    case 0x003D8080u:
        d3d8_state_set_a0(args[0]);
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
    uint32_t steps[MAX_STEPS][2u + STEP_ARGS];
    uint32_t window_count = 0u;
    uint32_t windows[MAX_WINDOWS][2];
    static uint8_t *bytes[MAX_WINDOWS];
    if (!in || !out || fread(&step_count, 4, 1, in) != 1 || step_count == 0u ||
        step_count > MAX_STEPS) {
        return 3;
    }
    for (uint32_t step = 0u; step < step_count; step++) {
        if (fread(steps[step], sizeof(steps[step]), 1, in) != 1) {
            return 3;
        }
    }
    if (fread(&window_count, 4, 1, in) != 1 || window_count == 0u || window_count > MAX_WINDOWS) {
        return 3;
    }
    for (uint32_t index = 0u; index < window_count; index++) {
        if (fread(windows[index], sizeof(windows[index]), 1, in) != 1) {
            return 3;
        }
    }
    for (uint32_t index = 0u; index < window_count; index++) {
        bytes[index] = malloc(windows[index][1]);
        if (bytes[index] == NULL || fread(bytes[index], windows[index][1], 1, in) != 1) {
            return 3;
        }
    }

    environment_begin(KERNEL_AV_PACK_HDTV);
    for (uint32_t index = 0u; index < window_count; index++) {
        const uint32_t base = windows[index][0];
        const uint32_t size = windows[index][1];
        if (!(base >= D3D_REGION_BASE && base + size <= D3D_REGION_BASE + D3D_REGION_BYTES) &&
            !(base >= RDATA_REGION_BASE && base + size <= RDATA_REGION_BASE + RDATA_REGION_BYTES)) {
            map_fixed(base, size);
        }
        memcpy(kernel_guest_at(base, size), bytes[index], size);
    }
    d3d8_pushbuffer_set_consumer(log_roll, NULL);

    uint32_t results[MAX_STEPS] = {0u};
    uint32_t fatal_step = NO_FATAL;
    uint32_t fatal_where = 0u;
    for (uint32_t step = 0u; step < step_count; step++) {
        uint32_t result = 0u;
        RUN_EXPECTING_FATAL(result = run_step(steps[step][0], &steps[step][2]));
        if (fatal_seen) {
            fatal_step = step;
            fatal_where = fatal_address;
            fprintf(stderr, "native fatal at %#x: %s\n", (unsigned)fatal_address, fatal_text);
            break;
        }
        results[step] = result;
    }

    const uint32_t logged = roll_count < MAX_ROLLS ? roll_count : MAX_ROLLS;
    if (fwrite(results, sizeof(uint32_t), step_count, out) != step_count ||
        fwrite(&fatal_step, 4, 1, out) != 1 || fwrite(&fatal_where, 4, 1, out) != 1 ||
        fwrite(&roll_count, 4, 1, out) != 1 || fwrite(roll_cursors, 4, logged, out) != logged) {
        return 4;
    }
    for (uint32_t index = 0u; index < window_count; index++) {
        if (fwrite(kernel_guest_at(windows[index][0], windows[index][1]), windows[index][1], 1,
                   out) != 1) {
            return 4;
        }
    }
    fclose(in);
    fclose(out);
    environment_end();
    return 0;
}
