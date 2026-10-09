# SPDX-License-Identifier: GPL-3.0-or-later
"""NV2A register-combiner configurations: decode, reference model, GLSL translator.

The pixel pipeline of the NV2A is a fixed chain of up to 8 general combiner stages and a
final combiner, programmed by method writes. The title's pixel-shader assembler turns
`xps.1.1` source into a 240-byte block of those register values. This package reads such
a block (`config`), evaluates it on one fragment in plain Python (`reference`), and
translates it to GLSL 4.50 (`glsl`). `docs/combiner-translator.md` records where each
field meaning comes from and how the translation was validated.
"""
