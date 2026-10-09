# SPDX-License-Identifier: GPL-3.0-or-later
"""The one list of src/gpu/d3d8_*.c sources for the Python oracle builds (plain `cc`, no Vulkan).

WHY THIS EXISTS. About forty harnesses (tests/test_d3dscan_*.py, test_d3d8_*_oracle.py,
test_dsound*.py, test_xgrph_*_oracle.py, test_xinput_devices_oracle.py, test_xdk_real_smoke.py,
tools/d3dscan/port_diff.py, ...) compile the D3D8 handlers with `cc` and link them with a runner.
They used to glob `d3d8_*.c` themselves. T84 (67ef6b1) added src/gpu/d3d8_gpu_pgraph.c and T84a
(47c32ec) src/gpu/d3d8_swap_replay.c, which need gpu_pgraph*, gpu_vsh_* and gpu_device (Vulkan), so
every one of those links failed with undefined references while ctest (CMake links them properly)
stayed green. This module is the single place that knows which d3d8_*.c files are the non-Vulkan
set, so the next Vulkan-dependent file is added to NON_ORACLE_SOURCES once, not forty times.
tests/test_oracle_build_sources.py fails if a harness globs d3d8_*.c on its own.

THE REPLAY IS OFF IN THESE BUILDS. d3d8_swap_replay.c is swapped for the no-op hooks in
tests/c/d3d8_swap_replay_stub.c (d3d8_present.c and d3d8_bind.c call them), and d3d8_gpu_pgraph.c is
left out. T990 extracts its unchanged guest readers into the plain-C
`d3d8_gpu_memory.c` leaf: surface modeling now calls that real production reader,
so the glob includes it without linking the Vulkan decoder. The stub always comes from
THIS checkout, but is added only
when `root` has d3d8_swap_replay.c, so a harness pointed at an older tree (TSFP_T86_RUNNER_ROOT)
without the replay still builds.
"""

from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# d3d8_*.c files that are not part of the plain-cc oracle build.
# d3d8_surface_adapter.c: the optional XDK surface bridge (src/xbox/xdk_surface.h is generated).
# d3d8_gpu_pgraph.c, d3d8_swap_replay.c: the Vulkan replay stack, replaced by the stub below.
SURFACE_ADAPTER = "d3d8_surface_adapter.c"
VULKAN_REPLAY = frozenset({"d3d8_gpu_pgraph.c", "d3d8_swap_replay.c"})
# d3d8_frame_profile.c (T422): the opt-in profile reads the recording through the pgraph decoder.
# No handler calls it (d3d8_present.c only holds an observer pointer), so it is never linked.
PROFILE_ONLY = frozenset({"d3d8_frame_profile.c"})
NON_ORACLE_SOURCES = frozenset({SURFACE_ADAPTER}) | VULKAN_REPLAY | PROFILE_ONLY
LEAF_SOURCES = ("gpu_png.c",)
REPLAY_STUB = Path("tests/c/d3d8_swap_replay_stub.c")


def d3d8_oracle_sources(root: Path = REPO, include_surface_adapter: bool = False) -> list[Path]:
    """The src/gpu/d3d8_*.c files of `root` to compile and link with plain `cc`, sorted, plus the
    replay stub (from this checkout) when `root` has the replay. `include_surface_adapter` keeps
    d3d8_surface_adapter.c for the one harness that links it."""
    skip = (VULKAN_REPLAY | PROFILE_ONLY) if include_surface_adapter else NON_ORACLE_SOURCES
    gpu = Path(root) / "src" / "gpu"
    sources = [path for path in sorted(gpu.glob("d3d8_*.c")) if path.name not in skip]
    # Plain C leaf of the handlers: T839 d3d8_overlay_present.c writes PNGs through gpu_png.c.
    sources += [Path(root) / "src" / "gpu" / name for name in LEAF_SOURCES if (gpu / name).exists()]
    if (gpu / "d3d8_swap_replay.c").exists():
        sources.append(REPO / REPLAY_STUB)
    return sources
