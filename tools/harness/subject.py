# SPDX-License-Identifier: GPL-3.0-or-later
"""The subject: the compiled lifted C, driven over a line protocol.

`tools/harness/driver.c` identity-maps the guest window, loads the same flat image the
oracle holds in memory, and runs one lifted function per case. This module speaks to
it and reduces each reply to an `ExecResult`, so that `compare.py` cannot tell which
side produced which result.

The protocol is line-based and synchronous:

    -> CASE <va> <eax> <ecx> <edx> <ebx> <esp> <ebp> <esi> <edi> <df>
    -> PATCH <addr> <HEXBYTES>          (zero or more)
    -> RUN
    <- REGS <eax> <ecx> <edx> <ebx> <esp> <ebp> <esi> <edi>
    <- SEHEBP <value>                   (the lifter's second frame pointer)
    <- FLAGS <eflags> <mask>            (opt-in EFLAGS publication; mask 0 = unpublished)
    <- W <addr> <HEXBYTES>              (zero or more, ascending)
    <- END
    or
    <- FAULT <KIND>
    <- END

The subject's READY line also carries the provenance of the lifted tree it was built
from, which is read here and travels with every verdict. `results.csv` once recorded
the seed and the case index but not which tree produced the numbers, and three
successive wrong answers were published and retracted because of it.

A hang is handled rather than waited out. A lifted function can loop forever where the
oracle merely exhausted its instruction budget, and one such function in a run of
thousands would otherwise cost the entire run. The read is bounded, and a subject that
misses its deadline is killed, reported as a `TIMEOUT` fault, and replaced -- a timeout
is a failure to be recorded, never something to retry quietly.
"""

from __future__ import annotations

import queue
import subprocess
import threading
from pathlib import Path
from types import TracebackType

from .callstub import StubPlan
from .fault_diagnostics import FaultObservation
from .model import FP_SCALAR_MODE, REG_NAMES, Case, ExecResult, Reachability
from .provenance import Provenance
from .x87_state import decode_record, encode_record

#: Still generous: a tier-1 function has no calls and should complete in microseconds, so
#: anything near this is looping. Kept low because the recovery path is expensive -- a
#: timeout costs the deadline plus a 64 MB process restart and a 16 MB image reload.
DEFAULT_CASE_TIMEOUT = 5.0
DEFAULT_STARTUP_TIMEOUT = 60.0


class SubjectError(RuntimeError):
    """The subject process could not be started or spoke something unparseable."""


class SubjectProcess:
    """A running `driver.c`, restarted transparently when a case hangs or it dies."""

    def __init__(
        self,
        binary: Path,
        image: Path,
        *,
        case_timeout: float = DEFAULT_CASE_TIMEOUT,
        startup_timeout: float = DEFAULT_STARTUP_TIMEOUT,
    ) -> None:
        self._binary = Path(binary)
        self._image = Path(image)
        self._case_timeout = case_timeout
        self._startup_timeout = startup_timeout
        self._proc: subprocess.Popen[str] | None = None
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._eof = threading.Event()
        self._reader: threading.Thread | None = None
        #: Counted and reported: a high restart count means the results are thinner
        #: than the case count suggests.
        self.restarts = 0
        self.timeouts = 0
        #: Cases where the subject process died rather than missing its deadline. Counted
        #: separately because they are different failures with different causes, and
        #: pooling them hid every crash inside the timeout figure.
        self.deaths = 0
        #: Whether the linked lifted objects route their calls through the stub. Taken
        #: from the subject's own READY line, which `build_subject.sh` derives from the
        #: objects by inspection, so the harness can refuse to run a stubbed selection
        #: against a subject that would execute the real callees instead.
        self.supports_stubs = False
        self.supports_stackprobe = False
        #: Whether the linked lifted tree publishes EFLAGS (a --publish-eflags lift).
        #: Derived by build_subject.sh from the generated header by inspection and
        #: reported on the READY line, for the same reason stub= is: a hand-set flag
        #: is how a flags run ends up comparing a subject that publishes nothing.
        self.supports_eflags = False
        #: Which lifted tree this binary was built from, read from its own READY line.
        #: Taken from the subject rather than from a flag, because the failure this
        #: guards against is a human believing a subject measures the current tree.
        self.provenance = Provenance()
        #: T1508: (kind letter, eip, step count) of the last CWSTOP block, else None. Only a
        #: CODEWRITE=1 subject ever prints one, so every other proof never sees a value.
        self.last_codewrite_stop: tuple[str, int, int] | None = None
        self._start()

    def _start(self) -> None:
        self._proc = subprocess.Popen(
            [str(self._binary), str(self._image)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        # The queue and the EOF flag are created here and handed to the pump as
        # ARGUMENTS rather than read off `self` inside it. A restart replaces both, and a
        # pump thread that reached for `self._lines` would push its own end-of-stream
        # marker into the NEW queue after the replacement -- so the first read of the
        # fresh subject would consume a dead one's EOF and be scored as a failure.
        self._lines = queue.Queue()
        self._eof = threading.Event()
        self._reader = threading.Thread(
            target=self._pump, args=(self._proc, self._lines, self._eof), daemon=True
        )
        self._reader.start()
        ready = self._readline(self._startup_timeout)
        if ready is None or not ready.startswith("READY"):
            raise SubjectError(f"subject did not report READY, got {ready!r}")
        self.provenance = Provenance.from_ready_line(ready)
        # Parsed as a field rather than searched for as a substring: `gen_dir` runs
        # to the end of the line, so a path that happened to contain the text
        # "stub=1" would otherwise make an unstubbed subject claim it could stub.
        head = ready.split(" gen_dir=", 1)[0]
        self.supports_stubs = "stub=1" in head.split()
        self.supports_stackprobe = "stackprobe=1" in head.split()
        self.supports_eflags = "eflags=1" in head.split()
        self.supports_x87 = "x87=1" in head.split()
        self.supports_vector = "vector=1" in head.split()
        self.supports_real_mxcsr = "mxcsr=1" in head.split()
        self.supports_raw_x87 = "x87raw=1" in head.split()
        #: T1508: the opt-in code-write stop channel (CODEWRITE=1 builds only).
        self.supports_codewrite = "codewrite=1" in head.split()

    @staticmethod
    def _pump(
        proc: subprocess.Popen[str],
        lines: queue.Queue[str | None],
        eof: threading.Event,
    ) -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.put(line.rstrip("\n"))
        # Set BEFORE the sentinel, so a consumer that has just taken the sentinel always
        # sees the flag already set and can tell death from a deadline.
        eof.set()
        lines.put(None)

    def _readline(self, timeout: float) -> str | None:
        try:
            return self._lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def _restart(self) -> None:
        if self._proc is not None:
            self._proc.kill()
            try:
                self._proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.restarts += 1
        self._start()

    def run(self, case: Case, plan: StubPlan | None = None) -> ExecResult:
        """Execute one case, restarting the subject if it hangs or dies mid-case.

        `plan` carries the callee stub table the ORACLE computed. The subject is told
        the table rather than deriving one, so both sides stub the same callees with the
        same stack discipline by construction.
        """
        try:
            self._send(case, plan)
        except (BrokenPipeError, OSError) as error:
            self._restart()
            if case.x87_state is not None:
                raise SubjectError("raw x87 producer died before complete observation") from error
            return ExecResult(fault="SUBJECT-DIED")
        return self._read_block(require_raw_x87=case.x87_state is not None)

    def _send(self, case: Case, plan: StubPlan | None = None) -> None:
        assert self._proc is not None and self._proc.stdin is not None
        parts = [f"{case.va:X}"] + [f"{value:X}" for value in case.regs] + [f"{case.df:X}"]
        payload = ["CASE " + " ".join(parts)]
        if case.xmm is not None:
            if not self.supports_vector:
                raise SubjectError("subject lacks complete vector-state support")
            payload.append(
                f"XMMIN {case.mxcsr:04X} " + " ".join(f"{value:032X}" for value in case.xmm)
            )
            if case.vector_mode == FP_SCALAR_MODE:
                # T1620: the replacement must run on the real MXCSR, not the legacy echo.
                if not self.supports_real_mxcsr:
                    raise SubjectError("subject lacks real-MXCSR support (fp-scalar-v1)")
                payload.append("MXCSRREAL")
        if case.x87_state is not None:
            if not self.supports_raw_x87:
                raise SubjectError("subject lacks faithful raw x87 capability")
            payload.append(encode_record(case.x87_state))
        if case.fp_stack:
            if not self.supports_x87:
                raise SubjectError("subject lacks x87 case support (READY x87=0)")
            from .x87 import double_bits

            bits = " ".join(f"{double_bits(v):016X}" for v in case.fp_stack)
            payload.append(f"FPIN {case.fp_control:X} {len(case.fp_stack)} {bits}")
        payload += [f"PATCH {addr:X} {blob.hex().upper()}" for addr, blob in case.patches]
        if plan is not None:
            payload += [f"STUB {va:X} {pop:X}" for va, pop in plan.table]
            if plan.passthrough_table and not self.supports_stackprobe:
                raise SubjectError("subject lacks verified stack-probe passthrough support")
            payload += [f"PASS {va:X}" for va in plan.passthrough_table]
            # Sent even when the table is empty, so that a call-bearing function whose
            # callees somehow failed to be enumerated voids its cases loudly rather than
            # quietly running the real ones.
            payload.append("STUBMODE 1")
        payload.append("RUN")
        self._proc.stdin.write("\n".join(payload) + "\n")
        self._proc.stdin.flush()

    def _read_block(self, *, require_raw_x87: bool = False) -> ExecResult:
        regs: tuple[int, ...] | None = None
        writes: dict[int, int] = {}
        fault: str | None = None
        fault_addr: int | None = None
        stubs_applied: int | None = None
        passthrough_applied = 0
        seh_ebp: int | None = None
        replaced: bool | None = None
        flags: int | None = None
        flags_mask: int | None = None
        fp: tuple[int, ...] | None = None
        fp_control: int | None = None
        fp_status: int | None = None
        fp_status_mask: int | None = None
        fp_ext: tuple[tuple[int, int], ...] | None = None
        xmm: tuple[int, ...] | None = None
        mxcsr: int | None = None
        raw_x87 = None
        self.last_codewrite_stop = None
        while True:
            line = self._readline(self._case_timeout)
            if line is None:
                # Death and a missed deadline are DIFFERENT failures and were previously
                # reported identically. A subject killed by an unhandled signal closes its
                # stdout at once, so the read returns immediately with nothing -- and was
                # counted as a case timeout, which inflates the timeout figure and hides a
                # crash behind "the lifted code looped". The EOF flag tells them apart.
                died = self._eof.is_set()
                if died:
                    self.deaths += 1
                else:
                    self.timeouts += 1
                self._restart()
                if require_raw_x87:
                    raise SubjectError("raw x87 producer ended before complete observation")
                return ExecResult(fault="SUBJECT-DIED" if died else "TIMEOUT")
            if require_raw_x87 and line.startswith(("FPOUT ", "FPCW ", "FPSTATUS ", "FPEXT ")):
                raise SubjectError("legacy FP output conflicts with raw x87 request")
            if line == "END":
                break
            if line.startswith("REGS "):
                values = line.split()[1:]
                if len(values) != len(REG_NAMES):
                    raise SubjectError(f"REGS had {len(values)} values: {line!r}")
                regs = tuple(int(v, 16) for v in values)
            elif line.startswith("X87RAW"):
                if raw_x87 is not None or not require_raw_x87:
                    raise SubjectError("duplicate or unsolicited raw x87 record")
                try:
                    raw_x87 = decode_record(line, output=True)
                except ValueError as error:
                    raise SubjectError("malformed raw x87 record") from error
            elif line.startswith("FPOUT "):
                # `FPOUT <depth> <double bits>...`, top first.
                fields = line.split()[1:]
                if len(fields) != int(fields[0]) + 1:
                    raise SubjectError(f"FPOUT depth does not match its values: {line!r}")
                fp = tuple(int(v, 16) for v in fields[1:])
            elif line.startswith("FPCW "):
                fp_control = int(line.split()[1], 16)
            elif line.startswith("FPSTATUS "):
                fields = line.split()
                if len(fields) != 3:
                    raise SubjectError(f"malformed FPSTATUS line: {line!r}")
                fp_status = int(fields[1], 16)
                fp_status_mask = int(fields[2], 16)
            elif line.startswith("FPEXT "):
                # `FPEXT <depth> <mantissa>:<sign+exponent>...`, top first.
                fields = line.split()[1:]
                if len(fields) != int(fields[0]) + 1:
                    raise SubjectError(f"FPEXT depth does not match its values: {line!r}")
                fp_ext = tuple(
                    (int(pair.split(":")[0], 16), int(pair.split(":")[1], 16))
                    for pair in fields[1:]
                )
            elif line.startswith("XMMOUT "):
                fields = line.split()
                if len(fields) != 10 or xmm is not None:
                    raise SubjectError("malformed or duplicate XMMOUT")
                try:
                    mxcsr = int(fields[1], 16)
                    xmm = tuple(int(word, 16) for word in fields[2:])
                except ValueError as error:
                    raise SubjectError("malformed XMMOUT hex") from error
                if not 0 <= mxcsr <= 0xFFFF or any(not 0 <= v < 1 << 128 for v in xmm):
                    raise SubjectError("XMMOUT value out of range")
            elif line.startswith("CWSTOP "):
                _, kind_text, eip_text, steps_text = line.split()
                self.last_codewrite_stop = (kind_text, int(eip_text, 16), int(steps_text))
            elif line.startswith("W "):
                _, addr_text, hex_text = line.split()
                addr = int(addr_text, 16)
                for i, byte in enumerate(bytes.fromhex(hex_text)):
                    writes[addr + i] = byte
            elif line.startswith("STUBS "):
                stubs_applied = int(line.split()[1])
            elif line.startswith("PASSES "):
                passthrough_applied = int(line.split()[1])
            elif line.startswith("SEHEBP "):
                seh_ebp = int(line.split()[1], 16)
            elif line.startswith("FLAGS "):
                # `FLAGS <eflags> <mask>`: the published flags word and which of
                # its bits the executed exit's model actually answered. Only a
                # --publish-eflags subject ever writes a nonzero mask; the driver
                # resets both to 0 before every case so a stale publish from a
                # previous case can never be read as this one's.
                _, flags_text, mask_text = line.split()
                flags = int(flags_text, 16)
                flags_mask = int(mask_text, 16)
            elif line == "REPL 1":
                replaced = True
            elif line.startswith("FAULTADDR "):
                fault_addr = int(line.split()[1], 16)
            elif line.startswith("FAULT "):
                fault = line.split(maxsplit=1)[1].strip()
            elif line.startswith("FATAL"):
                raise SubjectError(line)
            # Anything else is driver chatter and is ignored deliberately; it cannot
            # affect a verdict because a verdict needs either REGS or FAULT.
        if require_raw_x87 and raw_x87 is None:
            raise SubjectError("missing required raw x87 record")
        if fault is not None:
            return ExecResult(
                fault=fault,
                fault_addr=fault_addr,
                x87_state=raw_x87,
                fault_observation=FaultObservation("subject-legacy-unavailable", fault),
            )
        if regs is None:
            raise SubjectError("block ended with neither REGS nor FAULT")
        return ExecResult(
            regs=regs,
            x87_state=raw_x87,
            writes=writes,
            # Carried only when this subject can publish at all: a FLAGS line from a
            # non-publishing tree is always `0 0`, and turning that into a flags word
            # would read as "published nothing" on every case of a subject that was
            # never asked to publish.
            flags=flags if self.supports_eflags else None,
            flags_mask=flags_mask if self.supports_eflags else None,
            reach=Reachability(
                stub_applied=stubs_applied or 0, passthrough_applied=passthrough_applied
            ),
            seh_ebp=seh_ebp,
            replaced=replaced,
            fp=fp,
            fp_control=fp_control,
            fp_status=fp_status,
            fp_status_mask=fp_status_mask,
            fp_ext=fp_ext,
            xmm=xmm,
            mxcsr=mxcsr,
        )

    def list_replacements(self) -> list[str]:
        """The `REPL ...` lines the subject prints for its linked hand-written replacements.

        Empty for a subject with none. This is the registry of the BINARY THAT RUNS, not a
        scan of the sources, which is the only thing a proof is allowed to rest on.
        """
        assert self._proc is not None and self._proc.stdin is not None
        self._proc.stdin.write("LISTREPL\n")
        self._proc.stdin.flush()
        lines: list[str] = []
        while True:
            line = self._readline(self._startup_timeout)
            if line is None:
                raise SubjectError("the subject did not answer LISTREPL")
            if line == "END":
                return lines
            if line.startswith("REPL "):
                lines.append(line)
            elif line.startswith("ERROR "):
                raise SubjectError(f"invalid LISTREPL response: {line!r}")

    def close(self) -> None:
        if self._proc is None:
            return
        try:
            if self._proc.stdin is not None:
                self._proc.stdin.close()
            self._proc.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            self._proc.kill()
        self._proc = None

    def __enter__(self) -> SubjectProcess:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()
