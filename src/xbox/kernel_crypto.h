/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Crypto ordinals: SHA-1 (335/336/337), RC4 (338/339), HMAC-SHA1 (340)
 * and DES key parity (346). T1114 RC4 state/alias behavior is independently
 * measured through synthetic xemu kernel calls; the historical T452 standards
 * provenance below applies to SHA/HMAC/DES, not a guessed RC4 kernel layout.
 *
 * ORDINAL NUMBERS, RESOLVED NOT RECALLED. 335 is XcSHAInit, 336 XcSHAUpdate, 337
 * XcSHAFinal, 340 XcHMAC and 346 XcDESKeyParity, read out of
 * `tools/kernel_ordinals.py` (the single source of truth, derived from the kernel
 * export table). NONE of them appears in that file's `SUSPECT_ON_XDK_5849` -- which
 * is currently empty -- nor in `RESOLVED_ON_XDK_5849`, whose only entry is 49. So no
 * Xc* ordinal has ever been flagged as drifting between XDK versions, and nothing
 * here needed settling from the image. That is a measured absence, not an oversight:
 * the file's own header still asks every unlisted entry to be read as a HYPOTHESIS
 * for 5849, and the corroboration that these five are right is the argument shapes
 * below, which match the names and nothing else.
 *
 * ===========================================================================
 * PROVENANCE: THIS IS CLEAN-ROOM FROM PUBLISHED STANDARDS.
 *
 * `docs/provenance.md` forbids Microsoft XDK headers, libraries and source outright,
 * and separately forbids `Empyreal96/xboxkrnl_chk_5455` -- IDA pseudocode of the real
 * `xboxkrnl.exe` -- while naming it as the source most likely to resolve exactly this
 * kind of unknown. It is named here for the same reason, and refused. No XDK header,
 * no leaked `.lib` FLIRT signature, no decompilation of Microsoft's own
 * implementation and no xboxdevwiki prose was consulted for any line of
 * `kernel_crypto.c`.
 *
 * It did not need to be, and that is what makes this module different from every
 * other kernel module here. The ALGORITHMS are public standards with published test
 * vectors:
 *
 *   - SHA-1 is FIPS 180-1 (also published as RFC 3174). The initial chaining values,
 *     the four round constants, the 80-round compression, the message-schedule
 *     expansion and the big-endian length padding all come from that document, and
 *     `tests/c/test_kernel_crypto.c` pins the result against the THREE vectors FIPS
 *     180-1 publishes in its appendices plus the empty-message digest.
 *   - HMAC is RFC 2104, with the 0x36 and 0x5C pad constants from its section 2, and
 *     the vectors from RFC 2202 section 3.
 *   - DES key parity is FIPS 46-3 / ANSI X3.92: the low bit of each byte is a parity
 *     bit chosen to give the byte ODD population count. Pinned by the four weak keys
 *     FIPS 74 publishes, all of which are already parity-correct and so must come
 *     back byte-identical.
 *
 * A clean-room implementation of a published standard is only as good as its oracle,
 * so the oracle is EXTERNAL BY CONSTRUCTION: every expected digest in the test suite
 * is a number printed in a standards document, not a number this code produced. A
 * suite that hashed something twice and compared would pass against a wrong SHA-1,
 * and `tools/mutate/sets/crypto.py` exists to demonstrate that it would not pass
 * here.
 *
 * WHAT IS NOT DERIVED FROM A STANDARD is the Xbox's own deviation from it, and that
 * is treated as an open question rather than filled in -- see the XcHMAC section.
 *
 * ===========================================================================
 * NONE OF THIS IS ON THE BOOT PATH. MEASURED, AND SAID PLAINLY.
 *
 * At the commit this module was written against, the boot reaches 30 kernel calls
 * with `--hdd` and stops on ordinal 279 `RtlEqualString` at guest 0x0037C958. The
 * observed ordinal sequence is
 *
 *     255 187 277 294 47 107 113 24 301 149 184 184 291 202
 *     218 187 67 190 187 289 190 187 67 190 187 289 190 289 289 279
 *
 * and ALL ELEVEN Xc* ordinals report `calls 0` in that run, as they do in the bare
 * run that stops on 49. So implementing these five UNBLOCKS NOTHING. They are
 * backlog-order work: 111 of the 361 remaining call sites, which is the largest
 * coherent cluster left, and that is the entire argument for doing them.
 *
 * HOW FAR AWAY, since "not on the path" invites the question. The SHA trio is
 * structurally THREE calls from where the boot sits. The boot is inside
 * `XapiInitProcess` (0x0038126B), whose call at 0x003813B0 is unconditional and
 * reaches `_XapipUpdateRebootIfNecessary` (0x003809FE), which calls
 * `_XapipUpdateDetectAndVerify` (0x00380684), which is the XcSHAInit /
 * XcSHAUpdate / XcSHAFinal site at 0x00380749 / 0x0038075A / 0x0038076A.
 *
 * BUT STRUCTURALLY CLOSE IS NOT THE SAME AS WILL-EXECUTE, and only the first of
 * those is measured. `_XapipUpdateRebootIfNecessary` has four early exits before it
 * reaches that call, two of which test the string returned by ordinal 326
 * `XeImageFileName` -- itself unimplemented. The branch is the XDK "is there a title
 * update staged on the HDD, hash it and verify it, reboot into it" check, so with no
 * update image present the expected outcome is that the boot walks straight past
 * every one of these. INFERRED, not measured.
 *
 * XcHMAC's 40 sites are much further out: they are XAPI content signing and the
 * XONLINE stack, which need the front end and the network layer.
 *
 * ===========================================================================
 * ARITY: FIVE HAND-COUNTED ROWS, BECAUSE THE MEASUREMENT CANNOT SEE ANY OF THEM.
 *
 * `generated/lifted/gen/kernel_arity.inc` has NO ROW for any ordinal between 329 and
 * 357: the table steps straight from `{328u, 1u, 1u, 1}` to `{358u, 1u, 1u, 1}`.
 * That is not a gap in the data, it is a property of the method.
 * `generated/retail/ordinal_callsites.json` records every one of these ordinals as
 * 100% `stub`-attributed, with `bracket: 0` and `register: 0`:
 *
 *     ordinal  name              sites  bracket  register  stub
 *     335      XcSHAInit            17        0         0    17
 *     336      XcSHAUpdate          24        0         0    24   <- undercount, see below
 *     337      XcSHAFinal           17        0         0    17
 *     340      XcHMAC               40        0         0    40
 *     346      XcDESKeyParity       13        0         0    13
 *
 * THE RECORDED 24 FOR ORDINAL 336 IS ITSELF TWO LOW, and it is worth fixing in print
 * because 111 total sites is the number this work was scheduled against. 336 has a
 * SECOND entry point: a 5-byte trampoline at 0x0037E656 whose body is `jmp 0x384870`,
 * into the real stub. `stub_functions()` in callsites.py accepts only a body that is
 * exactly one `jmp dword ptr [slot]`, so a trampoline into a stub is not resolved and
 * its three callers (0x0037F759, 0x0037F7B5, 0x00412F57) are credited to nobody,
 * while the trampoline's own jump is credited as one site. 23 direct + 3 via the
 * trampoline is 26. So the five ordinals here are 113 call sites, not 111. Arities
 * are unaffected.
 *
 * and `tools/lift/callsites.py` says in terms why that yields nothing: the lifter
 * renders a call to a one-instruction import jump stub as a DIRECT call and opens an
 * `_icall_esp` bracket only for indirect ones, so "a stub caller proves the ordinal
 * is a FUNCTION and is counted in the ranking, but contributes nothing to `counts`".
 * `stack_args_for()` in src/host/kernel_thunk.c therefore has nothing to accept OR
 * refuse for these five, and without a hand-written ABI_TABLE row each, the host
 * stops on `HOST_STOP_KERNEL_ABI_UNKNOWN` at the first crypto call even with a
 * perfect handler registered. This is the same situation as ordinal 24,
 * ExQueryNonVolatileSetting, and it is handled the same way.
 *
 * THE STUBS, resolved from the guest's own import table rather than assumed:
 *
 *     ordinal  thunk slot   stub
 *     336      0x0047582C   0x00384870   and a SECOND stub at 0x0037E656
 *     335      0x00475830   0x00384876
 *     337      0x00475834   0x0038487C
 *     340      0x00475870   0x00384882
 *     346      0x0047597C   0x003D1898
 *
 * The second XcSHAUpdate stub is worth recording: an ordinal with two stubs is an
 * ordinal whose sites split across two direct-call targets, which is another way for
 * a slot-keyed measurement to under-report.
 *
 * WHERE THE COUNTS COME FROM INSTEAD: TWO METHODS, ONE NOISY AND ONE CLEAN, AGREEING.
 *
 * METHOD A, hand-counted push runs in the raw instruction stream
 * (`generated/lifted/disasm/asm/`), resolving each stub and the 336 trampoline and
 * counting the pushes before every call. This is the same measurement callsites.py
 * would make if it could see these sites, and it is NOISY in one known direction: a
 * backward walk crosses an unbroken prologue or a previous argument run and
 * OVER-counts. The votes:
 *
 *     ordinal  sites  push vote                          mode
 *     335         17  {1: 15, 3: 1, 4: 1}                  1
 *     336         26  {3: 24, 4: 2}                        3
 *     337         17  {2: 14, 3: 1, 4: 1, 9: 1}            2
 *     340         40  {6: 1, 7: 30, 8: 3, 10: 2, 12: 4}    7
 *     346         13  {2: 12, 3: 1}                        2
 *
 * METHOD B, the decompiler's own argument reconstruction at every site in
 * `generated/retail/src/`, which is evidence of a DIFFERENT KIND rather than a second
 * opinion of the same kind: it comes from dataflow around the call, not from a
 * textual push window, so it does not share method A's failure mode. It is UNANIMOUS
 * everywhere: 335 {1: 15}, 336 {3: 23}, 337 {2: 16}, 340 {7: 40}, 346 {2: 13}. Its
 * shortfalls against the site counts (15 of 17, 23 of 26, 16 of 17) are the stub and
 * trampoline bodies themselves, which are not call expressions.
 *
 * THE TWO METHODS AGREE ON ALL FIVE, and the mode of a noisy-upward estimator
 * coinciding with a clean estimator's unanimous value is stronger than either alone.
 *
 * AND THE MINIMUM RULE WOULD HAVE BEEN WRONG ABOUT XcHMAC -- the single most useful
 * thing in this section. callsites.py takes the MINIMUM over sites, and the minimum
 * for 340 is SIX. The outlier is 0x004149FC in sub_00414994, and the cause is exactly
 * the hoisting failure kernel_thunk.c warns about: the compiler lifted six shared
 * argument pushes ABOVE a conditional branch, leaving only `push 0x7f2ba4` in the
 * block that contains the call.
 *
 *     0x004149AB  push eax          ; arg7, the out pointer
 *     0x004149AC  push edi          ; arg6 = 0
 *     0x004149AD  push edi          ; arg5 = NULL
 *     0x004149AE  push 0x64         ; arg4 = 100
 *     0x004149B0  push ebx          ; arg3
 *     0x004149B3  push esi          ; arg2 = 0x10
 *     0x004149B5  je  0x4149F7      ; <-- the branch the six pushes were hoisted over
 *     0x004149B7  push [0x475838]   ; arg1 = XboxHDKey
 *     0x004149BD  call 0x384882     ; 7 pushes visible here
 *     ...
 *     0x004149F7  push 0x7f2ba4     ; arg1 on the other arm
 *     0x004149FC  call 0x384882     ; 1 push visible here -> votes 6
 *
 * Both arms are seven-argument calls. So had a measured row for 340 existed it would
 * have had to be DISTRUSTED rather than used, and the hand row below is not merely a
 * substitute for a missing measurement.
 *
 * CALLING CONVENTION: __stdcall, MEASURED, NOT ASSUMED FROM THE `Xc` PREFIX. ZERO of
 * the 144 crypto call sites is followed by `add esp, N`. The only post-call stack
 * instructions anywhere are `pop esi`/`pop edi` restores paired with prologue pushes.
 * A caller that does not clean up and a one-instruction `jmp` stub that cannot means
 * the kernel routine pops its own arguments. Corroborated by the thinnest wrapper in
 * the image: `_XcspComputeSectionDigest@8` at 0x003851A1 ends `ret 8`.
 *
 * ARGUMENT ORDER IS PINNED BY LITERALS, not by the count and not by the name:
 *
 *   - 336 at 0x00380684 is `XcSHAUpdate(local_170, &local_74, 0x44)`, immediately
 *     after a `ReadFile` of exactly 0x44 bytes into `local_74`. A length cannot be
 *     the buffer and a stack address cannot be the length.
 *   - 337 at 0x00439897 is `XcSHAFinal(local_8c, local_18)` where `local_8c` is the
 *     same local the matching XcSHAInit received and `local_18` is declared
 *     `undefined4 local_18[5]`: five dwords is 20 bytes, the digest, not a context.
 *   - 340 at 0x00422713 is `XcHMAC(XboxHDKey, 0x10, buffer, 500, 0, 0, local_18)`,
 *     and the stored MAC this is compared against sits at `buffer + 500`. The 500
 *     is therefore the length of segment 1, which fixes arguments 2 and 3.
 *   - 346 at twelve of its thirteen sites is `XcDESKeyParity(<local>, 0x18)`. A
 *     stack address is not 0x18 twelve times over.
 *
 * ===========================================================================
 * THE SHA CONTEXT IS 116 BYTES, MEASURED THREE INDEPENDENT WAYS.
 *
 * This matters because XcSHAInit's single argument is a caller-supplied buffer and
 * writing more than the caller reserved corrupts whatever follows it.
 *
 *   1. A STACK FRAME WITH A KNOWN NEXT VARIABLE. At 0x00439897 the frame declares
 *      `undefined1 local_8c [116]` for the context and `undefined4 local_18 [5]`
 *      for the digest. 0x8C - 0x18 is 0x74, which is 116, so the 116 is bounded by
 *      the next live slot and not merely by the decompiler's guess.
 *   2. A HEAP STRUCTURE HOLDING THREE OF THEM. 0x0042C95A, 0x0042CCCA, 0x0042DBC7
 *      and 0x0042DEA7 address contexts at `param_1 + 0x1430`, `param_1 + 0x14A4`
 *      and `param_1 + 0x1518`. The strides are 0x74 and 0x74: 116 and 116. A
 *      structure layout is the strongest form of this evidence, because the
 *      compiler had to reserve the space at compile time.
 *   3. A SECOND, UNRELATED STACK FRAME. 0x00380684 declares
 *      `undefined1 local_170 [116]`.
 *   4. A FUNCTION WHOSE ONLY LOCAL IS THE CONTEXT, which is the cleanest of the
 *      four because there is nothing else in the frame to confuse it with.
 *      `_XcspComputeSectionDigest@8` at 0x003851A1 opens `sub esp, 0x74` and then
 *      passes `lea eax,[ebp-0x74]` to all three of Init, Update and Final. The whole
 *      frame IS the context: 0x74, 116.
 *   5. A HEAP ALLOCATION. 0x0037E83E calls `XMemAlloc(0x7C, ...)` -- 124 bytes -- and
 *      places the context at +8. 124 - 8 is 116.
 *
 * SO THE 116 IS NOT A DECOMPILER GUESS. It is bounded by the next live stack slot, by
 * a structure stride, by a frame that contains nothing else, and by a heap size. For
 * contrast, the title's OWN software hash context at 0x0044442C is 92 bytes, which is
 * the textbook size (5 state + 2 count words + 64-byte block). The extra 24 bytes are
 * specific to the XDK's declared Xbox context and their purpose is NOT determinable
 * from this image, because no site ever reads inside one.
 *
 * THE DIGEST IS 20 BYTES AND THE BLOCK IS 64, PROVED ARITHMETICALLY BY THE GUEST.
 * `_XShaHmacComputeFinal@16` at 0x0037E65B lays a 64-byte opad immediately above a
 * 20-byte slot, writes the inner digest into that slot, and then hashes BOTH IN ONE
 * UPDATE:
 *
 *     XcSHAFinal(param_1, local_18);          // inner digest
 *     XcSHAInit(param_1);
 *     XcSHAUpdate(param_1, local_58, 0x54);   // 0x54 == 84 == 64 opad + 20 digest
 *     XcSHAFinal(param_1, param_4);
 *
 * 0x54 only adds up if XcSHAFinal writes exactly 20 bytes into a slot sitting exactly
 * 64 bytes above the opad's start. If the digest were 16 or 32 the literal would be
 * 0x50 or 0x60. The matching ipad construction at 0x0037E5F4 XORs 0x36363636 over 64
 * bytes, which fixes the block size the same way.
 *
 * WE STILL DO NOT KNOW MICROSOFT'S LAYOUT, AND DO NOT GUESS IT. 116 is what the
 * guest RESERVES, which bounds us; it is not a field list. So `kernel_crypto.c`
 * defines its OWN 100-byte serialised layout inside those 116 bytes, all 32-bit
 * fields written through the guest accessors so the form does not depend on the
 * host's word size, with a `_Static_assert` tying 100 <= 116.
 *
 * WHY IN GUEST MEMORY RATHER THAN A HOST-SIDE TABLE, which is what kernel_hal.c does
 * for shutdown registrations. Because the guest embeds contexts inside heap objects,
 * THREE TO A STRUCTURE (evidence 2 above), and allocates those per object: a
 * fixed-capacity host table would overflow silently on a title that signs several
 * things at once, and the overflow would look like a hash failure. Storing in the
 * guest's own reservation is bounded in exactly the way the guest's own allocation
 * is.
 *
 * THE COOKIE IS NOT DECORATION. The first word is 'TSHA'. A context the guest
 * zeroed, copied from elsewhere, or never passed to XcSHAInit fails that check, and
 * XcSHAUpdate/XcSHAFinal then REFUSE and say so rather than hashing from an unknown
 * state. Starting a fresh hash instead would be the tempting repair and it is the
 * worst available option: the guest would receive a digest over a SUFFIX of its
 * message, compare it against a stored one, and read the mismatch as a corrupt save
 * rather than as our bug. XcSHAFinal also clears the cookie, so finalising twice is
 * reported instead of quietly returning a different digest.
 *
 * ===========================================================================
 * WHAT THE GUEST DOES WITH THE DIGESTS -- AND IT DOES CHECK THEM.
 *
 * This was worth establishing rather than assuming, because if nothing ever compared
 * a digest then correctness would be decorative. It is not. There are TWO distinct
 * verification sites, both of which refuse the operation on a mismatch, and the
 * comparisons are INLINE dword loops rather than memcmp calls, which is why a search
 * for memcmp finds nothing and would mislead.
 *
 *   1. SHA-1, FULL 20 BYTES, at 0x00380684 `_XapipUpdateDetectAndVerify`. The
 *      content-metadata header is hashed and compared against a stored digest five
 *      dwords at a time:
 *
 *          XcSHAInit(local_170);
 *          XcSHAUpdate(local_170, &local_74, 0x44);
 *          XcSHAFinal(local_170, local_30);
 *          iVar1 = 5; piVar4 = local_30; piVar5 = local_c0;
 *          do { ... bVar6 = *piVar4 == *piVar5; ... } while (bVar6);
 *
 *      and a mismatch falls through to `uVar2 = 0x8007000d`, an HRESULT the caller
 *      treats as a refusal. A wrong SHA-1 means every staged title update is
 *      rejected.
 *
 *   2. HMAC-SHA1, TRUNCATED TO 12 BYTES, at 0x00422713. Each 512-byte block has its
 *      first 500 bytes MACed with the Xbox HD key and compared against the 12 bytes
 *      at offset 500 -- THREE dwords, not five:
 *
 *          XcHMAC(XboxHDKey_exref, 0x10, param_1, 500, 0, 0, local_18);
 *          iVar2 = 3; piVar3 = local_18; piVar4 = param_1 + 0x7d;
 *          do { ... bVar5 = *piVar3 == *piVar4; ... } while (bVar5);
 *          if (!bVar5) { *puVar1 = 0; return 0x80004005; }
 *
 *      0x7D * 4 is 500, so the stored MAC directly follows the MACed region. The
 *      truncation to 12 bytes is the guest's choice and costs us nothing: we must
 *      still produce the correct 20, because the first 12 of a wrong 20 are wrong.
 *
 * THE QUANTITATIVE PICTURE, because "two sites" would understate it badly. NO NULL
 * MODEL APPLIES to any proportion below: these are a census of this one image, not a
 * sample of anything, so the figures are counts and not estimates.
 *
 * All 17 XcSHAFinal sites, by what consumes the digest:
 *      1  COMPARED, 20 bytes, at 0x0038076A (the update verifier above)
 *      1  fed to XcVerifyPKCS1Signature, at 0x004398F3 -- a digest that is CHECKED,
 *         just not by a byte compare: sub_004366B0 hands it to ordinal 344 with
 *         XePublicKeyData and branches on the returned AL
 *      2  fed into further hashing, the HMAC inner/outer construction
 *      1  consumed as an INTEGER: sub_0042AF91 computes
 *         (digest[0] & 0x7FFF) to derive a LAN port number
 *     12  stored or written out -- save manifests, signature structures, network
 *         records, several of them straight into a WriteFile
 *
 * XcHMAC is where verification really lives, and a per-site count misses it because
 * the verification is FACTORED OUT. Five of the 40 sites compare in-function, with
 * widths of 8, 20, 12, 12 and a caller-supplied length. But one of those five,
 * sub_00439958 at 0x00439976, is a GENERIC HMAC-VERIFY PRIMITIVE -- it takes the
 * same seven arguments plus an expected MAC and a length, computes, compares and
 * returns a bool -- and it has TEN external callers across the XNet secure-transport
 * layer. Its emit-side sibling sub_00439918 has eleven more. So every inbound secure
 * packet's MAC is checked, through one function that the site census counts once.
 *
 * THE UPPER-BOUND CAVEAT IS KEPT. "The function contains a comparison idiom" does not
 * prove the compared bytes are the MAC. 0x00422713 and 0x00439958 were read closely
 * enough to confirm that they are; the other three were not.
 *
 * WHAT THE OTHER OUTPUTS ARE FOR is not verification but KEY DERIVATION, which is a
 * stricter requirement, not a looser one. At 0x0041524E two HMACs write into
 * overlapping stack slots four bytes apart, the resulting 24 bytes go to
 * XcDESKeyParity, and that goes straight to XcKeyTable and XcBlockCryptCBC. Nothing
 * compares the MAC because the MAC IS the key: a wrong HMAC yields a wrong 3DES key
 * and silently wrong ciphertext, with no diagnostic anywhere. That is the strongest
 * reason in this file for testing against published vectors.
 *
 * ===========================================================================
 * XcHMAC IS NOT TEXTBOOK HMAC, AND ONE DEVIATION REMAINS UNRESOLVED.
 *
 * Two things set the Xbox export apart from RFC 2104, and they have very different
 * evidential status.
 *
 * MEASURED: IT TAKES TWO MESSAGE SEGMENTS. Seven arguments, not five, at all 40
 * sites -- (key, key length, data1, length1, data2, length2, mac out). 33 sites pass
 * a literal 0 for both segment-2 arguments, so reading only those would make it look
 * like the five-argument textbook form; 7 sites pass a real pointer and a real
 * length, five of them the literal 0x10. An HMAC over a concatenation is an HMAC
 * over the segments fed in order, so this is a signature difference rather than an
 * algorithmic one, but getting the signature wrong desyncs the guest's esp.
 *
 * NOT SETTLED BY THE CALL SITES, BUT CORROBORATED TWICE: WHAT A KEY LONGER THAN 64
 * BYTES DOES. RFC 2104 hashes an over-long key down to 20 bytes first. No call site
 * reaches the question: 33 sites pass the literal 0x10, one passes 0xc, and six pass
 * a runtime value, so every key we can see is 16 bytes or less against a 64-byte
 * block.
 *
 * The evidence that it TRUNCATES instead comes from somewhere better than
 * recollection, and from inside the user's own binary. THE TITLE SHIPS TWO OF ITS
 * OWN SOFTWARE HMAC IMPLEMENTATIONS, at 0x0037E5F4 and 0x004258BA, both written
 * against this same seven-argument interface, and BOTH CLAMP RATHER THAN HASH:
 *
 *     if (0x40 < param_2) { param_2 = 0x40; }
 *
 * Two independent in-image models of the API agreeing is real corroboration, and it
 * is the reason `kernel_crypto_xc_hmac` truncates. It is still not proof about the
 * KERNEL's routine: these are the title's reimplementations, not Microsoft's, and
 * 0x004258BA is a model of the INTERFACE ONLY -- its underlying hash is MD5, not
 * SHA-1 (four initial words, no 0xC3D2E1F0, and it tags its context "MD 5"), with a
 * 16-byte digest and a 0x50-byte outer update. Borrowing its algorithm would be a
 * serious error; borrowing its argument semantics is sound.
 *
 * So the status is: MEASURED that no site reaches the case, CORROBORATED twice that
 * truncation is the house behaviour, and UNVERIFIED against hardware.
 *
 * So the module does three things rather than picking a winner. It implements the
 * RFC 2104 reduction in `kernel_crypto_hmac_sha1`, which is what the RFC 2202
 * long-key vectors (cases 6 and 7) pin. It implements truncation in
 * `kernel_crypto_xc_hmac`, which is what the ordinal 340 handler calls. And when a
 * guest key does exceed the block it increments
 * `kernel_crypto_hmac_oversize_key_count()` and SAYS SO in the log, because the one
 * thing that must not happen is this choice being made silently. For every key at
 * or below the block size the two are bit-identical, which is why RFC 2202 cases 1
 * through 5 are genuine published-vector coverage of the Xbox entry point and not
 * merely of the RFC one.
 *
 * ===========================================================================
 * WHAT IS THEREFORE NOT MODELLED, said plainly:
 *
 *   - THE REAL CONTEXT LAYOUT. 116 bytes is a bound, not a field list. If a later
 *     task finds the guest reading a field INSIDE a context -- no site does today;
 *     every one of the 15 XcSHAInit sites and all 23 XcSHAUpdate sites treat it as
 *     an opaque pointer -- that is the point at which the layout has to be derived
 *     from those reads, and the cookie check will fail loudly first.
 *   - THE OVER-LONG KEY REDUCTION, above.
 *   - THE SIX REMAINING Xc* ORDINALS, which are the same cluster by subject and are
 *     NOT implemented here. Arities measured the same two ways, for whoever takes
 *     them: 338 XcRC4Key 3 args over 4 sites and 339 XcRC4Crypt 3 over 7, both
 *     unanimous by BOTH methods (implemented later by T1114 after xemu state
 *     measurements; this paragraph records T452's historical baseline);
 *     349 XcBlockCryptCBC 7 over 8, likewise unanimous;
 *     347 XcKeyTable 3 over 8, push vote {3: 7, 5: 1}.
 *
 *     TWO ARE NOT SAFE TO GIVE A ROW TO YET, and saying which is more useful than
 *     listing the four that are. 345 XcModExp has three sites and THE TWO METHODS
 *     DISAGREE: the push vote is {5: 2, 6: 1} and the decompiler reconstructs
 *     {4: 1, 5: 2}. They agree only on the mode, 5, and a mode of three is not
 *     corroboration. 344 XcVerifyPKCS1Signature has exactly ONE site, 0x00436712,
 *     so "unanimous" over it is the tautology `stack_args_for()` exists to refuse;
 *     its padding scheme is also invisible from the caller. Both need evidence from
 *     outside the call sites.
 *
 *     Non-obvious semantics worth not rediscovering: XcKeyTable's and
 *     XcBlockCryptCBC's first argument is literally `keylen != 8`, emitted as
 *     `cmp dword ptr [ebp+0x10], 8 / setne al` at 0x004345A7 -- MEASURED to be a
 *     boolean, INFERRED to mean 3DES versus single DES. XcBlockCryptCBC's sixth
 *     argument is MEASURED to be the direction flag, because 0x0041524E passes 1 and
 *     0x0041539D passes 0 over the same buffers. The key table is 384 bytes, the IV
 *     8, the big-integer width 96 bytes (0x18 dwords, 768-bit Diffie-Hellman), and
 *     the RC4 context 260 bytes.
 *   - RETURN VALUES. The kernel boundary here is 32-bit (see kernel_call.h), which
 *     is sufficient: these five return void or a BOOLEAN on hardware and no measured
 *     site tests the result of any of them. That is NOT true of the rest of the
 *     cluster -- ordinal 344's AL is branched on at 0x00436717 and 345's EAX is
 *     tested at one of its three sites -- so whoever takes those two inherits a
 *     question this module does not have.
 *
 * TWO THINGS THE NEXT TASK IN THIS CLUSTER WILL NEED, recorded here because they were
 * found while measuring these five and would otherwise have to be found again:
 *
 *   - FIVE DATA EXPORTS ARE LOAD-BEARING FOR THE KEYS. 323 XboxHDKey (slot 0x475838,
 *     a 16-byte array, and the key at the majority of XcHMAC sites), 325
 *     XboxSignatureKey, 353 XboxLANKey, 354 XboxAlternateSignatureKeys and 355
 *     XePublicKeyData. These are VARIABLES, not functions: their thunk slots must
 *     hold the address of a readable object rather than a dispatch stub, or the guest
 *     faults. All five are already in `MEASURED_DATA_ORDINALS`. We hold none of their
 *     values, so a run that reaches a real verification will compare a MAC derived
 *     from whatever those slots point at.
 *   - XcRC4Crypt IS CALLED AT DISPATCH_LEVEL. 0x00439CA3 wraps it in
 *     KeRaiseIrqlToDpcLevel / KfLowerIrql, so that handler must not allocate or
 *     block. Not our ordinal, but it constrains whoever implements 339.
 */

#ifndef TSFP_XBOX_KERNEL_CRYPTO_H
#define TSFP_XBOX_KERNEL_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_hle.h"

/* T1114 xemu reference: S[256], i and j bytes, then untouched two-byte
 * caller padding within the original 260-byte allocation. No host cookie or
 * singleton: the entire cipher state remains guest-resident. Key's effective
 * length is its low byte, with zero meaning 256 bytes, measured at 0/256/257/
 * 512/513. KSA resets i/j before reading aliased key bytes; PRGA commits cached
 * i/j after XOR. Arbitrary/unkeyed states are accepted by the kernel.
 * See docs/t1114-xnet-rc4.md for reference/version and pointer-fault limits. */
#define KERNEL_RC4_STATE_BYTES 258u
#define KERNEL_RC4_CALLER_ALLOCATION_BYTES 260u
size_t kernel_crypto_rc4_key_span(uint32_t key_length);
bool kernel_crypto_rc4_key(uint8_t state[KERNEL_RC4_STATE_BYTES],
                            const uint8_t *key, uint32_t key_length);
bool kernel_crypto_rc4_crypt(uint8_t state[KERNEL_RC4_STATE_BYTES],
                              uint8_t *data, uint32_t length);
/* Checked guest boundary, also usable by an original-backed XNET helper.
 * Invalid/unreadable/wrapped spans refuse before mutation. These are host HLE
 * errors, not an implementation of the original kernel's exception dispatch.
 * Readability is not writability; protection/unmap faults follow the existing
 * armed host guest-memory fault boundary. No allocation or cipher-state locks. */
uint32_t kernel_crypto_rc4_key_guest(uint32_t state, uint32_t key_length, uint32_t key);
uint32_t kernel_crypto_rc4_crypt_guest(uint32_t state, uint32_t length, uint32_t data);

/**
 * SHA-1 working state, HOST-side and OURS.
 *
 * This is deliberately NOT a model of the Xbox context. It is the form the core
 * works in; the guest-visible form is the 100-byte serialisation in
 * kernel_crypto.c, which is what has to stay inside the guest's 116 bytes.
 */
typedef struct {
    uint32_t chain[5];
    /** Message length in BITS, as FIPS 180-1 counts it, and 64 bits wide so a long
     *  message cannot silently wrap the way a 32-bit byte count would. */
    uint64_t bits;
    uint8_t block[64];
    size_t pending;
} kernel_sha1_state;

/** FIPS 180-1 SHA-1, streaming. `absorb` may be called any number of times. */
void kernel_crypto_sha1_reset(kernel_sha1_state *state);
void kernel_crypto_sha1_absorb(kernel_sha1_state *state, const void *data, size_t length);

/**
 * Finish and write the 20-byte digest.
 *
 * CONSUMES the state: the padding is absorbed into it, so a second call returns a
 * different and meaningless answer. The ordinal 337 handler enforces this by
 * clearing the guest context's cookie, which is the only place it can be enforced.
 */
void kernel_crypto_sha1_squeeze(kernel_sha1_state *state, uint8_t digest[20]);

/** FIPS 180-1 SHA-1 in one call. */
void kernel_crypto_sha1(const void *data, size_t length, uint8_t digest[20]);

/**
 * RFC 2104 HMAC-SHA1 exactly as specified, including HASHING a key longer than the
 * 64-byte block.
 *
 * Exposed because this is the form RFC 2202's published vectors cover, cases 6 and 7
 * being the two with an 80-byte key. Without it the long-key path would have no
 * external oracle at all. The ordinal 340 handler does NOT call this -- see the
 * divergence note in this file.
 */
void kernel_crypto_hmac_sha1(const void *key, size_t key_length, const void *message,
                             size_t message_length, uint8_t mac[20]);

/**
 * The XcHMAC shape: TWO message segments, and a key longer than the block TRUNCATED
 * rather than hashed.
 *
 * Either segment may be NULL with length 0, which is the common case: 33 of the 40
 * measured sites pass 0 for both segment-2 arguments. For any key of 64 bytes or
 * fewer this is bit-identical to kernel_crypto_hmac_sha1 over the concatenation, and
 * no measured call site passes a longer one.
 */
void kernel_crypto_xc_hmac(const void *key, size_t key_length, const void *first,
                           size_t first_length, const void *second, size_t second_length,
                           uint8_t mac[20]);

/**
 * Force ODD parity on the low bit of every byte, per FIPS 46-3 / ANSI X3.92.
 *
 * Returns whether any byte actually changed, which is what lets a test assert that
 * an already-correct published key is left alone rather than merely that it still
 * looks plausible.
 */
bool kernel_crypto_des_key_parity(void *key, size_t length);

/** Register the crypto ordinals with the HLE dispatcher. Returns how many bound. */
unsigned kernel_crypto_register(void);

/** Zero every counter. For tests. */
void kernel_crypto_reset(void);

/* Existing SHA/HMAC/DES call counters. T1114 does not claim a measured live
 * retail RC4 call census; its original-wrapper integration is synthetic. */
unsigned kernel_crypto_sha_init_count(void);
unsigned kernel_crypto_sha_update_count(void);
unsigned kernel_crypto_sha_final_count(void);
unsigned kernel_crypto_hmac_count(void);
unsigned kernel_crypto_des_parity_count(void);

/**
 * How many times an update or a final was handed a context this host had not
 * initialised, or had already finalised.
 *
 * Should be 0 on any honest run. A nonzero value means either the guest is copying
 * or zeroing contexts behind our back -- in which case the 116-byte reservation is a
 * bound but not the whole story and the real layout has to be derived -- or our
 * argument order is wrong and the context pointer is not argument 0. Those two are
 * distinguishable: the second would make it nonzero at EVERY site.
 */
unsigned kernel_crypto_stale_context_count(void);

/**
 * How many times XcHMAC was given a key longer than the 64-byte block.
 *
 * Expected to be 0, because no measured site does it. If it is ever nonzero, the
 * truncate-versus-hash choice documented in this file became load-bearing on that
 * run and the digest should not be trusted until it is settled.
 */
unsigned kernel_crypto_hmac_oversize_key_count(void);

/** Total bytes fed through XcSHAUpdate, so a plausibility check has a number. */
unsigned long long kernel_crypto_sha_bytes_absorbed(void);

/**
 * Bytes of the guest's context buffer this host writes.
 *
 * Exposed so a test can assert the serialised form fits the guest's measured 116
 * without hard-coding our size in two places, which would let the two drift apart
 * and quietly stop testing the bound.
 */
size_t kernel_crypto_guest_context_bytes(void);

#endif /* TSFP_XBOX_KERNEL_CRYPTO_H */
