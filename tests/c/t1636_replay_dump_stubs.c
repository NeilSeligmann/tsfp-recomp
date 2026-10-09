/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1636: link stubs for t1636_replay_dump.c. xinput_script_install and the poll path reach these, the dump never calls them
 * (the build uses -ffunction-sections and --gc-sections, so nothing else is needed). */
void xinput_devices_run_locked(void) {}
void xinput_hle_set_synthetic_pad_state(void) {}
