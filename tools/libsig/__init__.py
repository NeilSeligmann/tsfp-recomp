# SPDX-License-Identifier: GPL-3.0-or-later
"""Full-body, relocation-masked fingerprinting of XDK library functions (T272).

The XDK `.lib` archives are Microsoft material, kept LOCALLY under
`tmp/xbox-sdk-ref/` for reference only. This package is code only: every signature
database and every match output is written under the gitignored `tmp/` or
`generated/` trees (enforced by `tools.libsig.guard`), and the tests build their own
synthetic COFF archives. See `docs/provenance.md` and `docs/libsig.md`.
"""
