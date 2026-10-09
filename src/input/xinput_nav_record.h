/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_INPUT_XINPUT_NAV_RECORD_H
#define TSFP_INPUT_XINPUT_NAV_RECORD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xinput_route_nav.h"

/* T1640 recorder side (docs/t1640-nav-state-machine.md "Recording"): while a route is RECORDED the menu table is sampled at every port 0 pad
 * poll and
 *  - every change of (menu, cursor, count, item id, ready) is a line of the route event log, `nav menu=ID cursor=C count=K id=I ready=R`, or
 *    `nav menu=none` when no menu of the table is on screen (changes only);
 *  - every ACTIVATION becomes a `# nav: at=P to=Q menu=ID select=id:I|index:C activate` comment of the record. An activation is the rising edge of
 *    the menu's select button (default A) at a poll where a menu of the table is active AND ready and the cursor is on a row the cursor does not
 *    skip and that is not greyed. at = the poll of the first d-pad press edge made in this page instance (the page, since it was found), else the
 *    poll of the select edge. to = the first poll after the edge with the select button up (the record's end when it is still down at the close).
 *    The select edge of a press while the menu is not ready, on a greyed or skipped row, or with the cursor on no row is NOT an activation
 *    (`nav-skip` line). A back press (B) is never emitted: the recorded presses stay open loop.
 * The module is a function of its probe and of the polls it is given; the host wires it to the record (xinput_record_set_nav_hooks). */
typedef struct {
    route_nav_probe probe; /* read_mem, read_bytes, now_ms (unused), user */
    void (*log)(const char *line, void *user);
    void *log_user;
    /* Adds a nav line to the record (xinput_record_add_nav); NULL result = added, else the reason it was refused. */
    const char *(*add)(uint64_t at, uint64_t to, uint64_t edge_poll, const char *body, int select_analog, uint16_t select_digital, void *user);
    void *add_user;
} nav_record_hooks;

typedef struct nav_recorder nav_recorder;

/* The table is copied. NULL when the table has no menu. */
nav_recorder *nav_recorder_create(const route_nav_menu_table *table, const nav_record_hooks *hooks);
void nav_recorder_free(nav_recorder *recorder);
/* One port 0 poll with the pad state that was recorded for it. */
void nav_recorder_observe(nav_recorder *recorder, uint64_t poll_index, const xinput_pad_state *recorded);
/* The record is closing after `total_polls` polls: an activation whose select button is still down ends there. */
void nav_recorder_close(nav_recorder *recorder, uint64_t total_polls);
/* Counters for the tests and the log: polls skipped by the mode gate, polls scanned, activations emitted, activations skipped. */
void nav_recorder_stats(const nav_recorder *recorder, uint64_t *gated, uint64_t *scanned, unsigned *emitted, unsigned *skipped);

#endif
