# SPDX-License-Identifier: GPL-3.0-or-later
"""The ONE table of host flags `tools.play` passes (T741).

The recipe is `docs/boot-frontier.md` row 18 (T394 movie boot, T183 audio flags, T681, T696).
`tier` is `base` (measured recipe, always on), `default` (T762: a HOST default, announced and
labelled, so the command line does NOT repeat it and `--strict` or `--disable` pass the host's
`--no-...` opt-out), `opt-in` (an owner decision or invented state, on in the frontier profile
but printed as such and removable), `off` (available, never on unless asked), `opt-out` (the
synthetic entry of a default turned off) or `explicit` (a default asked for by name, passed as
given).  `label` is the honesty label the host itself prints (FABRICATED, INFERRED, XEMU-LEVEL,
or empty).
The tool only builds a command line, it never changes a host default.
"""

from dataclasses import dataclass, replace

BASE, OPT_IN, OFF = "base", "opt-in", "off"
DEFAULT, OPT_OUT, EXPLICIT = "default", "opt-out", "explicit"
THREAD_TIMEOUT_MS = 120000


@dataclass(frozen=True)
class Flag:
    name: str
    args: tuple[str, ...]
    tier: str
    label: str
    why: str


def _flag(name: str, tier: str, label: str, why: str, *values: str) -> Flag:
    return Flag(name, (f"--{name}", *values), tier, label, why)


FLAG_TABLE: tuple[Flag, ...] = (
    _flag(
        "xonline-offline",
        OFF,
        "INFERRED",
        "T904/T1161: offline Startup and empty accounts; no live services",
    ),
    _flag(
        "skip-intro",
        OFF,
        "FABRICATED",
        "T1139: terminate startup logos through original movie cleanup; gameplay movies unchanged",
    ),
    _flag(
        "xnet-local-entropy",
        OFF,
        "INFERRED",
        "T1111: local OS entropy/key layout; preserves XNET initialization guard, no network",
    ),
    _flag("ac97-ready", BASE, "FABRICATED", "T183: codec reads ready, no AC97 device exists"),
    _flag("headless-streams", BASE, "", "T183: passive DirectSound streams"),
    _flag("headless-buffers", BASE, "", "T183: passive DirectSound buffers"),
    _flag("headless-listener", BASE, "", "T183: passive DirectSound listener"),
    _flag("headless-second-vblank", BASE, "", "T183: second vblank callback model"),
    _flag("native-shader-assembler", BASE, "", "T183: native shader assembler"),
    _flag("native-xmv", BASE, "", "T394: native XMV movie path (default with --disc, T1093)"),
    _flag("headless-movie-audio", BASE, "", "T394: movie audio without a device"),
    _flag("couple-vblank-effects", BASE, "", "T394: couple vblank effects"),
    _flag("check-vblank-quiescence", BASE, "", "T394: vblank quiescence check"),
    _flag("overlay-consume", BASE, "", "T394: consume movie overlays"),
    _flag("vblank-owner-waits", BASE, "", "T460: owner frame wait budget", "1000"),
    _flag("vblank-worker-blanks", BASE, "", "T592: modelled worker blanks", "100"),
    _flag("thread-timeout", BASE, "", "watchdog", str(THREAD_TIMEOUT_MS)),
    _flag(
        "passive-audio-completion",
        DEFAULT,
        "",
        "T681: passive audio completion (owner decision T608), host default since T762",
    ),
    _flag(
        "vblank-poll-blank",
        DEFAULT,
        "FABRICATED",
        "T696: poll-delivered blank (FABRICATED timing, owner decision), host default since T762",
    ),
    _flag(
        "async-file-io",
        DEFAULT,
        "XEMU-LEVEL",
        "T743: overlapped NtReadFile completes on the virtual clock (XEMU-LEVEL drive timing, "
        "T763), host default since T762, the title's own audio stream reads its packets",
    ),
    _flag(
        "async-file-io-file-object",
        OFF,
        "INFERRED",
        "T764: with async-file-io, an NtReadFile with no Event (loader helper, XMV reads) is "
        "queued and signals the FILE OBJECT at completion (INFERRED drive timing)",
    ),
    _flag(
        "gpu-live",
        OFF,
        "INFERRED",
        "T838 (M9): the live Vulkan renderer draws the title in the --window (a Vulkan window, "
        "movie overlay pictures through the live compositor), refused draws are named in the "
        "stop report; INFERRED until validated against xemu, needs --window, `--gpu-live` of "
        "tools.play passes its modules directory",
    ),
    _flag(
        "gpu-live-inferred",
        OFF,
        "INFERRED",
        "T838: with gpu-live, also admit the INFERRED texture formats, sampler states and "
        "byte format CopyRects",
    ),
    _flag(
        "gpu-live-blit",
        OFF,
        "INFERRED",
        "T849: with gpu-live, present with the swapchain pre-pass blit (front target image and "
        "overlay texture blitted into the acquired image, no front buffer readback); the "
        "readback route stays the fallback",
    ),
    _flag(
        "synthetic-pad",
        OFF,
        "FABRICATED",
        "T717: one synthetic gamepad on port 0 (the title never opens a pad without it)",
    ),
)
TABLE_BY_NAME = {flag.name: flag for flag in FLAG_TABLE}


class UnknownFlagError(ValueError):
    pass


def select(
    enable: tuple[str, ...] = (), disable: tuple[str, ...] = (), strict: bool = False
) -> list[Flag]:
    """Active flags in table order: base always, opt-in unless `strict` or disabled, off if enabled.

    Base flags cannot be disabled (that would be a different recipe, not a profile).
    """
    for name in (*enable, *disable):
        if name not in TABLE_BY_NAME:
            raise UnknownFlagError(f"unknown flag {name!r}, table has: {', '.join(TABLE_BY_NAME)}")
    for name in disable:
        if TABLE_BY_NAME[name].tier == BASE:
            raise UnknownFlagError(f"{name!r} is part of the base recipe and cannot be disabled")
    active = []
    for flag in FLAG_TABLE:
        if flag.tier == BASE:
            on = True
        elif flag.tier in (OPT_IN, DEFAULT):
            on = not strict and flag.name not in disable
        else:
            on = False
        if flag.name in enable:
            on = True
            if flag.tier == DEFAULT:
                flag = replace(flag, tier=EXPLICIT)
        if on:
            active.append(flag)
        elif flag.tier == DEFAULT:
            # the host default is on, turning it off is the host's own `--no-...` flag
            active.append(Flag(flag.name, (f"--no-{flag.name}",), OPT_OUT, flag.label, flag.why))
    return active


def announce(active: list[Flag]) -> list[str]:
    """One printed line per active flag, defaults, opt-ins and invented state marked as such."""
    lines = []
    for flag in active:
        tags = []
        if flag.tier == DEFAULT:
            tags.append("DEFAULT")
        elif flag.tier == OPT_OUT:
            tags.append("OPT-OUT")
        elif flag.tier != BASE:
            tags.append("OPT-IN")
        if flag.label:
            tags.append(flag.label)
        tag = f"[{' '.join(tags)}] " if tags else ""
        lines.append(f"  {tag}{' '.join(flag.args)}  ({flag.why})")
    return lines


def argv_of(active: list[Flag]) -> list[str]:
    """The command line: a default left on is the host's own, so it is not repeated (T762)."""
    return [arg for flag in active if flag.tier != DEFAULT for arg in flag.args]
