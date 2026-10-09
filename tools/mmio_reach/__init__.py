# SPDX-License-Identifier: GPL-3.0-or-later
"""T59 probe: does a boot still run original library code that touches NV2A MMIO?

Three independent pieces of evidence, kept separate because they fail differently:

  census      STATIC. Which lifted functions mention an NV2A register address, and how
              each is reached from game code (callers walked up from the binary's own
              call table, stopping at an intercepted address).
  instrument  MEASURED at run time. A copy of the lifted tree with one entry marker per
              function, so a boot reports exactly which original bodies executed.
  boot        MEASURED. Runs the host, parses the stop and the ordered XDK trace.

`python -m tools.mmio_reach --help`.
"""
