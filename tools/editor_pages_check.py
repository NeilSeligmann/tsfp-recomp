# SPDX-License-Identifier: GPL-3.0-or-later
"""Private-input original-byte acceptance checks for T1714; run from repo root."""

import csv
import hashlib
import re
from pathlib import Path

import capstone

from tools.editor_pages import catalogue
from tools.frontend_labels import load
from tools.xppscan.image import Image

root = Path.cwd()
xbe = Path("/workspace/build/default.xbe")
iso = Path("/workspace/discs/tsfp-xbox.iso")
im = Image(xbe)
labels = load(iso)
assert (
    hashlib.sha256(im.raw).hexdigest()
    == "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
)
text = (root / "docs/t1714-editor-pages.md").read_text()
assert catalogue(root, xbe, iso) == text
entries = {int(x, 16) for x in re.findall(r"^### 0x([0-9A-F]+):", text, re.M)}
assert len(entries) == 60
assert [im.u32(0x52D464 + 4 * i) for i in range(4)] == [0x313010, 0x311AE0, 0, 0]
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
assert [(i.mnemonic, i.op_str) for i in cs.disasm(im.read(0x316B61, 17), 0x316B61)][:6] == [
    ("neg", "eax"),
    ("push", "0"),
    ("sbb", "eax, eax"),
    ("push", "0x42480000"),
    ("and", "eax, 5"),
    ("add", "eax, 3"),
]
assert [im.u32(0x52DAC0 + 12 * i) for i in range(8)] == [
    0xA2F,
    0xA32,
    0xA30,
    0xA31,
    0xA33,
    0xA34,
    0xA35,
    0xA36,
]
assert [im.u32(0x52DB20 + 12 * i) for i in range(3)] == [0xA2F, 0xA32, 0xA30]
assert [
    int.from_bytes(im.read(0x4D1F78 + 32 * im.u32(0x52D1D8 + 36 * i) + 0x18, 2), "little")
    for i in range(15)
] == [
    0x16F6,
    0x16C9,
    0x16C5,
    0x16CF,
    0x16C3,
    0x16C1,
    0x16D1,
    0x16DF,
    0x16E1,
    0x16D8,
    0x16D3,
    0x16C7,
    0x16CB,
    0x16DB,
    0x16E3,
]
assert [im.u32(0x52EBB8 + 20 * i) for i in range(10)] == [
    0x771,
    0x7F4,
    0x873,
    0x879,
    0x899,
    0x8AE,
    0x8F0,
    0x93D,
    0,
    0,
]
for table in (0x75FC08, 0x75FD48):
    s = im.sections[".data"]
    assert table >= s.virtual_addr + s.raw_size
assert labels.text(0xAD5) == "Map Settings" and labels.text(0xADB) == "Editor Settings"
assert im.u32(0x52EC78) == 0x337ED0
names = {
    int(r["entry_va"], 0): r["name"] for r in csv.DictReader(open("tools/data/function_names.csv"))
}
adds = list(csv.DictReader(open("tools/data/function_additions.csv")))
starts = sorted(set(names) | {int(r["entry_va"], 0) for r in adds})
audit = 0
for ix, a in enumerate(starts[:-1]):
    if not (0x312000 <= a < 0x338200 or names.get(a, "").startswith("game_mapedit_")):
        continue
    for ins in cs.disasm(im.read(a, starts[ix + 1] - a), a):
        if ins.mnemonic == "int3":
            break
        if ins.mnemonic == "call" and ins.op_str == "0x75b70":
            assert a in entries, hex(a)
            audit += 1
for p in ["docs/t1714-editor-pages.md", "docs/t1714-editor-page-contracts.md"]:
    for link in re.findall(r"\]\(([^)]+)\)", Path(p).read_text()):
        assert (Path(p).parent / link).exists(), link
print(
    "PASS 60 reproducible records;",
    audit,
    "independently audited header callers; original table/descriptor/BSS assertions and links",
)
print(
    "Bank hashes",
    hashlib.sha256(labels.first).hexdigest(),
    hashlib.sha256(labels.second).hexdigest(),
)
