/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Name a host code address, or a guest VA, after the lifted function that holds it (T505).
 *
 * A fault inside lifted code used to end a run with a signal and an address and nothing to
 * map it to a function. The host executable is linked with its static symbol table, so the
 * lifted `sub_XXXXXXXX` functions are all in `.symtab`. This module reads that table from
 * the ELF file itself at REPORT time, never in a signal handler: it allocates, reads files
 * and sorts. Nothing needs `-rdynamic` or a link time address table, and a stripped binary
 * simply names nothing (the callers print the raw addresses).
 *
 * Two lookups, both on a symbol table built from one ELF file:
 *  - a HOST address (a PIE load address, as a signal context reports it) to
 *    `name+0xOFFSET`. Addresses outside the executable fall back to `dladdr`.
 *  - a GUEST VA to the lifted function that contains it, from the `sub_%08X` symbol names:
 *    the greatest function start at or below the VA, `sub_00383DF3+0x1C`.
 */

#ifndef TSFP_HOST_SYMBOLS_H
#define TSFP_HOST_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct host_symbol_table host_symbol_table;

/** Read the function symbols of `elf_path` (a 64 bit ELF). `load_bias` is added to every
 *  symbol value, which is the PIE load address (0 for a fixed executable). NULL on any
 *  failure or when the file has no symbol table. */
host_symbol_table *host_symbol_table_load(const char *elf_path, uintptr_t load_bias);
void host_symbol_table_free(host_symbol_table *table);

/** `name+0xOFFSET` for the function symbol holding `address`. False when none does. */
bool host_symbol_table_describe(const host_symbol_table *table, uintptr_t address, char *out,
                                size_t size);

/** `sub_XXXXXXXX+0xOFFSET` for the lifted function whose guest start is the greatest one at
 *  or below `guest_va`. False when no `sub_%08X` symbol is at or below it. */
bool host_symbol_table_guest_function(const host_symbol_table *table, uint32_t guest_va,
                                      char *out, size_t size);

/** The running executable's table, loaded once on first use (`/proc/self/exe`).
 *  Not async signal safe. */
bool host_symbols_describe(uintptr_t address, char *out, size_t size);
bool host_symbols_guest_function(uint32_t guest_va, char *out, size_t size);

#endif /* TSFP_HOST_SYMBOLS_H */
