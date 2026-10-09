# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutations for the fabricated FILE_OBJECT bodies and ordinal 76.

Both defects are the silent kind the guest-shaped replay exists for: the guest
performs the double dereference ITSELF out of guest memory, so a body whose
extension holds the right values at the wrong offsets, or answers the wrong
filesystem name, produces a plausible-looking sort key of 0 instead of the
file's sector and no error anywhere.
"""

MUTATIONS = [
    {
        "id": "file-object-gdfx-start-sector-at-the-wrong-offset",
        "file": "src/xbox/kernel_file_object.c",
        "old": "    ok = ok && kernel_guest_write_u32("
        "body + KERNEL_FILE_OBJECT_EXTENSION_OFFSET + 0u,\n"
        "                                      first_dword);",
        "new": "    ok = ok && kernel_guest_write_u32("
        "body + KERNEL_FILE_OBJECT_EXTENSION_OFFSET + 4u,\n"
        "                                      first_dword);",
        "targets": ["test_kernel_file_object"],
        "why": "the GDFX arm of the wrapper at 0x37D14F reads [[FileObject+8]] -- the "
        "extension's FIRST dword. A start sector written four bytes further still "
        "builds, still answers ordinal 76 correctly, and still dereferences "
        "cleanly; the guest just reads the zero the block was initialised with "
        "and every pak file gets sort key offset/2048. Only the replay's read of "
        "the NONZERO synthetic sector at the guest's own offset can see it.",
    },
    {
        "id": "file-object-disc-answers-the-wrong-name",
        "file": "src/xbox/kernel_file_object.c",
        "old": "        fs_name = KERNEL_FILE_OBJECT_FS_NAME_GDFX;\n"
        "        first_dword = open.disc_sector;",
        "new": "        fs_name = KERNEL_FILE_OBJECT_FS_NAME_FATX;\n"
        "        first_dword = open.disc_sector;",
        "targets": ["test_kernel_file_object"],
        "why": "the name dword at FsInformation+0xC is what steers the guest between "
        "[[p+8]] and the flag-gated [[p+8]+0x1C]. Answering FATX for a disc file "
        "is not an error anywhere: ordinal 76 succeeds, the guest takes the FATX "
        "arm, reads the disc sector's LOW BYTE as a flag (bit 0 of 40 is clear) "
        "and then the zero at +0x1C. The sort key silently loses the sector. "
        "Killed by the replay's name-dword and key assertions.",
    },
]
