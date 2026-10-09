"""Template for a mutation set. Copy this file, do not edit it.

ONE FILE PER OWNER. `tools/mutate/c_suites.py` loads every `*.py` here except those
starting with an underscore, concatenates their `MUTATIONS` lists, and refuses to run if
two sets share a mutation id. That is so several concurrent tasks can each add coverage
without editing the same bytes, which has previously cost a lost mutation -- and a lost
mutation looks exactly like one that was never written.

Each entry needs:
    id        unique across ALL sets, kebab-case, prefixed with your area
    file      path relative to the repo root
    old/new   exact text to swap. `old` must appear EXACTLY ONCE or the run reports
              ANCHOR-DRIFT rather than silently doing nothing
    targets   ctest binaries to rebuild and run
    why       the argument for why a survivor would matter, written BEFORE the run so a
              survivor cannot be rationalised afterwards

Two traps worth knowing, both of which have bitten here:
  - A mutation that fails to compile scores NOT-A-MUTANT, which READS like evidence while
    meaning the mutation was never injected. `-Wunused-parameter -Werror` is the usual
    cause, so write `if (cond && false)` rather than `if (false)`.
  - `tsfp_host` is NOT a ctest binary, so no mutation can reach host-only code. If your
    change lives there, say so in the `why` instead of pretending coverage.
  - IF YOU HAND-MUTATE PYTHON rather than using this harness, clear `__pycache__` between
    mutations. CPython validates bytecode on source mtime TO THE SECOND plus size, so two
    mutations of equal size written within the same second REUSE THE FIRST ONE'S
    BYTECODE -- and a mutation that was never loaded looks exactly like a survivor. This
    harness clears `__pycache__` beside any `.py` entry on every write (mutate AND
    restore, see `clear_stale_bytecode` in `c_suites.py`), but a task writing its own
    Python sweep gets no such protection and has already been caught by it.
"""

MUTATIONS: list[dict] = []
