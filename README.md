# TimeSplitters: Future Perfect — Xbox static recompilation

A static recompilation of the original Xbox build of **TimeSplitters: Future Perfect**
(Free Radical Design / EA, 2005) into C, with a native host layer (kernel, XDK, D3D8/NV2A
rendering on Vulkan, DirectSound audio, input) for modern PCs.

> **Status: experimental.** The title boots, plays its intros and menus and runs Story
> mode in a window with audio and keyboard/pad input. It is not complete or bug-free
> (rendering gaps, missing XDK functions and stops are expected). See [docs/STATUS.md](docs/STATUS.md).

This repository contains **no game code, assets, BIOS or disc images**. You must supply your
own legally obtained copy of the game. Everything derived from it is generated locally and is gitignored.

## AI disclaimer

This project was built with heavy use of AI coding agents: **Claude** (Anthropic, via Claude Code)
and **Codex** (OpenAI). The agents wrote most of the code, tooling and documentation under human
direction. Not every line has been reviewed by a human. Claims in the code are labelled by
confidence (MEASURED, INFERRED, FABRICATED), and FABRICATED/INFERRED behaviour (for example
host-side timing and shortcuts) is not original console behaviour. Use at your own risk, with no warranty.
Details: [docs/AI-DISCLOSURE.md](docs/AI-DISCLOSURE.md).

## Quick start (Linux)

You need: the Xbox NTSC-U disc image of TimeSplitters: Future Perfect, plus the packages below.

```bash
# 1. Dependencies (Debian/Ubuntu)
sudo apt-get install build-essential clang cmake ninja-build pkg-config git \
    libsdl3-dev libvulkan-dev glslang-tools xvfb          # xvfb only for headless runs
curl -LsSf https://astral.sh/uv/install.sh | sh             # installs uv (Python tooling)

# 2. Python environment
uv sync

# 3. Put your disc image here and extract the executable
mkdir -p discs build
cp /path/to/your/TimeSplitters-Future-Perfect-xbox.iso discs/tsfp-xbox.iso
uv run python -m tools.xdvdfs.cli discs/tsfp-xbox.iso --extract default.xbe --dest build

# 4. Lift the executable to C and build the host (first run takes several minutes)
uv run python -m tools.private_host build

# 5. Play
uv run python -m tools.private_host play -- --disc discs/tsfp-xbox.iso --window --interactive \
    --gpu-live --gpu-live-inferred --present window --pad-source keyboard
```

Add `--mute` to silence audio, `--skip-intro` to skip the studio logos. Press Escape or close the window to exit.
Run `uv run python -m tools.play --disc discs/tsfp-xbox.iso --list-flags` for every option.

Expected `build/default.xbe` SHA-256: `3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc`.
Other dumps are not supported.

More: [docs/RUNNING.md](docs/RUNNING.md) (controls, flags, troubleshooting),
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/STATUS.md](docs/STATUS.md).

## Layout

| Path | Contents |
|---|---|
| `src/` | native host: loader, kernel/XDK HLE, GPU (NV2A to Vulkan), audio, input, hand-written game replacements |
| `tools/` | disc/XBE tools, the lifter driver (`tools/lift`), build/run drivers (`private_host`, `play`) |
| `third_party/xboxrecomp/` | vendored lifter (see its `LICENSE` and `NOTICE`) |
| `tests/c/` | C test suites built by CMake (`ctest --test-dir build`) |

## License

GPL-3.0, see [LICENSE](LICENSE). Vendored code keeps its own license. TimeSplitters is the property of its owners;
this project is unaffiliated.
