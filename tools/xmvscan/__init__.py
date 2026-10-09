# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only XMV container metadata; no decoder implementation."""

from tools.xmvscan.model import AudioTrack, Container, Packet
from tools.xmvscan.reader import parse

__all__ = ["AudioTrack", "Container", "Packet", "parse"]
