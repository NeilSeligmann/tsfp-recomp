# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect user-supplied XMV metadata without decoding or modifying it."""

import argparse
import json
from dataclasses import asdict
from pathlib import Path

from tools.errors import ParseError
from tools.xmvscan import parse


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args(argv)
    try:
        result = parse(args.input.read_bytes())
    except (OSError, ParseError) as error:
        parser.exit(2, f"xmvscan: {error}\n")
    print(json.dumps(asdict(result), indent=2))
    return 0
