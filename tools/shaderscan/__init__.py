# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure what feeds the title's shader pipeline, from the retail XBE.

`docs/d3d8-usage.md` found that the title assembles shaders at run time through
`XGAssembleShader` and that fixed-function transform is not linked in. Whether that
means a shader COMPILER or a bounded set of shaders translated offline depends on
what goes into the assembler, which is what this package measures:

    callargs    follow the argument pushes at each call site and classify each value,
                including every path into a shared call
    image       section-aware reads of the XBE, telling initialised bytes from BSS
    vsh         decode and validate NV2A vertex-program microcode, counts only
    builders    run the title's own shader-source builders under emulation
    assemble    run the title's own XGAssembleShader under emulation
    seh         x86 exception dispatch (RtlRaiseException, RtlUnwind) for that emulation, opt in
    catch_path  measure what the original returns when its C++ throw is caught
    xrefs       who references an address, and what the code does to a global
    disc        scan a game disc for vertex programs, inflating pak entries
    combiners   count the register-combiner configurations the title can install
    cli         `python -m tools.shaderscan.cli`

Read-only on every input. Prints counts, structure and addresses, never the bytes of
a shader or a source string. See `docs/shader-inputs.md` for the findings.
"""
