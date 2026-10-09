# SPDX-License-Identifier: GPL-3.0-or-later
"""Static dataflow traces that close two inferred-by-absence claims (T263).

    createevent   every direct caller of the CreateEventA wrapper `0x37FF30` in every
                  executable section, the four arguments each pushes, and the
                  NtCreateEvent arguments the title's own wrapper builds from them
    vertexkeys    what can reach the key argument of the vertex-key writers' sites
                  that pass a non-literal (T51/T97/T84c)
    code          every executable section decoded once, plus xref helpers
    flow          a per-function control-flow graph with a stack model and a value-set
                  resolver (registers and frame slots, backward over every path)
    emulate       the title's own wrapper code run in Unicorn on literal arguments
    cli           `python -m tools.tracegaps`

Read-only on the retail image. Prints addresses and small integers only. Every figure
is MEASURED (read off decoded code or produced by running the title's code) or reported
NOT DERIVABLE with the exact reason, never inferred from the absence of a hit.
"""
