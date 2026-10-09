/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1510 x87 roots (000CB3B0, 001B7D00, 001B7D70, 00259DF0). The registrations live in
 * x87_roots.inc so tools/replace can hash and mutate them as one unit; they use the x87
 * replacement ABI of x87_replace.h.
 */
#include "x87_replace.h"

#include "x87_roots.inc"
