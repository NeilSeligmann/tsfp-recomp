/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The single list of kernel HLE modules.
 *
 * WHY THIS EXISTS. Two programs register modules: `tsfp_host`, which runs the guest, and
 * `hle_report`, which reports the backlog. Each used to carry its own copy of the list, and
 * `hle_report`'s copy was EMPTY for months, so it printed "151 of 151 imported ordinals
 * still need implementations" no matter how much was built, and a test pinned that figure.
 * A module added to one list and forgotten in the other makes the backlog tool overstate
 * the work again, silently. One list means that cannot happen.
 *
 * Deliberately NOT included here: anything that needs the host or the loader. Modules that
 * need extra wiring after registration (the section image base, the firmware sink) are
 * wired by the host, which is the only caller that has those inputs.
 */

#ifndef TSFP_XBOX_KERNEL_REGISTER_ALL_H
#define TSFP_XBOX_KERNEL_REGISTER_ALL_H

#include <stddef.h>

/** Register every kernel HLE module. Returns how many ordinals were bound in total. */
size_t kernel_register_all(void);

#endif /* TSFP_XBOX_KERNEL_REGISTER_ALL_H */
