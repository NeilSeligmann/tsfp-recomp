/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1616: glue between the route player (src/input/xinput_route.h), the recorded marks (xinput_record.h), the guest
 * memory read path and the guarded poke (guest_dump_poke_now). FABRICATED-STATE: it only ever pokes through the T1613
 * guards, and only when --forced-state is on (host_options enforces that).
 */
#ifndef TSFP_HOST_HOST_ROUTE_H
#define TSFP_HOST_HOST_ROUTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_route.h"

typedef struct {
    const char *const *waits; /* --route-wait specs */
    size_t wait_count;
    /* T1633: --route-wait-event specs, and the `# wait:` lines of the record (host_route_setup fills them from the loaded
     * record). A command line wait for a mark replaces the record's wait for it. */
    const char *const *event_waits;
    size_t event_wait_count;
    const char *const *record_waits;
    size_t record_wait_count;
    /* NULL: the host's observers (route_probe.c, enabled here). Tests pass their own. */
    const xinput_route_probe *probe;
    const char *const *pokes; /* --poke-at-poll specs */
    size_t poke_count;
    xinput_source_fn live; /* the live pad after the replay (NULL = rest) */
    void *live_user;
    /* Called once from the pad poll when a route wait times out; the host stops with `reason`. May be NULL. */
    void (*on_failure)(const char *reason);
    /* Test seams, NULL in the host: guest memory read and the poke action (returns whether it ran). */
    bool (*read_mem)(uint32_t address, unsigned width, uint32_t *value, void *user);
    bool (*poke)(const char *label);
    void *test_user;
    /* T1640 closed loop menu navigation (docs/t1640-nav-state-machine.md). record_navs: the `# nav:` lines of the loaded record
     * (host_route_setup fills them). nav_mode: --route-nav on|off|strict, NULL = on when a table is given, else off. nav_menus_file:
     * --route-nav-menus FILE, nav_menus_text: the table text itself (test seam, wins over the file). nav_timing: --route-nav-timing
     * HOLD:GAP. read_bytes: test seam for the guest byte reader (item names), NULL in the host. */
    const char *const *record_navs;
    size_t record_nav_count;
    const char *nav_mode;
    const char *nav_menus_file;
    const char *nav_menus_text;
    const char *nav_timing;
    bool (*read_bytes)(uint32_t address, void *buffer, size_t length, void *user);
} host_route_config;

/* Parse the specs, take the marks and the script of the loaded replay (xinput_replay_load) and install the route
 * player as the pad source. False with `error` otherwise (nothing installed). The poke triggers log one line each. */
bool host_route_setup(const host_route_config *config, char *error, size_t error_size);
/* Same, from an explicit script and marks (tests). */
bool host_route_setup_from(const host_route_config *config, const xinput_script *script, const uint64_t *marks,
                           size_t mark_count, char *error, size_t error_size);
/* The installed route (NULL before setup). Torn down by host_route_teardown. */
xinput_route *host_route_current(void);

/* T1640 recorder side: while a route is RECORDED (--record-input) sample the menu table and write `nav` lines to the route event log and
 * `# nav:` lines to the record (src/input/xinput_nav_record.h). The table comes from config->nav_menus_text or the file config->nav_menus_file; guest
 * memory through config->read_mem / read_bytes (test seams, NULL in the host). False with `error` when the table cannot be read or parsed or has no menu.
 * Call after xinput_record_open. host_route_nav_record_stop frees it (the record's close hook already flushed an activation still held). */
bool host_route_nav_record_start(const host_route_config *config, char *error, size_t error_size);
void host_route_nav_record_stop(void);
/* T1629: true once the recorded inputs are exhausted and the live pad has the controls (the XINPUT_ROUTE_END event
 * fired, sticky). Read on the pad poll thread by the button dump (--dump-button-after-replay). False without a route. */
bool host_route_handed_over(void);
void host_route_teardown(void);
#endif
