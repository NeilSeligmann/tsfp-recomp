/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No-op stand-in for src/gpu/d3d8_swap_replay.c in the Python oracle harnesses, which compile the
 * D3D8 handlers with plain `cc` and no Vulkan. The real file (T84a) pulls in gpu_pgraph*, gpu_vsh_*
 * and gpu_device, so it cannot link there. d3d8_swap_replay_enable is never called by those builds,
 * so the replay is off and these hooks are exactly what the real file does when it is not enabled:
 * nothing. The only symbols the other d3d8_*.c files reference are the hooks and the two read-only
 * getters (stats, last frame; T839 d3d8_overlay_present.c) below, which answer as when not enabled. The
 * header is included so a signature drift in either fails this file's compile, not the link.
 *
 * The list of sources to build is tools/d3dscan/oracle_sources.py, never a hand-written glob.
 */

#include "d3d8_swap_replay.h"
#include "live_texture_watch.h"

void d3d8_swap_replay_on_render_target(uint32_t previous, uint32_t target)
{
    (void)previous;
    (void)target;
}

void d3d8_swap_replay_on_present(uint64_t frame_number)
{
    (void)frame_number;
}

void d3d8_swap_replay_on_texture(uint32_t stage, uint32_t texture)
{
    (void)stage;
    (void)texture;
}

d3d8_swap_replay_stats d3d8_swap_replay_get_stats(void)
{
    d3d8_swap_replay_stats stats = {0};
    return stats;
}

const gpu_image *d3d8_swap_replay_last_frame(void)
{
    return NULL;
}

/* T792: d3d8_lock.c notes guest writes for the live texture cache, which is Vulkan side and absent here. */
void live_texture_watch_note(uint32_t address, uint32_t bytes)
{
    (void)address;
    (void)bytes;
}
