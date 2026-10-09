# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the Ghidra-derived dirty-cascade model against original instructions.

The controller cases substitute recording helpers: they check dispatch, arguments,
mask feedback and cleanup, not helper implementations. The default-device draw
executes all helpers unmodified under the existing instantaneous-GPU model.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from tools.d3dscan.oracle import (
    DEVICE_BASE,
    DEVICE_POINTER_SLOT,
    SENTINEL,
    KernelHandler,
    Oracle,
    create_device,
)
from tools.ghidra.query import QueryError, validate_output

DIRTY_SLOT = 0x003E3AB8
CONTROLLER = 0x003DED80
MASK_HELPER = 0x003E11D0
# Ordered predicates recovered from 0x003DED80; arities from its actual PUSH instructions.
HELPERS = (
    (0x100, 0x003DD7C0, 1),
    (0x800, MASK_HELPER, 2),
    (0x4000, 0x003DD930, 1),
    (0xF, 0x003DDCB0, 2),
    (0x2000, 0x003DDEA0, 1),
    (0x400, 0x003DE080, 1),
    (0xFF1000, 0x003DE830, 2),
    (0x200, 0x003DEB80, 2),
)


def expected_dispatch(dirty: int, replacement: int | None) -> tuple[list[dict], int]:
    calls = []
    for bits, target, arity in HELPERS:
        if dirty & bits:
            arguments = [DEVICE_BASE, dirty][:arity]
            calls.append({"entry": target, "arguments": arguments})
            if target == MASK_HELPER and replacement is not None:
                dirty = replacement
    return calls, dirty & 0xC0000070


def probe_controller(xbe: Path) -> list[dict]:
    from unicorn.x86_const import UC_X86_REG_EIP

    original = Oracle(xbe, instant_gpu=True)
    original.write32(DEVICE_POINTER_SLOT, DEVICE_BASE)
    masks = [0, 0x70, 0xC0000070, 0xFF7F7F, 0xFF7F2F, 0xFFFFFFFF]
    masks.extend(1 << bit for bit in range(32))
    results = []
    for mask in masks:
        for replacement in (None, 0, 0xFF7F7F):
            calls: list[dict] = []

            def recorder(
                target: int, arity: int, case_calls: list[dict], feedback: int | None
            ) -> KernelHandler:
                def record(oracle: Oracle, esp: int) -> int | None:
                    arguments = [oracle.read32(esp + 4 + i * 4) for i in range(arity)]
                    case_calls.append({"entry": target, "arguments": arguments})
                    # Check that the controller uses its saved local mask, not later global writes.
                    oracle.write32(DIRTY_SLOT, 0xDEADCAFE)
                    if target == MASK_HELPER:
                        return arguments[1] if feedback is None else feedback
                    return None

                return record

            # Hooks are installed once; replace their handlers for each case.
            for _, target, arity in HELPERS:
                handler = recorder(target, arity, calls, replacement)
                if target not in original._function_hooks:
                    original.hook_function(target, arity, handler)
                else:
                    original._function_hooks[target] = (arity, handler)
            original.write32(DIRTY_SLOT, mask)
            original.run(CONTROLLER, budget=1000)
            expected, dirty_after = expected_dispatch(mask, replacement)
            if original.emulator.reg_read(UC_X86_REG_EIP) != SENTINEL:
                raise QueryError("controller did not return within its instruction budget")
            if calls != expected or original.read32(DIRTY_SLOT) != dirty_after:
                raise QueryError(
                    f"cascade model disagrees at mask {mask:#x}, feedback {replacement}"
                )
            results.append(
                {
                    "mask": mask,
                    "replacement": replacement,
                    "calls": calls,
                    "dirty_after": dirty_after,
                }
            )
    return results


def probe_default_device(xbe: Path) -> dict:
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP

    original = Oracle(xbe, instant_gpu=True)
    if not create_device(original).succeeded:
        raise QueryError("CreateDevice failed in the existing oracle model")
    entries = {0x003DEE00, CONTROLLER, *(target for _, target, _ in HELPERS)}
    trace = []
    arities = {target: arity for _, target, arity in HELPERS}
    arities[0x003DEE00] = 1
    arities[CONTROLLER] = 0

    def record(emulator: object, pc: int, size: int, user: object) -> None:
        if pc in entries:
            esp = original.emulator.reg_read(UC_X86_REG_ESP)
            trace.append(
                {
                    "entry": pc,
                    "dirty": original.read32(DIRTY_SLOT),
                    "arguments": [original.read32(esp + 4 + 4 * i) for i in range(arities[pc])],
                }
            )

    original.emulator.hook_add(UC_HOOK_CODE, record, begin=0x003D0000, end=0x003E2000)
    original.write32(DIRTY_SLOT, 0xFF7F7F)
    start = original.read32(DEVICE_BASE)
    original.run(0x003D4FB0, [8, 0, 4], budget=1_000_000)
    if original.emulator.reg_read(UC_X86_REG_EIP) != SENTINEL:
        raise QueryError("default-device draw did not return within its instruction budget")
    end = original.read32(DEVICE_BASE)
    stream = original.read_bytes(start, end - start)
    return {
        "scenario": "CreateDevice defaults, dirty=0xFF7F7F, DrawVertices(8,0,4)",
        "hardware_model": "existing oracle, instantaneous GPU; not actual title boot state",
        "trace": trace,
        "dirty_after": original.read32(DIRTY_SLOT),
        "command_bytes": len(stream),
        "command_sha256": hashlib.sha256(stream).hexdigest(),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    output = validate_output(args.out)
    cases = probe_controller(args.xbe)
    result = {
        "xbe_sha256": hashlib.sha256(args.xbe.read_bytes()).hexdigest(),
        "controller_cases": cases,
        "default_device_draw": probe_default_device(args.xbe),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{len(cases)} controller cases agree; original draw returned; wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
