# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover what the guest asks the GPU to do, from the statically-linked D3D8 section.

The Xbox has no user-mode graphics driver. `D3D8.lib` is linked into the title and
*is* the driver, so every one of the 85 D3D entry points the game calls ends in a
write of NV2A pushbuffer methods. That makes the methods a function writes the most
direct available statement of what the entry point *means* -- far more direct than
its name, which for 74 of the 85 we do not have.

See `tools/d3dscan/pushscan.py` for the scanner and the reasons it is shaped the way
it is, and `docs/d3d8-usage.md` for the findings.
"""
