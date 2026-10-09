# SPDX-License-Identifier: GPL-3.0-or-later
"""Function to subsystem table and the decomp-tree coverage report.

    python -m tools.subsystems seed   [--xbe PATH] [--generated-dir DIR] [--no-callgraph]
    python -m tools.subsystems report [--tree] [--format text|csv|json] [--xbe PATH] [--write]
    python -m tools.subsystems check  [--xbe PATH] [--generated-dir DIR]

See `tools/subsystems/rules.py` for the placement rules, `anchors.py` for the address anchors,
`callgraph.py` for the optional propagation and `docs/subsystem-coverage.md` for the output.
"""
