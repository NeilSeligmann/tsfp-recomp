# SPDX-License-Identifier: GPL-3.0-or-later
"""A single measured leaf stack-probe entry that cannot use the ret-pop stub.

These original bytes preserve ECX/EDX and relocate the return slot while allocating
EAX bytes below the caller's ESP. Exact bytes and the reviewed lifted body are both
required; this grants no permission to execute another callee or interior entry.
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

STACKPROBE_VA = 0x003C9D90
STACKPROBE_SIZE = 65
STACKPROBE_ORIGINAL_SHA = "b77ad187f6f3db2a5ae4f8e214b023d7c62ce25a38ce769a993d2a8c81643760"
STACKPROBE_LIFTED_SHA = "a66f31767c54f8a6110d61bcdff719014d02d3f689299da56a71884820f09f7d"
STACKPROBE_COMPILE_POLICY = "O0-no-strict-aliasing-v1"

# These two original callers overwrite arithmetic flags after the probe before
# reading them. The lifted C frames do not share arithmetic flag mirrors, so no
# other caller is granted passthrough merely because the leaf itself is proved.
STACKPROBE_CALLERS = {
    0x00337BD0: (422, "d4f0269c5068d292a4da4f535ee1f1c2fb3b6aecb595f8b4b87aad61a7665ce2"),
    0x0038FE80: (256, "931de85e6ad46489970633a0e7a2e843db5824fc08669df2a1e00658ce4d30c0"),
}


def original_stackprobe_caller_proven(va: int, code: bytes) -> bool:
    expected = STACKPROBE_CALLERS.get(va)
    return (
        expected is not None
        and len(code) == expected[0]
        and hashlib.sha256(code).hexdigest() == expected[1]
    )


def original_stackprobe_proven(va: int, code: bytes) -> bool:
    return (
        va == STACKPROBE_VA
        and len(code) == STACKPROBE_SIZE
        and hashlib.sha256(code).hexdigest() == STACKPROBE_ORIGINAL_SHA
    )


def lifted_stackprobe_proven(gen_dir: Path) -> bool:
    definitions = []
    pattern = re.compile(r"^void sub_003C9D90\(void\)\n\{.*?^\}", re.M | re.S)
    for path in gen_dir.glob("recomp_*.c"):
        definitions.extend(match[0] for match in pattern.finditer(path.read_text()))
    return (
        len(definitions) == 1
        and hashlib.sha256(definitions[0].encode()).hexdigest() == STACKPROBE_LIFTED_SHA
    )


if __name__ == "__main__":
    import sys

    print(int(lifted_stackprobe_proven(Path(sys.argv[1]))))
