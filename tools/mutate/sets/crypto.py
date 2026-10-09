# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for the Xc* crypto ordinals (335/336/337 SHA-1, 340 HMAC, 346 DES parity).

WHY THIS SET IS DIFFERENT FROM THE OTHERS HERE. Every other module in this repo is
checked against behaviour we inferred from the guest. SHA-1 and HMAC-SHA1 are PUBLISHED
STANDARDS with PUBLISHED TEST VECTORS, so `test_kernel_crypto` can be wrong in a way no
amount of self-consistency would reveal and a single vector settles. That cuts both
ways: it makes the kills below meaningful, and it means a SURVIVOR here is damning
rather than arguable, because the oracle is external.

EVERY MUTATION BELOW WAS CONFIRMED TO COMPILE AND TO CHANGE THE DIGEST before it was
written down, against a standalone driver over the FIPS 180-1 and RFC 2202 vectors. A
mutation nobody checked can be a NOT-A-MUTANT dressed as coverage.

ONE MUTATION THAT CANNOT EXIST, said plainly rather than faked. The `ABI_TABLE` rows
that give these five ordinals their stack-argument counts live in
`src/host/kernel_thunk.c`, and `tsfp_host` is not a ctest binary, so no mutation in this
file can reach them. A wrong count there desyncs the guest's `esp` permanently and this
set would not notice. That is covered by the hand-count evidence in `kernel_crypto.h`
and by nothing else.
"""

MUTATIONS: list[dict] = [
    {
        "id": "crypto-sha1-wrong-round-constant",
        "file": "src/xbox/kernel_crypto.c",
        "old": "#define SHA1_K2 0x8F1BBCDCu",
        "new": "#define SHA1_K2 0x8F1BBCDDu",
        "targets": ["test_kernel_crypto"],
        "why": "a wrong round constant still produces a well-formed 20-byte digest of "
        "the right length with the right avalanche, so every structural check and "
        "every self-consistency check passes. ONLY a published vector can tell the "
        "difference, which is the whole argument for testing against FIPS 180-1 "
        "rather than against our own output. The third constant is chosen because "
        "it is used in only 20 of the 80 rounds, so a suite that happened to test "
        "only short inputs would be the most likely to miss it.",
    },
    {
        "id": "crypto-hmac-inner-outer-pad-swapped",
        "file": "src/xbox/kernel_crypto.c",
        "old": """        inner_pad[i] = (uint8_t)(padded[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(padded[i] ^ 0x5Cu);""",
        "new": """        inner_pad[i] = (uint8_t)(padded[i] ^ 0x5Cu);
        outer_pad[i] = (uint8_t)(padded[i] ^ 0x36u);""",
        "targets": ["test_kernel_crypto"],
        "why": "swapping ipad and opad is the classic HMAC defect and it is INVISIBLE to "
        "every property a reasonable test would check without a vector: the result "
        "is still deterministic, still 20 bytes, still changes when the key or the "
        "message changes, and still agrees with itself across runs. It is also the "
        "mutation most likely to survive a suite that tested HMAC by hashing twice "
        "and comparing, because both halves would be swapped identically.",
    },
    {
        "id": "crypto-sha1-length-field-little-endian",
        "file": "src/xbox/kernel_crypto.c",
        "old": "tail[i] = (uint8_t)((bits >> (56u - 8u * i)) & 0xFFu);",
        "new": "tail[i] = (uint8_t)((bits >> (8u * i)) & 0xFFu);",
        "targets": ["test_kernel_crypto"],
        "why": "FIPS 180-1 specifies the trailing length field BIG-ENDIAN, and an x86 "
        "port is exactly where that gets written host-native by reflex. The defect "
        "is confined to the final block, so a streaming test that only checked "
        "that split updates agree with each other would still pass -- both paths "
        "would be wrong identically. Needs an external digest to catch.",
    },
    {
        "id": "crypto-sha1-final-block-padding-off-by-one",
        "file": "src/xbox/kernel_crypto.c",
        "old": """    while (state->pending != 56u) {
        size_t want = (state->pending < 56u) ? (56u - state->pending) : (120u - state->pending);""",
        "new": """    while (state->pending != 57u) {
        size_t want = (state->pending < 57u) ? (57u - state->pending) : (121u - state->pending);""",
        "targets": ["test_kernel_crypto"],
        "why": "one byte too much padding pushes the last byte of the 8-byte length "
        "field into a block that is never compressed, so the digest is taken from "
        "the chaining value one block early. The output is still 20 well-mixed "
        "bytes. Mutated CONSISTENTLY across the loop guard and the step, on "
        "purpose: changing the guard alone spins forever, and a hang is a "
        "different finding from a wrong answer.",
    },
    {
        "id": "crypto-sha1-schedule-not-rotated",
        "file": "src/xbox/kernel_crypto.c",
        "old": "w[i] = rotl32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);",
        "new": "w[i] = (uint32_t)(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u]);",
        "targets": ["test_kernel_crypto"],
        "why": "dropping the rotate in the message-schedule expansion is literally the "
        "difference between SHA-1 and SHA-0, which is a real algorithm that was "
        "really published and really superseded. A port derived from an old "
        "reference can produce exactly this. It is undetectable without a vector, "
        "because SHA-0 is a perfectly well-behaved hash.",
    },
    {
        "id": "crypto-sha1-length-counted-in-bytes",
        "file": "src/xbox/kernel_crypto.c",
        "old": "state->bits += (uint64_t)length * 8u;",
        "new": "state->bits += (uint64_t)length;",
        "targets": ["test_kernel_crypto"],
        "why": "FIPS 180-1 pads with the message length in BITS. Counting bytes is a "
        "one-character slip that leaves every other property intact and is "
        "invisible to any test that does not know the answer in advance.",
    },
    {
        "id": "crypto-des-parity-even-instead-of-odd",
        "file": "src/xbox/kernel_crypto.c",
        "old": "const uint8_t wanted = (uint8_t)((ones % 2u == 0u) ? 1u : 0u);",
        "new": "const uint8_t wanted = (uint8_t)((ones % 2u == 0u) ? 0u : 1u);",
        "targets": ["test_kernel_crypto"],
        "why": "DES key parity is ODD per byte (FIPS 46-3). An even-parity routine still "
        "only ever touches bit 0, still leaves all 56 key bits intact, and still "
        "looks like it is doing parity work -- so a test asserting merely that the "
        "high seven bits survived would pass. Pinned instead by the four PUBLISHED "
        "FIPS 74 weak keys, which are all already odd-parity-correct and so must "
        "come back BYTE-IDENTICAL.",
    },
    # ------------------------------------------------------------------ the glue.
    # The seven above attack the algorithm, which published vectors defend. These
    # five attack the guest boundary, where there is no external oracle at all and
    # the only defence is a test that knows the measured argument order.
    {
        "id": "crypto-335-bound-to-the-wrong-ordinal",
        "file": "src/xbox/kernel_crypto.c",
        "old": "#define ORD_XcSHAInit 335u",
        "new": "#define ORD_XcSHAInit 334u",
        "targets": ["test_kernel_crypto"],
        "why": "a perfect SHA-1 bound to the wrong number leaves 335 a STUB, and a stub "
        "returns a plausible default rather than failing, so the guest would hash "
        "nothing and compare it against a stored digest. Checking that the entry "
        "merely EXISTS would not catch this either, because every known ordinal "
        "exists as a stub from kernel_hle_init onward -- only asserting "
        "KERNEL_ENTRY_IMPLEMENTED does.",
    },
    {
        "id": "crypto-337-context-and-digest-swapped",
        "file": "src/xbox/kernel_crypto.c",
        "old": """    const kernel_guest_ptr ctx = args[0];
    const kernel_guest_ptr digest_out = args[1];""",
        "new": """    const kernel_guest_ptr ctx = args[1];
    const kernel_guest_ptr digest_out = args[0];""",
        "targets": ["test_kernel_crypto"],
        "why": "XcSHAFinal's two arguments are both guest pointers, so a swap is not a "
        "type error and the measured table cannot tell them apart -- only the "
        "0x00439897 evidence that argument 1 is a five-dword local can. Swapped, "
        "the handler writes 20 bytes of digest OVER the guest's context and looks "
        "for a context where the digest belongs. Nothing about the shape of the "
        "call reveals it.",
    },
    {
        "id": "crypto-336-data-and-length-swapped",
        "file": "src/xbox/kernel_crypto.c",
        "old": """    const kernel_guest_ptr data = args[1];
    const uint32_t length = args[2];""",
        "new": """    const kernel_guest_ptr data = args[2];
    const uint32_t length = args[1];""",
        "targets": ["test_kernel_crypto"],
        "why": "the (data, length) order is the thing the 0x00380684 literal 0x44 pins, "
        "and getting it backwards is the most natural way to be wrong about a "
        "three-argument hash update. A test that only checked the call SUCCEEDED "
        "would pass: the handler would still return STATUS_SUCCESS whenever the "
        "swapped length happened to address mapped memory.",
    },
    {
        "id": "crypto-stale-context-silently-restarted",
        "file": "src/xbox/kernel_crypto.c",
        "old": """        stale_context_count++;
        kernel_hle_log()("kernel: XcSHAUpdate given a context at %#x this host did not "
                         "initialise -- refusing rather than hashing from an unknown "
                         "state\\n",
                         (unsigned)ctx);
        return STATUS_INVALID_PARAMETER;""",
        "new": """        stale_context_count++;
        kernel_crypto_sha1_reset(&state);""",
        "targets": ["test_kernel_crypto"],
        "why": "THIS IS THE DEFECT THE HEADER ARGUES HARDEST AGAINST, injected so the "
        "argument is testable rather than merely stated. Quietly starting a fresh "
        "hash is the tempting repair and the worst available outcome: the guest "
        "receives a digest over a SUFFIX of its message, 0x00380684 compares it "
        "against the stored one, and the mismatch is reported as corrupt content "
        "rather than as our bug. Note it still increments the counter, so a test "
        "watching only the counter would be fooled.",
    },
    {
        "id": "crypto-340-second-segment-ignored",
        "file": "src/xbox/kernel_crypto.c",
        "old": "(size_t)second_length, out);",
        "new": "0u, out);",
        "targets": ["test_kernel_crypto"],
        "why": "the second message segment is the whole reason XcHMAC takes seven "
        "arguments instead of five, and 33 of the 40 measured sites pass 0 for it "
        "-- so a handler that ignored it would work at 33 sites and silently "
        "produce the wrong MAC at the other 7, including the XNet session-key KDF "
        "at 0x00434508 which feeds its output straight into 3DES. Exactly the kind "
        "of defect a majority-of-sites test would miss.",
    },
]
