# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact register inputs are independent from stack cleanup and output scratch."""

from collections.abc import Mapping

INPUT_SETS = {"ecx": ("ecx",), "esi": ("esi",), "ecx_esi": ("ecx", "esi")}


def register_inputs(record: Mapping[str, object]) -> tuple[str, ...] | None:
    if "register_inputs" not in record:
        return None
    value = record["register_inputs"]
    if not isinstance(value, list) or tuple(value) not in INPUT_SETS.values():
        raise ValueError("register_inputs must be exactly ecx, esi or ordered ecx+esi")
    return tuple(value)


def validate_inputs(
    inputs: tuple[str, ...] | None, convention: str, stack_args: int, scratch: tuple[str, ...]
) -> None:
    if inputs is None:
        return
    if inputs not in INPUT_SETS.values() or convention not in ("cdecl", "stdcall"):
        raise ValueError("explicit inputs require canonical inputs and cdecl/stdcall cleanup")
    if type(stack_args) is not int or not 0 <= stack_args <= 8 or scratch:
        raise ValueError("explicit inputs require 0..8 stack arguments and no scratch exclusions")


def input_contract(
    inputs: tuple[str, ...] | None, convention: str, stack_args: int
) -> dict[str, object] | None:
    if inputs is None:
        return None
    validate_inputs(inputs, convention, stack_args, ())
    return {
        "version": 1,
        "registers": list(inputs),
        "convention": convention,
        "stack_args": stack_args,
    }


def validate_contract(value: object) -> None:
    if not isinstance(value, dict) or set(value) != {
        "version",
        "registers",
        "convention",
        "stack_args",
    }:
        raise ValueError("malformed exact input_contract")
    if type(value["version"]) is not int or value["version"] != 1:
        raise ValueError("unknown exact input_contract version")
    inputs = register_inputs({"register_inputs": value["registers"]})
    validate_inputs(inputs, value["convention"], value["stack_args"], ())
