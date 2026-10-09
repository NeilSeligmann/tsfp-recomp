/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal <stdint.h> for Ghidra's C parser.
 *
 * WHY THIS FILE EXISTS. `tools/gen_donor_header.py` emits a header whose single
 * include is <stdint.h>, which is correct for a self-contained header but leaves
 * Ghidra's PreProcessor with nothing to resolve: Ghidra ships no libc headers, and
 * an unresolved include means every `uint32_t` becomes an undefined type and the
 * whole parse aborts on the first field. Ghidra's built-in type manager does not
 * supply the C99 fixed-width names either.
 *
 * So this is the include path handed to ParseCHeader.java, and nothing more. It is
 * deliberately NOT a general libc shim: adding declarations nothing asks for would
 * put types into the program's database that no evidence supports.
 *
 * WIDTHS ARE THE TARGET'S, NOT THE DONOR'S. These are the x86 Win32 widths of the
 * TSFP Xbox build, which is the program being parsed into. That is the right
 * choice even though the struct layouts come from a MIPS EE donor: Ghidra sizes
 * the aggregate with the data organization of the program it belongs to, and
 * pretending `long` were 8 bytes here would corrupt every offset in the database
 * for no gain. Donor offsets are already untrustworthy and recorded as comments
 * only; see the generated header's own warning.
 */

#ifndef TSFP_CPARSER_STDINT_H
#define TSFP_CPARSER_STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

#endif /* TSFP_CPARSER_STDINT_H */
