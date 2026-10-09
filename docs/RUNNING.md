# Running the recomp

Follow the Quick start in the [README](../README.md) first. Run everything from the repository root.

## Controls
`--pad-source keyboard` maps a keyboard to the pad. A connected SDL gamepad is used otherwise.
Exact keyboard bindings are listed in `tools/hotkey_defaults.py` and `src/input/`.
Hotkeys: `Ctrl+Shift+S` screenshot, `Ctrl+Shift+O` stop.

## Useful flags (after `--`)
- `--window` open an SDL3 window with audio, `--mute` silence it
- `--skip-intro` skip logos (host shortcut, not original behaviour)
- `--strict` drop all opt-in flags, `--dry-run` print the host command only
- `--list-flags` print the flag table

Each run writes `tmp/play/<time>/` with `host.out`, `host.err`, `stop.txt` and `repro.sh`.

## Rebuilding / cache
The lift and host are cached under `tmp/private-host/` keyed by the lifter, tools and your XBE.
Delete that directory to force a clean rebuild. Build tests with `cmake -B build && cmake --build build && ctest --test-dir build`
(tests that need your game files report `Skipped`).

## Troubleshooting
- `libSDL3.so.0 not found`: install `libsdl3-dev` (or SDL3 from source) and rebuild.
- No display: use `xvfb-run` and add `--headless-display` or `SDL_VIDEODRIVER=dummy`.
- Vulkan: needs a Vulkan 1.1 driver; Mesa `llvmpipe` works but is slow.
- `unimplemented ... at 0x...` stop report: an unfinished XDK function or instruction. Expected in places, see STATUS.
