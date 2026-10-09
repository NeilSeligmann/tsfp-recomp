# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for T848: a CLEAR_SURFACE event carries its own surface words (gpu_pgraph.c) and live_vk_draw.c resolves the clear's
target from them (not from a draw's snapshot).

TARGETS. The capture mutants are killed by `test_gpu_pgraph_state` (device free). The `clear-*` mutants need `test_live_vk_draw`, which
needs SDL3 and a Vulkan device (llvmpipe is enough, SDL_VIDEODRIVER=dummy): they were NOT run on the machine that wrote this set
(no SDL3 there), run them with `c_suites.py --prefix t848` where SDL3 is installed.
"""

PGRAPH = "src/gpu/gpu_pgraph.c"
DRAW = "src/gpu/live_vk_draw.c"


def mutation(identifier: str, file: str, old: str, new: str, target: str, why: str) -> dict:
    return {
        "id": f"t848-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": [target],
        "why": why,
    }


def pgraph(identifier: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, PGRAPH, old, new, "test_gpu_pgraph_state", why)


def draw(identifier: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, DRAW, old, new, "test_live_vk_draw", why)


MUTATIONS: list[dict] = [
    pgraph(
        "capture-format",
        "        clear->surface_format = state->output[GPU_PGRAPH_OUT_SURFACE_FORMAT];\n",
        "",
        "the clear must carry the surface format word it was written with.",
    ),
    pgraph(
        "capture-pitch",
        "        clear->surface_pitch = state->output[GPU_PGRAPH_OUT_SURFACE_PITCH];\n",
        "        clear->surface_pitch = state->output[GPU_PGRAPH_OUT_SURFACE_FORMAT];\n",
        "the clear must carry the pitch word, not another one.",
    ),
    pgraph(
        "capture-offset",
        "        clear->surface_color_offset = state->output[GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET];\n",
        "        clear->surface_color_offset = state->output[GPU_PGRAPH_OUT_SURFACE_ZETA_OFFSET];\n",
        "the clear must carry the colour offset, not the zeta offset.",
    ),
    pgraph(
        "written-any",
        "state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT] &&\n                                 state->output_written[GPU_PGRAPH_OUT_SURFACE_PITCH] &&",
        "state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT] ||\n                                 state->output_written[GPU_PGRAPH_OUT_SURFACE_PITCH] ||",
        "a clear is only named by surface words when all three were written.",
    ),
    draw(
        "clear-words-unwritten",
        "    const surface_words words = {event->surface_format, event->surface_pitch, event->surface_color_offset, event->surface_written};\n",
        "    const surface_words words = {event->surface_format, event->surface_pitch, event->surface_color_offset, true};\n",
        "a clear with no surface words written must be refused, not drawn into a guessed target.",
    ),
    draw(
        "clear-offset-ignored",
        "    const surface_words words = {event->surface_format, event->surface_pitch, event->surface_color_offset, event->surface_written};\n",
        "    const surface_words words = {event->surface_format, event->surface_pitch, 0x0200000u, event->surface_written};\n",
        "a clear lands in the target its own colour offset names.",
    ),
]
