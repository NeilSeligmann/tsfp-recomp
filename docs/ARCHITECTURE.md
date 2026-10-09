# Architecture
1. `tools.xdvdfs` extracts `default.xbe` from your disc. `tools.xbe` parses it.
2. `tools.lift` (vendored `third_party/xboxrecomp`) translates the x86 code to C under `tmp/private-host/lift-*/gen`.
3. CMake builds that C with `src/`: `loader` maps the XBE, `xbox` implements kernel/XDK/D3D8 calls, `gpu` turns NV2A pushbuffer
   state into Vulkan, `audio` emulates DirectSound/AC97, `input` maps pads, `game` holds proven hand-written replacements.
4. `tools.private_host` automates 1-3, `tools.play` runs the host and writes a stop report.
