# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1153: whole-process snapshot and resume of the host (docs/state-snapshot.md).

`python -m tools.snapshot build-dmtcp` builds the pinned, patched DMTCP into the gitignored `tmp/dmtcp`.
`python -m tools.play ... --snapshot-at-poll N` takes a snapshot, `--resume-snapshot DIR` resumes one.
A snapshot is valid only for the exact host binary and shared libraries it was taken on.
"""
