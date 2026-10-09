# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for bounded DbgPrint formatting (T468)."""

_T = ["test_kernel_dbg"]
MUTATIONS = [
    {
        "id": "kdbg-recognised-spec-advances-format",
        "file": "src/xbox/kernel_dbg.c",
        "old": "        case '%':\n            put_char(out, out_size, &used, '%');\n            scanned++;\n            break;",  # noqa: E501
        "new": "        case '%':\n            put_char(out, out_size, &used, '%');\n            break;",  # noqa: E501
        "targets": _T,
        "why": "A recognized conversion must consume its specifier byte; otherwise the specifier is emitted again as literal text after formatting the argument.",  # noqa: E501
    },
    {
        "id": "kdbg-unknown-spec-no-argument",
        "file": "src/xbox/kernel_dbg.c",
        "old": "            put_char(out, out_size, &used, '%');\n            break;",
        "new": "            (void)next_arg(frame, &cursor, &value);\n            put_char(out, out_size, &used, '%');\n            break;",  # noqa: E501
        "targets": _T,
        "why": "Unknown format syntax is raw passthrough and must not consume an argument, or later valid conversions read the wrong stack slot.",  # noqa: E501
    },
    {
        "id": "kdbg-null-string-marker",
        "file": "src/xbox/kernel_dbg.c",
        "old": '            } else if (value == 0u) {\n                put_text(out, out_size, &used, "(null)");',  # noqa: E501
        "new": '            } else if (value == 0u) {\n                put_text(out, out_size, &used, "");',  # noqa: E501
        "targets": _T,
        "why": "A null %s pointer needs an explicit marker so malformed guest data is visible instead of silently disappearing.",  # noqa: E501
    },
    {
        "id": "kdbg-lowercase-pointer-width",
        "file": "src/xbox/kernel_dbg.c",
        "old": '"0x%08x", value);',
        "new": '"0x%x", value);',
        "targets": _T,
        "why": "The supported %p form is a fixed eight-digit guest pointer; omitting zero padding obscures its 32-bit address representation.",  # noqa: E501
    },
    {
        "id": "kdbg-bugcheck-code-forwarded",
        "file": "src/xbox/kernel_dbg.c",
        "old": '                     "KeBugCheck(0x%08X), the title halted itself", code);',
        "new": '                     "KeBugCheck(0x%08X), the title halted itself", code + 1u);',
        "targets": _T,
        "why": "KeBugCheck must report the exact fatal code supplied by the guest; changing it destroys the only evidence for the title's stop.",  # noqa: E501
    },
]
