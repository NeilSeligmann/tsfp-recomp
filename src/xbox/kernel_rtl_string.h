/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Counted-string character-set conversion: ordinals 260 and 308.
 *
 *     NTSTATUS __stdcall RtlAnsiStringToUnicodeString(PUNICODE_STRING DestinationString,
 *                                                     PANSI_STRING SourceString,
 *                                                     BOOLEAN AllocateDestinationString);   // 260
 *     NTSTATUS __stdcall RtlUnicodeStringToAnsiString(PANSI_STRING DestinationString,
 *                                                     PUNICODE_STRING SourceString,
 *                                                     BOOLEAN AllocateDestinationString);   // 308
 *
 * ARITY, ORDER AND MODE, MEASURED. THREE stdcall arguments each (nxdk @12 both). One site
 * each, and each is complete: the thunk slots 0x004759D4 (260) and 0x004759D8 (308) are
 * referenced once, as `call dword ptr`, with no register or import-stub use. Both sites are
 * the kernel call inside the title's statically linked XAPI conversion wrappers:
 *
 *     260  0x003D15A2  inside MultiByteToWideChar (ret 0x18, six arguments)
 *     308  0x003D166A  inside WideCharToMultiByte (ret 0x20, eight arguments)
 *
 * At each, three pushes precede the call and the order is `push 0; push &SourceDescriptor;
 * push &DestinationDescriptor`, so the LAST argument is the literal 0, the middle one the
 * source and the first the destination. Both descriptors are 8-byte structures the wrapper
 * builds in its own frame (guest_object_string: Length, MaximumLength, Buffer):
 *
 *     260  source ANSI    [ebp-8]   Length = cbMultiByte, MaximumLength = cbMultiByte+1,
 *                                    Buffer = the caller's bytes
 *          dest UNICODE   [ebp-0x10] Length = 2*cbMultiByte (a placeholder, the callee
 *                                    overwrites it), MaximumLength = 2*cchWideChar,
 *                                    Buffer = the caller's wide buffer
 *     308  source UNICODE [ebp-8]   Length = 2*cchWideChar, MaximumLength = 2*(cchWideChar+1)
 *          dest ANSI      [ebp-0x10] Length = 0, MaximumLength = cbMultiByte,
 *                                    Buffer = the caller's byte buffer
 *
 * The wrapper has checked before the call that the caller's buffer holds the converted count
 * PLUS ONE (`cmp cchWideChar, count+1; jl -> ERROR_INSUFFICIENT_BUFFER 0x7A`) and returns
 * count+1 on success, so the NUL terminator IS part of what the wrapper reports, and a
 * terminator write is required. The wrapper tests only the SIGN of the status (`jl`), and
 * on a negative status calls RtlNtStatusToDosError. 308's wrapper also insists on a NULL
 * lpDefaultChar (`cmp [ebp+0x20], 0`, else ERROR_INVALID_PARAMETER 0x57) and zeroes
 * lpUsedDefaultChar, so no default character is ever supplied.
 *
 * THE ONLY MEASURED MODE is AllocateDestinationString = 0 (a caller-supplied buffer). A
 * nonzero low byte is REFUSED loudly with STATUS_NOT_IMPLEMENTED and no change: allocating
 * needs the kernel's string allocator, which no site exercises.
 *
 * INFERRED, from the NT contract (not measured, and stated as such in every report):
 *   - needed bytes: 260 writes 2*Length + 2 (the characters and a WCHAR NUL), 308 writes
 *     Length/2 + 1 (the characters and a NUL). A needed size above 0xFFFF is REFUSED with
 *     STATUS_INVALID_PARAMETER (a counted string cannot describe it).
 *   - Destination.MaximumLength smaller than needed: STATUS_BUFFER_OVERFLOW, nothing written
 *     and Destination.Length left alone. The measured site can never take this arm.
 *   - On success Destination.Length is the converted byte count WITHOUT the terminator, and
 *     Destination.MaximumLength and Buffer are not touched.
 *   - THE MAPPING. 260 zero-extends every byte to a WCHAR (Latin-1), 308 narrows a WCHAR to
 *     its low byte when it is at most 0xFF and writes '?' (0x3F) otherwise. This is the
 *     code-page-free reading: this kernel carries no NLS tables the title could select, and
 *     for ASCII (all this title's strings so far) every reading agrees. A byte at or above
 *     0x80 in 260 and a character above 0xFF in 308 are therefore REPORTED, never silent.
 *   - An odd Unicode Length in 308 is REFUSED (STATUS_INVALID_PARAMETER): NT drops the last
 *     byte, nothing here measured an odd length, and guessing a truncation is worse.
 *   - An unreadable source descriptor or text, or an unwritable destination descriptor or
 *     buffer, is STATUS_ACCESS_VIOLATION (the real kernel would fault), with nothing
 *     written: the whole output is composed on the host and written once, so a refusal
 *     never leaves a half-converted string. Source and destination text may overlap.
 */
#ifndef TSFP_XBOX_KERNEL_RTL_STRING_H
#define TSFP_XBOX_KERNEL_RTL_STRING_H

#include <stddef.h>
#include <stdint.h>

#define ORD_RtlAnsiStringToUnicodeString 260u
#define ORD_RtlUnicodeStringToAnsiString 308u

/** Bind 260 and 308. Returns how many ordinals bound. */
size_t kernel_rtl_string_register(void);

/** Bytes at/above 0x80 converted by 260 plus characters above 0xFF replaced by 308, since
 * the last register. Non-zero means the title left ASCII and the Latin-1 reading matters. */
uint32_t kernel_rtl_string_nonascii_count(void);

#endif /* TSFP_XBOX_KERNEL_RTL_STRING_H */
