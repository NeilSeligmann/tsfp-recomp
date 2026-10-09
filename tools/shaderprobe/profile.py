# SPDX-License-Identifier: GPL-3.0-or-later
"""The addresses of one build of the title that the probe watches, as data.

Every value here was measured by `tools.shaderscan` and is recorded in
`docs/shader-inputs.md`. They are defaults, not constants of the method: every one is a
command line option of `tools.shaderprobe.cli`, and `analyze` cross-checks them against
the capture (a function the profile names that never logs, or a store the lift did not
have, is reported rather than assumed).
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class BuilderProfile:
    """A source builder, the wrapper mask applied to the key before it, and its assembler site."""

    name: str
    entry: int
    key_register: str
    mask: int
    #: the function that calls the builder's output into the assembler (the cache wrapper)
    wrapper: int
    #: byte size of the builder function, to attribute an assembler caller to it
    size: int


@dataclass(frozen=True)
class Profile:
    key_global: int = 0x4B85E4
    #: functions that store a key argument to the global (batchers)
    batchers: tuple[int, ...] = (0x18EA0, 0x18FF0)
    #: 1-based argument of each batcher that carries the key at its direct call sites. The
    #: second batcher stores a different argument: measured from its prologue, not assumed.
    batcher_key_arguments: tuple[tuple[int, int], ...] = ((0x18EA0, 3), (0x18FF0, 2))
    #: the function that ORs and ANDs modifications into the key and requests programs
    key_modifier: int = 0x22BE0
    vertex_cache: int = 0x210A0
    pixel_cache: int = 0x20FE0
    #: the game's vertex-program loader and its SetPixelShader wrapper
    loader: int = 0x226E0
    pixel_install: int = 0x22720
    #: the XDK addresses the host probe watches
    xdk_loader: int = 0x3D59E0
    xdk_pixel_shader: int = 0x3D92E0
    xdk_declaration: int = 0x3D5630
    xdk_draws: tuple[int, ...] = (0x3D4FB0, 0x3D5050)
    assembler: int = 0x3EE2B3
    #: 0-based stack arguments of the assembler: source pointer, byte length, flags
    assembler_source: int = 1
    assembler_length: int = 2
    assembler_flags: int = 3
    vertex_builder: BuilderProfile = BuilderProfile(
        "vertex", 0x208B0, "ebx", 0xFFFD9FFF, 0x210A0, 897
    )
    pixel_builder: BuilderProfile = BuilderProfile(
        "pixel", 0x20CC0, "edx", 0xC83FE804, 0x20FE0, 783
    )
    #: translation wrappers that load literal keys into a register: (entry, register, end)
    wrapper_literals: tuple[tuple[int, str, int], ...] = (
        (0x1CD90, "esi", 0x1CFE2),
        (0x1D0B0, "esi", 0x1D2D0),
    )

    def key_argument(self, batcher: int) -> int:
        """The 1-based stack argument of `batcher` that it stores to the key global."""
        return dict(self.batcher_key_arguments)[batcher]

    def entry_functions(self) -> set[int]:
        """The lifted functions whose calls the instrumented lift logs."""
        return {
            *self.batchers,
            self.key_modifier,
            self.vertex_cache,
            self.pixel_cache,
            self.loader,
            self.pixel_install,
        }

    def probe_spec(self) -> str:
        """The TSFP_XDK_PROBE value for this profile (grammar in src/host/xdk_thunk.c)."""
        addresses = [
            self.xdk_loader,
            self.xdk_pixel_shader,
            self.xdk_declaration,
            self.assembler,
            *self.xdk_draws,
        ]
        at = ",".join(f"0x{a:X}" for a in addresses)
        return (
            f"at={at};word=0x{self.key_global:X};"
            f"hash=0x{self.assembler:X}:{self.assembler_source}:{self.assembler_length}"
        )


RETAIL = Profile()
