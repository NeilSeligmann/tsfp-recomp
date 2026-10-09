# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Graft one function of a fresh lift into a COPY of the canonical lifted tree (T803, T821).

The canonical `generated/lifted/gen` is owned by the promotion task (T659) and is never written here. A fresh lift
also differs from it in hundreds of bodies (newer lifter patches), so to measure the effect of ONE new function
(for example the `0x191B10` seed of `tools/config/lift_thread_entries.json`) the like for like comparison is the
canonical tree plus that one body. The function is added as a new chunk `recomp_<chunk>.c`, its prototype goes into
`recomp_funcs.h` and its row into the address sorted `recomp_dispatch.c` table (whose size is raised by one).

    python3 -m tools.graft_lifted_function --canonical generated/lifted/gen --fresh tmp/relift/gen \
        --out tmp/graft/gen --address 0x00191B10

Build with `-DTSFP_LIFTED_DIR=$PWD/tmp/graft/gen`. A function the canonical tree already has is refused.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

CHUNK_HEADER = """/**
 * Grafted chunk (T821 tools.graft_lifted_function): {name} from a fresh lift, added to a copy of the canonical tree.
 */

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <math.h>

"""


def function_body(fresh: Path, name: str) -> str:
    """The function's doc comment and definition from whichever fresh chunk holds it."""
    pattern = re.compile(
        rf"/\*\*\n \* {name}\n.*?\nvoid {name}\(void\)\n\{{\n.*?\n\}}\n", re.DOTALL
    )
    found = []
    for chunk in sorted(fresh.glob("recomp_[0-9][0-9][0-9][0-9].c")):
        match = pattern.search(chunk.read_text())
        if match:
            found.append(match[0])
    if len(found) != 1:
        raise SystemExit(f"{name}: expected exactly one definition in {fresh}, found {len(found)}")
    return found[0]


def graft(canonical: Path, fresh: Path, out: Path, address: int, chunk: str) -> None:
    name = f"sub_{address:08X}"
    row = f"    {{ 0x{address:08X}u, (recomp_func_t){name} }},\n"
    dispatch_text = (canonical / "recomp_dispatch.c").read_text()
    if row in dispatch_text or f"(recomp_func_t){name}" in dispatch_text:
        raise SystemExit(f"{name} is already in the canonical dispatch table")
    fresh_dispatch = (fresh / "recomp_dispatch.c").read_text()
    if row not in fresh_dispatch:
        raise SystemExit(f"{name} is not in the fresh lift's dispatch table")
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(canonical, out)
    (out / f"recomp_{chunk}.c").write_text(
        CHUNK_HEADER.format(name=name) + function_body(fresh, name)
    )
    funcs = (out / "recomp_funcs.h").read_text()
    prototype = f"void {name}(void);\n"
    if prototype in funcs:
        raise SystemExit(f"{name} already has a prototype in the canonical recomp_funcs.h")
    guard = funcs.rfind("#endif")
    if guard < 0:
        raise SystemExit("include guard #endif not found in the canonical recomp_funcs.h")
    (out / "recomp_funcs.h").write_text(funcs[:guard] + prototype + "\n" + funcs[guard:])
    # insert the row in address order (the dispatcher binary searches the table)
    rows = list(
        re.finditer(
            r"^    \{ 0x([0-9A-F]{8})u, \(recomp_func_t\)\w+ \},\n", dispatch_text, re.MULTILINE
        )
    )
    if not rows:
        raise SystemExit("no dispatch rows found in the canonical recomp_dispatch.c")
    after = [match for match in rows if int(match[1], 16) > address]
    at = after[0].start() if after else rows[-1].end()
    dispatch_text = dispatch_text[:at] + row + dispatch_text[at:]
    size = re.search(r"static const size_t g_recomp_table_size = (\d+);", dispatch_text)
    if size is None:
        raise SystemExit("g_recomp_table_size not found")
    dispatch_text = dispatch_text.replace(
        size[0], f"static const size_t g_recomp_table_size = {int(size[1]) + 1};"
    )
    (out / "recomp_dispatch.c").write_text(dispatch_text)
    print(
        f"grafted {name} into {out}: table {size[1]} -> {int(size[1]) + 1} rows, chunk recomp_{chunk}.c"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--canonical", type=Path, required=True, help="canonical lifted gen directory (read only)"
    )
    parser.add_argument(
        "--fresh",
        type=Path,
        required=True,
        help="gen directory of a fresh lift that has the function",
    )
    parser.add_argument(
        "--out", type=Path, required=True, help="directory to create (replaced when present)"
    )
    parser.add_argument("--address", required=True, help="function address, for example 0x00191B10")
    parser.add_argument(
        "--chunk", default="9000", help="chunk number of the new file (default 9000)"
    )
    arguments = parser.parse_args()
    graft(
        arguments.canonical,
        arguments.fresh,
        arguments.out,
        int(arguments.address, 0),
        arguments.chunk,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
