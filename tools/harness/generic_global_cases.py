# SPDX-License-Identifier: GPL-3.0-or-later
"""Original-backed Cartesian global-data inputs, never substituted callee answers.

Frozen contracts: docs/t1512-generic-default-domains.md. Namespace80. Every
configuration receives the same eight seed/frame backgrounds, independent of
its eventual guest behavior. Historical amended domains remain independent.
"""

from dataclasses import dataclass, replace
from math import prod

from .model import Case
from .seeding import SeedPolicy, make_case

NAMESPACE = 80
BACKGROUNDS = 8


@dataclass(frozen=True, slots=True)
class Axis:
    address: int
    width: int
    values: tuple[int, ...]

    def __post_init__(self) -> None:
        if (
            type(self.address) is not int
            or self.width not in (1, 4)
            or type(self.width) is not int
            or not 0 <= self.address <= (1 << 32) - self.width
            or type(self.values) is not tuple
            or not self.values
            or any(
                type(value) is not int or not 0 <= value < 1 << (8 * self.width)
                for value in self.values
            )
            or len(set(self.values)) != len(self.values)
        ):
            raise ValueError("invalid global-data axis")


@dataclass(frozen=True, slots=True)
class Alias:
    values: tuple[int, ...]
    esp: int


@dataclass(frozen=True, slots=True)
class Domain:
    va: int
    axes: tuple[Axis, ...]
    aliases: tuple[Alias, ...] = ()

    def __post_init__(self) -> None:
        if (
            type(self.va) is not int
            or not 0 <= self.va < 1 << 32
            or type(self.axes) is not tuple
            or not self.axes
            or any(not isinstance(axis, Axis) for axis in self.axes)
            or type(self.aliases) is not tuple
            or any(
                not isinstance(alias, Alias) or type(alias.values) is not tuple
                for alias in self.aliases
            )
        ):
            raise ValueError("invalid global-data root")
        occupied: set[int] = set()
        for axis in self.axes:
            addresses = set(range(axis.address, axis.address + axis.width))
            if occupied & addresses:
                raise ValueError("overlapping global-data axes")
            occupied.update(addresses)
        for alias in self.aliases:
            if (
                len(alias.values) != len(self.axes)
                or any(
                    type(value) is not int or value not in axis.values
                    for axis, value in zip(self.axes, alias.values, strict=True)
                )
                or type(alias.esp) is not int
                or not 0 <= alias.esp <= (1 << 32) - 4
            ):
                raise ValueError("invalid global-data alias")
        if len(set(self.aliases)) != len(self.aliases):
            raise ValueError("duplicate global-data alias")

    @property
    def products(self) -> int:
        return prod(len(axis.values) for axis in self.axes)

    @property
    def count(self) -> int:
        return (self.products + len(self.aliases)) * BACKGROUNDS

    def configuration(self, ordinal: int) -> tuple[tuple[int, ...], int | None]:
        if type(ordinal) is not int or not 0 <= ordinal < self.count:
            raise ValueError("unsupported global-data ordinal")
        position = ordinal // BACKGROUNDS
        if position >= self.products:
            alias = self.aliases[position - self.products]
            return alias.values, alias.esp
        values = []
        for axis in reversed(self.axes):
            position, index = divmod(position, len(axis.values))
            values.append(axis.values[index])
        return tuple(reversed(values)), None


_FLAGS = tuple(
    background | bits for background in (0, 0xFF7FFFBF) for bits in (0, 0x40, 0x800000, 0x800040)
)
DOMAINS = (
    Domain(
        0x22DE90,
        (
            Axis(0x7DE458, 4, _FLAGS),
            Axis(0x7DE470, 4, (0xFFFFFFFF, 0, 5, 6, 7, 8, 9, 0x80000000, 0x7FFFFFFF)),
        ),
        (Alias((0, 7), 0x7DE45C), Alias((0, 7), 0x7DE474)),
    ),
    Domain(
        0x356990,
        (
            Axis(0x79094C, 4, (0x64, 0x65, 0x66)),
            Axis(0x76B140, 4, (0, 1, 2, 3, 0x80000000)),
            Axis(0x76B108, 4, (0, 1, 0x80000000, 0xFFFFFFFF)),
            Axis(0x76B1A4, 4, (0, 9, 0xA, 0xB)),
        ),
        tuple(Alias((0x64, 2, 1, 0xA), esp) for esp in (0x790950, 0x76B144, 0x76B10C, 0x76B1A8)),
    ),
    Domain(
        0x2E4470,
        (
            Axis(0x7DE455, 1, (9, 0xA, 0xB)),
            Axis(0x790950, 4, (0xFFFFFFFF, 0, 1, 2, 3, 0x7FFFFFFF)),
            Axis(0x7330D8, 4, (0, 1, 0x80000000, 0xFFFFFFFF)),
            Axis(0x7DE458, 4, (0, 0x200, 0xFFFFFFFF, 0xFFFFFDFF)),
        ),
        tuple(Alias((0xA, 2, 0, 0x200), esp) for esp in (0x7DE459, 0x790954, 0x7330DC, 0x7DE45C)),
    ),
    Domain(
        0x190A80,
        tuple(
            Axis(address, 4, (0, 1, 0x80000000, 0xFFFFFFFF))
            for address in (0x7B9798, 0x7B9794, 0x7B978C)
        ),
        tuple(Alias((1, 0x80000000, 0xFFFFFFFF), esp) for esp in (0x7B97A0, 0x7B979C, 0x7B9794)),
    ),
    Domain(
        0x2D28F0,
        (Axis(0x76BBC8, 4, (0, 1, 2, 3, 4, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF)),),
        (Alias((2,), 0x76BBD0), Alias((2,), 0x76BBCC)),
    ),
)
SUPPORTED_VAS = tuple(domain.va for domain in DOMAINS)


def _domain(va: int) -> Domain:
    for domain in DOMAINS:
        if va == domain.va:
            return domain
    raise ValueError("unsupported global-data root")


def global_case_count(va: int) -> int:
    return next((domain.count for domain in DOMAINS if domain.va == va), 0)


def make_global_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    domain = _domain(va)
    values, esp = domain.configuration(ordinal)
    case = make_case(seed, (NAMESPACE << 40) + ordinal, va, size, policy=policy)
    patches = list(case.patches)
    regs = list(case.regs)
    if esp is not None:
        frame = next(data for address, data in case.patches if address == case.esp)
        patches.append((esp, frame[:4]))
        regs[4] = esp
    patches.extend(
        (axis.address, value.to_bytes(axis.width, "little"))
        for axis, value in zip(domain.axes, values, strict=True)
    )
    return replace(case, patches=tuple(patches), regs=tuple(regs))
