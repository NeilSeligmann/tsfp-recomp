# SPDX-License-Identifier: GPL-3.0-or-later
"""An explicit, validating model of the DSP command mailbox the retail DSOUND posts to (T373).

WHAT THE ORIGINAL DOES (measured, see docs/audio-input-recovery.md "DSP command mailbox"). The
library keeps a 6-dword header at `scratch + 0x800` of the DSP's contiguous scratch allocation.
Dword 4 of it (`scratch + 0x810`) is the command word. The library posts a command by storing it
there and the DSP is expected to clear it. Exactly two loops in the whole library read the word:

    0x0040A22C  cmp [ebx], 0 / jne    after posting command 3 (download), ebx = the word's address
    0x0040A28F  cmp [eax+0x10], 0 / jne   before posting command 2 (commit), eax = header base

Command 3 (DownloadEffectsImage) waits for the clear. Command 2 is posted by the commit path
(CommitDeferredSettings, `0x40AD22` then `0x40A261`) while the settings dirty mask is set. It waits
for idle BEFORE posting and does not wait after, so a posted 2 stays pending until the next poll.
No other command was seen on any path the oracle reached.

WHAT THIS MODEL IS. A stand-in for hardware that is not emulated. It is installed explicitly, it
logs every command it serves, and it never clears a word it does not recognise. It validates only
what is observable in guest memory and what the DSP could not run without:

* command 3: header dwords 1 and 3 (code and state sizes in 32-bit slots) are nonzero and the
  code and state regions that follow the header are 24-bit words (high byte zero), which is the
  DSP56300 word size. The library decrypts the code on the CPU, so a failed decrypt shows up here.
* command 2: recorded as posted, fields kept. Their meaning is not derived, so nothing is asserted.
* any other value: refused with `OracleError`. No implicit acknowledgement.

WHAT IT IS NOT. It does not execute the effects code, produces no audio and says nothing about
what the real DSP firmware validates. The acknowledgement is FABRICATED hardware behaviour, the
checks are this repository's own derivation.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from tools.dsoundscan.oracle import Oracle

HEADER_OFFSET = 0x800
HEADER_WORDS = 6
COMMAND_WORD_INDEX = 4
COMMAND_WORD_OFFSET = COMMAND_WORD_INDEX * 4
CODE_OFFSET = HEADER_OFFSET + HEADER_WORDS * 4

COMMAND_POST = 2
COMMAND_DOWNLOAD = 3

# Where each loop reads the word, as a function of the registers at the loop head.
POLL_DOWNLOAD = 0x0040A22C
POLL_POST = 0x0040A28F

WORD24_LIMIT = 1 << 24
# The original workspace is 0x60000 bytes, a header that declares more cannot be read back.
MAX_PAYLOAD_BYTES = 0x60000


class DspRefusal(RuntimeError):
    """The model declines to acknowledge: the command or the memory it names is not valid."""


@dataclass(frozen=True)
class ServedCommand:
    command: int
    header: tuple[int, ...]
    word_address: int
    poll_site: int


def _words(data: bytes) -> tuple[int, ...]:
    return struct.unpack(f"<{len(data) // 4}I", data)


def download_payload_bytes(header: tuple[int, ...]) -> int:
    """The code plus state size a command 3 header declares, or `DspRefusal` if unreadable."""
    code_words, state_words = header[1], header[3]
    if code_words == 0 or state_words == 0:
        raise DspRefusal(
            f"command 3 declares an empty region (code {code_words}, state {state_words})"
        )
    declared = (code_words + state_words) * 4
    if declared > MAX_PAYLOAD_BYTES:
        raise DspRefusal(
            f"command 3 declares {declared:#x} bytes, more than {MAX_PAYLOAD_BYTES:#x}"
        )
    return declared


def judge_download(header: tuple[int, ...], payload: bytes) -> None:
    """Raise `DspRefusal` unless a command 3 header and the bytes after it are acceptable.

    `payload` is the memory from `scratch + 0x818` for code plus state, in header-declared size.
    """
    expected = download_payload_bytes(header)
    if len(payload) != expected:
        raise DspRefusal(f"command 3 payload is {len(payload)} bytes, header declares {expected}")
    wide = [index for index, word in enumerate(_words(payload)) if word >= WORD24_LIMIT]
    if wide:
        raise DspRefusal(
            f"command 3 payload holds {len(wide)} words wider than 24 bits (first at word "
            f"{wide[0]}): the DSP56300 cannot run code that was not decrypted"
        )


class DspMailbox:
    """Serves the two mailbox polling loops of the original library under the oracle."""

    def __init__(self) -> None:
        self.served: list[ServedCommand] = []

    def install(self, oracle: Oracle) -> None:
        oracle.hook_pc(
            POLL_DOWNLOAD,
            lambda model: self.service(model, model.register("ebx"), POLL_DOWNLOAD),
        )
        oracle.hook_pc(
            POLL_POST,
            lambda model: self.service(
                model, model.register("eax") + COMMAND_WORD_OFFSET, POLL_POST
            ),
        )

    def service(self, oracle: Oracle, word_address: int, poll_site: int) -> None:
        """Called at a polling loop head: validate and clear a pending command, else do nothing."""
        command = oracle.read32(word_address)
        if command == 0:
            return
        base = word_address - COMMAND_WORD_OFFSET
        header = _words(oracle.read_bytes(base, HEADER_WORDS * 4))
        if command == COMMAND_DOWNLOAD:
            try:
                declared = download_payload_bytes(header)
                judge_download(header, oracle.read_bytes(base + HEADER_WORDS * 4, declared))
            except DspRefusal as refusal:
                from tools.dsoundscan.oracle import OracleError

                raise OracleError(f"DSP mailbox refused command 3: {refusal}") from refusal
        elif command != COMMAND_POST:
            from tools.dsoundscan.oracle import OracleError

            raise OracleError(
                f"DSP mailbox: unknown command {command:#x} at {word_address:#010x}, "
                "not acknowledged (no implicit acknowledgement)"
            )
        self.served.append(ServedCommand(command, header, word_address, poll_site))
        oracle.write32(word_address, 0)
