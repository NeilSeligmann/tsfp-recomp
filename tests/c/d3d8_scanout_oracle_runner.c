/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native recording test; original-helper comparisons are in the Python caller. */
#include "test_d3d8_support.h"
#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_present.h"
#include "d3d8_vblank_effects.h"

static uint32_t failed_wait(void *context) { (void)context; return 0xC0000001u; }

static void create(void)
{
    (void)d3d8_device_register();
    write_title_parameters(SCRATCH_DATA, 0x140u);
    const uint32_t args[6] = {0u, 1u, 0u, 0u, SCRATCH_DATA, SCRATCH_DATA + 0x200u};
    CHECK_EQ_U32(call_stdcall(0x003D9230u, args, 6u), 0u);
    d3d8_present_reset();
    d3d8_device_store32(0x1DE0u, 0u);
    d3d8_present_save_state();
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    const unsigned scenario = (unsigned)strtoul(argv[1], NULL, 0);
    environment_begin(KERNEL_AV_PACK_HDTV);
    create();
    d3d8_flip_configure(true);
    d3d8_vblank_effects_configure(true);
    /* A real model write, deliberately different from the kernel mode-set scanout. */
    d3d8_flip_lock();
    (void)d3d8_flip_queue_locked((0x620001u << 5) | 1u);
    d3d8_flip_unlock();
    (void)d3d8_swap(4u);
    if (scenario == 1u) d3d8_flip_configure(false);
    if (scenario == 2u) d3d8_vblank_effects_configure(false);
    if (scenario == 3u) { create(); d3d8_present_set_mode(); } /* old lazy hardware record must not survive the wait */
    if (scenario == 4u || scenario == 5u) {
        d3d8_present_set_mode(); /* new kernel address supersedes earlier write */
        if (scenario == 5u) d3d8_flip_reset(); /* same GPU lifetime, reset write counter */
    }
    if (scenario == 8u) create();
    if (scenario == 6u) {
        (void)kernel_hle_register(159u, failed_wait);
    }
    const uint32_t mode = scenario == 7u ? 0x01000000u : 0u;
    d3d8_device_store32(D3D8_FLIP_DEV_DISPLAY_MODE, mode);
    d3d8_device_store32(D3D8_FLIP_DEV_FIELD_STATUS, 0u);
    const uint32_t count = d3d8_device_load32(D3D8_FLIP_DEV_COUNT);
    /* Only scenarios 0/5/7 get a new actual queue; others retain their current display. */
    if (scenario == 0u || scenario == 5u || scenario == 7u) {
        d3d8_flip_lock();
        (void)d3d8_flip_queue_locked((0x480001u << 5) | 1u);
        d3d8_flip_unlock();
    }
    const uint32_t offsets[] = {D3D8_FLIP_DEV_PITCH, D3D8_FLIP_DEV_MODE_WORD,
        D3D8_FLIP_DEV_DISPLAY_MODE, D3D8_FLIP_DEV_DISPLAY_START_OFF,
        D3D8_FLIP_DEV_CONSUMER, D3D8_FLIP_DEV_COUNT, D3D8_FLIP_DEV_THRESHOLD,
        D3D8_FLIP_DEV_FIELD_STATUS, D3D8_FLIP_DEV_SLOT0, D3D8_FLIP_DEV_SLOT0+4u,
        D3D8_FLIP_DEV_SLOT0+8u, D3D8_FLIP_DEV_SLOT1, D3D8_FLIP_DEV_SLOT1+4u,
        D3D8_FLIP_DEV_SLOT1+8u, D3D8_FLIP_DEV_GAMMA_PENDING,
        D3D8_FLIP_DEV_GAMMA_PENDING+4u};
    FILE *out = fopen(argv[2], "wb");
    if (!out) return 3;
    for (unsigned i=0; i<sizeof(offsets)/sizeof(offsets[0]); i++) {
        const uint32_t value=d3d8_device_load32(offsets[i]); fwrite(&value,4,1,out);
    }
    RUN_EXPECTING_FATAL((void)d3d8_swap(4u));
    fprintf(stderr,"fatal=%u text=%s\n",(unsigned)fatal_seen,fatal_text);
    kernel_av_display display;
    const uint32_t known=kernel_av_display_get(&display);
    const d3d8_flip_hardware hw=d3d8_flip_hardware_get();
    const uint32_t result[] = {fatal_seen, fatal_address,
        d3d8_frame_queue_at(d3d8_frame_queue_count()-1u).scanout,
        known ? display.frame_buffer : 0u, hw.display_start,
        (uint32_t)hw.display_start_writes, count,
        d3d8_device_load32(D3D8_FLIP_DEV_DISPLAY_START_OFF)};
    fwrite(result,sizeof(result),1,out); fclose(out);
    environment_end();
    return failures != 0;
}
