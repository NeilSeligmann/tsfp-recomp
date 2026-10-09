# SPDX-License-Identifier: GPL-3.0-or-later
"""Replace lifted functions with hand-written C, and prove each replacement.

See docs/decompilation-workflow.md. The package has no third-party imports at module
level so `python3 -m tools.replace wire` runs from CMake with the system interpreter.
"""
