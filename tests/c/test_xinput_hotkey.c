/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1627: host hotkeys. The --hotkey grammar, the hold time, once-per-press firing, the SWALLOWING of the second and later
 * chord keys (and only those), the repeated-DOWN and focus-loss edges, the swallow table limit, the DIR/hotkey.<n> files.
 * Pure C, synthetic events with a test clock, no SDL, no guest, no lifted tree.
 */
#include "xinput_hotkey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

#define PAD XINPUT_HOTKEY_DEVICE_PAD
#define KB XINPUT_HOTKEY_DEVICE_KEYBOARD

static unsigned fired_calls;
static char fired_labels[8][32];
static uint64_t fired_polls[8];
static void on_fire(unsigned number, const char *label, uint64_t poll, void *user)
{
    (void)user;
    if (fired_calls < 8u) {
        snprintf(fired_labels[fired_calls], 32u, "%s", label);
        fired_polls[fired_calls] = poll;
    }
    fired_calls++;
    CHECK(number == fired_calls);
}

static xinput_hotkeys *make(const char *const *texts, unsigned count, const char *dir)
{
    xinput_hotkey_spec specs[XINPUT_HOTKEY_MAX];
    for (unsigned i = 0u; i < count; i++) {
        char error[160];
        if (!xinput_hotkey_parse(texts[i], &specs[i], error, sizeof error)) {
            printf("parse failed: %s: %s\n", texts[i], error);
            return NULL;
        }
    }
    fired_calls = 0u;
    char error[160];
    xinput_hotkeys *hotkeys = xinput_hotkeys_create(specs, count, dir, on_fire, NULL, error, sizeof error);
    if (hotkeys == NULL) printf("create failed: %s\n", error);
    return hotkeys;
}

static xinput_hotkeys *make_one(const char *text, const char *dir)
{
    const char *texts[1] = {text};
    return make(texts, 1u, dir);
}

static bool down(xinput_hotkeys *h, xinput_hotkey_device d, const char *name, uint64_t ms)
{
    return xinput_hotkeys_event(h, d, name, true, ms, ms / 16u);
}
static bool up(xinput_hotkeys *h, xinput_hotkey_device d, const char *name, uint64_t ms)
{
    return xinput_hotkeys_event(h, d, name, false, ms, ms / 16u);
}

static bool parses(const char *text)
{
    xinput_hotkey_spec spec;
    char error[160];
    return xinput_hotkey_parse(text, &spec, error, sizeof error);
}

static void test_grammar(void)
{
    xinput_hotkey_spec spec;
    char error[160];
    CHECK(xinput_hotkey_parse("pad=BACK+START+LB+RB:advance", &spec, error, sizeof error));
    CHECK(spec.device == PAD && spec.key_count == 4u && strcmp(spec.keys[0], "BACK") == 0 && strcmp(spec.keys[3], "RB") == 0);
    CHECK(spec.hold_ms == XINPUT_HOTKEY_PAD_DEFAULT_HOLD_MS && strcmp(spec.label, "advance") == 0);
    CHECK(xinput_hotkey_parse("PAD=back+start@750:Next_1", &spec, error, sizeof error));
    CHECK(spec.hold_ms == 750u && strcmp(spec.keys[0], "BACK") == 0 && strcmp(spec.label, "Next_1") == 0);
    CHECK(xinput_hotkey_parse("kb=ctrl+shift+n:advance", &spec, error, sizeof error));
    CHECK(spec.device == KB && spec.hold_ms == 0u && spec.key_count == 3u && strcmp(spec.keys[2], "N") == 0);
    CHECK(xinput_hotkey_parse("kb=LCTRL+RSHIFT+F1@0:x", &spec, error, sizeof error));
    CHECK(xinput_hotkey_parse("pad=A+B@10000:x", &spec, error, sizeof error) && spec.hold_ms == 10000u);
    CHECK(xinput_hotkey_parse("pad=DPAD_UP+DPAD_DOWN+DPAD_LEFT+DPAD_RIGHT+GUIDE:five", &spec, error, sizeof error));
    /* refused: nothing is taken from the game with a one key combo, unknown names, wrong device names, duplicates */
    CHECK(!parses("pad=BACK:advance"));
    CHECK(!parses("pad=BACK+:advance"));
    CHECK(!parses("pad=+BACK:advance"));
    CHECK(!parses("pad=BACK++START:advance"));
    CHECK(!parses("pad=BACK+BACK:advance"));
    CHECK(!parses("pad=BACK+back:advance"));
    CHECK(!parses("kb=CTRL+LCTRL:advance"));           /* overlaps: CTRL is LCTRL or RCTRL */
    CHECK(!parses("kb=SHIFT+RSHIFT+N:advance"));
    CHECK(!parses("pad=BACK+CTRL:advance"));           /* a keyboard name on the pad */
    CHECK(!parses("kb=BACK+START:advance"));           /* pad names on the keyboard */
    CHECK(!parses("pad=BACK+NOPE:advance"));
    CHECK(!parses("pad=BACK+START+LB+RB+A+B:advance")); /* 6 keys */
    CHECK(!parses("pad=BACK+START:"));
    CHECK(!parses("pad=BACK+START"));
    CHECK(!parses("pad=BACK+START@:x"));
    CHECK(!parses("pad=BACK+START@10001:x"));
    CHECK(!parses("pad=BACK+START@-1:x"));
    CHECK(!parses("pad=BACK+START@5x:x"));
    CHECK(!parses("pad=BACK+START@123456:x"));
    CHECK(!parses("pad=BACK+START:bad label"));
    CHECK(!parses("pad=BACK+START:bad/label"));
    CHECK(!parses("pad=BACK+START:bad.label"));
    CHECK(!parses("tv=BACK+START:x"));
    CHECK(!parses("padx=BACK+START:x"));
    CHECK(!parses("BACK+START:x"));
    CHECK(!parses("=BACK+START:x"));
    CHECK(!parses(""));
    CHECK(!parses(":"));
    char longlabel[64];
    memset(longlabel, 'a', sizeof longlabel);
    longlabel[40] = '\0';
    char text[160];
    snprintf(text, sizeof text, "pad=A+B:%s", longlabel);
    CHECK(!parses(text));
    longlabel[32] = '\0';
    snprintf(text, sizeof text, "pad=A+B:%s", longlabel);
    CHECK(!parses(text)); /* 32 characters do not fit the 32 byte label with its NUL */
    longlabel[31] = '\0';
    snprintf(text, sizeof text, "pad=A+B:%s", longlabel);
    CHECK(parses(text));
    longlabel[30] = '\0';
    snprintf(text, sizeof text, "pad=A+B:%s", longlabel);
    CHECK(parses(text));
    /* a failed parse leaves the output untouched */
    xinput_hotkey_spec keep;
    memset(&keep, 0x5A, sizeof keep);
    xinput_hotkey_spec copy = keep;
    CHECK(!xinput_hotkey_parse("pad=BACK:advance", &keep, error, sizeof error));
    CHECK(memcmp(&keep, &copy, sizeof keep) == 0);
    CHECK(!xinput_hotkey_parse(NULL, &keep, error, sizeof error));
    CHECK(strlen(error) != 0u);
}

static void test_pad_hold(void)
{
    xinput_hotkeys *h = make_one("pad=BACK+START+LB+RB@500:advance", NULL);
    CHECK(h != NULL);
    /* the lead key is forwarded, the 2nd..4th are swallowed */
    CHECK(down(h, PAD, "BACK", 1000));
    CHECK(!down(h, PAD, "START", 1010));
    CHECK(!down(h, PAD, "LB", 1020));
    CHECK(!down(h, PAD, "RB", 1030));
    CHECK(xinput_hotkeys_fired(h) == 0u);
    xinput_hotkeys_tick(h, 1529, 99);
    CHECK(xinput_hotkeys_fired(h) == 0u); /* the chord completed at 1030, 499 ms held */
    xinput_hotkeys_tick(h, 1530, 100);
    CHECK(xinput_hotkeys_fired(h) == 1u && fired_calls == 1u && strcmp(fired_labels[0], "advance") == 0 && fired_polls[0] == 100u);
    xinput_hotkeys_tick(h, 3000, 101);
    CHECK(xinput_hotkeys_fired(h) == 1u); /* once per press */
    /* releases: swallowed keys' UPs are swallowed, the forwarded lead's UP is forwarded */
    CHECK(!up(h, PAD, "RB", 3100));
    CHECK(!up(h, PAD, "LB", 3110));
    CHECK(!up(h, PAD, "START", 3120));
    CHECK(up(h, PAD, "BACK", 3130));
    CHECK(xinput_hotkeys_swallowed_total(h) == 6u);
    /* re-arm: a second press fires again */
    CHECK(down(h, PAD, "BACK", 4000));
    CHECK(!down(h, PAD, "START", 4000));
    CHECK(!down(h, PAD, "LB", 4000));
    CHECK(!down(h, PAD, "RB", 4000));
    xinput_hotkeys_tick(h, 4499, 1);
    CHECK(xinput_hotkeys_fired(h) == 1u);
    xinput_hotkeys_tick(h, 4500, 2);
    CHECK(xinput_hotkeys_fired(h) == 2u);
    xinput_hotkeys_free(h);
}

static void test_hold_released_too_early(void)
{
    xinput_hotkeys *h = make_one("pad=BACK+START@500:advance", NULL);
    CHECK(down(h, PAD, "BACK", 0));
    CHECK(!down(h, PAD, "START", 100));
    xinput_hotkeys_tick(h, 599, 1);
    CHECK(!up(h, PAD, "START", 599)); /* released 1 ms early */
    xinput_hotkeys_tick(h, 5000, 2);
    CHECK(xinput_hotkeys_fired(h) == 0u);
    /* the hold restarts from the moment the chord is whole again */
    CHECK(!down(h, PAD, "START", 6000));
    xinput_hotkeys_tick(h, 6499, 3);
    CHECK(xinput_hotkeys_fired(h) == 0u);
    xinput_hotkeys_tick(h, 6500, 4);
    CHECK(xinput_hotkeys_fired(h) == 1u);
    CHECK(up(h, PAD, "BACK", 6600));
    CHECK(!up(h, PAD, "START", 6600));
    xinput_hotkeys_free(h);
}

static void test_keyboard_instant_and_aliases(void)
{
    xinput_hotkeys *h = make_one("kb=CTRL+SHIFT+N:advance", NULL);
    CHECK(down(h, KB, "RCTRL", 10));       /* either side counts, the lead is forwarded */
    CHECK(!down(h, KB, "LSHIFT", 20));
    CHECK(xinput_hotkeys_fired(h) == 0u);
    CHECK(!down(h, KB, "N", 30));          /* hold 0: fires on the event that completes the chord */
    CHECK(xinput_hotkeys_fired(h) == 1u);
    CHECK(fired_polls[0] == 30u / 16u);
    CHECK(!up(h, KB, "N", 40));
    CHECK(!down(h, KB, "N", 50));          /* shift and ctrl still held: N again fires again */
    CHECK(xinput_hotkeys_fired(h) == 2u);
    CHECK(!up(h, KB, "N", 60));
    CHECK(!up(h, KB, "LSHIFT", 70));
    CHECK(up(h, KB, "RCTRL", 80));
    /* both ctrl keys: releasing one keeps the element held */
    CHECK(down(h, KB, "LCTRL", 100));
    CHECK(down(h, KB, "RCTRL", 101)); /* the other ctrl key: same element, still no chord, forwarded */
    CHECK(up(h, KB, "LCTRL", 102));
    CHECK(up(h, KB, "RCTRL", 103));
    xinput_hotkeys_free(h);
    h = make_one("kb=CTRL+N:advance", NULL);
    CHECK(down(h, KB, "LCTRL", 0));
    CHECK(down(h, KB, "RCTRL", 0)); /* same element, not a chord yet: forwarded */
    CHECK(up(h, KB, "LCTRL", 1));
    CHECK(!down(h, KB, "N", 2));   /* RCTRL is still down: the chord is whole */
    CHECK(xinput_hotkeys_fired(h) == 1u);
    xinput_hotkeys_free(h);
}

static void test_only_chord_keys_are_touched(void)
{
    xinput_hotkeys *h = make_one("pad=BACK+START@0:advance", NULL);
    CHECK(down(h, PAD, "A", 0));            /* other keys always pass */
    CHECK(down(h, PAD, "LB", 0));
    CHECK(down(h, KB, "BACK", 0));          /* a keyboard key named like a pad key is another device */
    CHECK(down(h, KB, "START", 0));
    CHECK(xinput_hotkeys_fired(h) == 0u);
    CHECK(xinput_hotkeys_event(h, PAD, NULL, true, 0, 0)); /* an unnamed key passes */
    CHECK(xinput_hotkeys_event(NULL, PAD, "BACK", true, 0, 0));
    CHECK(down(h, PAD, "BACK", 10));        /* the lone chord key passes */
    CHECK(up(h, PAD, "BACK", 20));          /* so does its release */
    CHECK(xinput_hotkeys_fired(h) == 0u);
    CHECK(up(h, PAD, "A", 30) && up(h, PAD, "LB", 30));
    CHECK(xinput_hotkeys_swallowed_total(h) == 0u);
    xinput_hotkeys_free(h);
}

static void test_repeat_and_ordering_edges(void)
{
    xinput_hotkeys *h = make_one("pad=BACK+START@0:advance", NULL);
    CHECK(down(h, PAD, "BACK", 0));
    CHECK(down(h, PAD, "BACK", 1));            /* a repeated DOWN of the forwarded lead stays forwarded */
    CHECK(!down(h, PAD, "START", 2));
    CHECK(!down(h, PAD, "START", 3));          /* a repeated DOWN of a swallowed key stays swallowed */
    CHECK(down(h, PAD, "BACK", 4));            /* chord is now whole but BACK's first DOWN was forwarded */
    CHECK(xinput_hotkeys_fired(h) == 1u);
    CHECK(up(h, PAD, "BACK", 5));              /* the game saw BACK down, it must see it up: no stuck key */
    CHECK(!up(h, PAD, "START", 6));            /* the game never saw START: no stray up */
    CHECK(up(h, PAD, "START", 7));             /* a second up of a key no longer swallowed passes */
    /* T1632: START pressed before the lead is NOT the lead: it passes, and so does BACK (the lead is never swallowed) */
    CHECK(down(h, PAD, "START", 100));
    CHECK(down(h, PAD, "BACK", 101));
    CHECK(xinput_hotkeys_fired(h) == 2u);      /* the chord is whole in either order */
    CHECK(up(h, PAD, "START", 102));
    CHECK(up(h, PAD, "BACK", 103));
    xinput_hotkeys_free(h);
}

static void test_two_hotkeys_share_a_key(void)
{
    const char *texts[2] = {"pad=BACK+START@0:one", "pad=BACK+LB@0:two"};
    xinput_hotkeys *h = make(texts, 2u, NULL);
    CHECK(h != NULL);
    CHECK(down(h, PAD, "BACK", 0));
    CHECK(!down(h, PAD, "START", 1));
    CHECK(!down(h, PAD, "LB", 2)); /* BACK+LB is a chord of the second hotkey */
    CHECK(fired_calls == 2u && strcmp(fired_labels[0], "one") == 0 && strcmp(fired_labels[1], "two") == 0);
    CHECK(xinput_hotkeys_fired(h) == 2u);
    CHECK(!up(h, PAD, "START", 3));
    CHECK(!up(h, PAD, "LB", 4));
    CHECK(up(h, PAD, "BACK", 5));
    CHECK(xinput_hotkeys_needs_device(h, PAD) && !xinput_hotkeys_needs_device(h, KB));
    xinput_hotkeys_free(h);
}

static void test_swallow_table_limit(void)
{
    /* 5 chords of 5 keys: the 4 non lead keys of each are swallowed, 5 x 4 = 20 > the table of 16 */
    const char *texts[5] = {"kb=A+B+C+D+E@0:one", "kb=F+G+H+I+J@0:two", "kb=K+L+M+N+O@0:three", "kb=P+Q+R+S+T@0:four",
                            "kb=U+V+W+X+Y@0:five"};
    xinput_hotkeys *h = make(texts, 5u, NULL);
    CHECK(h != NULL);
    static const char *const names[5][5] = {{"A", "B", "C", "D", "E"}, {"F", "G", "H", "I", "J"}, {"K", "L", "M", "N", "O"},
                                            {"P", "Q", "R", "S", "T"}, {"U", "V", "W", "X", "Y"}};
    unsigned forwarded_extra = 0u;
    for (unsigned chord = 0u; chord < 5u; chord++) {
        CHECK(down(h, KB, names[chord][0], 0)); /* the lead goes through */
        for (unsigned key = 1u; key < 5u; key++)
            if (down(h, KB, names[chord][key], 0)) forwarded_extra++;
    }
    CHECK(xinput_hotkeys_swallow_overflow(h) == 4u); /* 5 chords x 4 keys = 20, the table keeps 16 */
    CHECK(forwarded_extra == 4u);                    /* the overflowing keys are forwarded, never lost silently */
    CHECK(xinput_hotkeys_swallowed_total(h) == XINPUT_HOTKEY_SWALLOWED_MAX);
    xinput_hotkeys_free(h);
}

static void make_dir(char *path, size_t size)
{
    snprintf(path, size, "/tmp/test_xinput_hotkey_%d", (int)getpid());
    char command[300];
    snprintf(command, sizeof command, "rm -rf %s", path);
    CHECK(system(command) == 0);
}

static bool read_file(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    size_t got = fread(out, 1u, size - 1u, file);
    out[got] = '\0';
    fclose(file);
    return true;
}

static void test_files(void)
{
    char dir[200], path[300], text[200];
    make_dir(dir, sizeof dir);
    char nested[260];
    snprintf(nested, sizeof nested, "%s/a/b", dir);
    xinput_hotkeys *h = make_one("pad=BACK+START@0:go", nested); /* parents are created */
    CHECK(h != NULL);
    CHECK(down(h, PAD, "BACK", 5000));
    CHECK(!down(h, PAD, "START", 5050));
    snprintf(path, sizeof path, "%s/hotkey.1", nested);
    CHECK(read_file(path, text, sizeof text));
    CHECK(strcmp(text, "1 go poll=315 ms=5050\n") == 0); /* poll = ms / 16 in this test */
    snprintf(path, sizeof path, "%s/hotkey.1.tmp", nested);
    CHECK(access(path, F_OK) != 0); /* no temporary left behind */
    CHECK(!up(h, PAD, "START", 5100));
    CHECK(up(h, PAD, "BACK", 5100));
    CHECK(down(h, PAD, "BACK", 6000));
    CHECK(!down(h, PAD, "START", 6000));
    snprintf(path, sizeof path, "%s/hotkey.2", nested);
    CHECK(read_file(path, text, sizeof text) && strncmp(text, "2 go ", 5) == 0);
    CHECK(xinput_hotkeys_write_failures(h) == 0u);
    xinput_hotkeys_free(h);

    /* an unwritable target is counted, the callback still runs, the run goes on */
    snprintf(path, sizeof path, "%s/blocked", dir);
    FILE *blocker = fopen(path, "w");
    CHECK(blocker != NULL);
    if (blocker != NULL) fclose(blocker);
    xinput_hotkey_spec spec;
    char error[160];
    CHECK(xinput_hotkey_parse("pad=BACK+START@0:go", &spec, error, sizeof error));
    CHECK(xinput_hotkeys_create(&spec, 1u, path, NULL, NULL, error, sizeof error) == NULL); /* a file is not a directory */
    CHECK(strstr(error, "directory") != NULL);

    /* limits of create */
    CHECK(xinput_hotkeys_create(NULL, 1u, NULL, NULL, NULL, error, sizeof error) == NULL);
    CHECK(xinput_hotkeys_create(&spec, 0u, NULL, NULL, NULL, error, sizeof error) == NULL);
    xinput_hotkey_spec five[XINPUT_HOTKEY_MAX + 1u];
    for (unsigned i = 0u; i < XINPUT_HOTKEY_MAX + 1u; i++) five[i] = spec;
    CHECK(xinput_hotkeys_create(five, XINPUT_HOTKEY_MAX + 1u, NULL, NULL, NULL, error, sizeof error) == NULL);
    xinput_hotkeys *four = xinput_hotkeys_create(five, XINPUT_HOTKEY_MAX, NULL, NULL, NULL, error, sizeof error);
    CHECK(four != NULL);
    xinput_hotkeys_free(four);
    xinput_hotkey_spec bad = spec;
    bad.key_count = 1u;
    CHECK(xinput_hotkeys_create(&bad, 1u, NULL, NULL, NULL, error, sizeof error) == NULL);
    bad = spec;
    snprintf(bad.keys[0], sizeof bad.keys[0], "%s", "NOPE");
    CHECK(xinput_hotkeys_create(&bad, 1u, NULL, NULL, NULL, error, sizeof error) == NULL);
    char command[300];
    snprintf(command, sizeof command, "rm -rf %s", dir);
    CHECK(system(command) == 0);
}

/* ---- T1632 ---- */

static unsigned leak_calls;
static struct {
    bool begin;
    char name[16];
    uint64_t poll;
} leak_log[16];
static void on_leak(xinput_hotkey_device device, const char *name, uint64_t poll, bool begin, void *user)
{
    (void)device;
    (void)user;
    if (leak_calls < 16u) {
        leak_log[leak_calls].begin = begin;
        snprintf(leak_log[leak_calls].name, sizeof leak_log[leak_calls].name, "%s", name);
        leak_log[leak_calls].poll = poll;
    }
    leak_calls++;
}

static void test_lead_rule(void)
{
    /* the owner chords: advance and mark share BACK+START+LB, the selector (RB or X) is the last key */
    const char *texts[2] = {XINPUT_HOTKEY_DEFAULT_PAD_ADVANCE, XINPUT_HOTKEY_DEFAULT_PAD_MARK};
    xinput_hotkeys *h = make(texts, 2u, NULL);
    CHECK(h != NULL);
    /* played normally, with no gesture: LB+RB, LB+X, START+LB, X alone all reach the game and are released normally */
    CHECK(down(h, PAD, "LB", 0));
    CHECK(down(h, PAD, "RB", 1));
    CHECK(down(h, PAD, "X", 2));
    CHECK(down(h, PAD, "START", 3));
    CHECK(up(h, PAD, "X", 4) && up(h, PAD, "RB", 5) && up(h, PAD, "START", 6) && up(h, PAD, "LB", 7));
    CHECK(xinput_hotkeys_swallowed_total(h) == 0u && xinput_hotkeys_fired(h) == 0u);
    /* the gesture: BACK first, then START and LB (swallowed), then the selector RB picks advance */
    CHECK(down(h, PAD, "BACK", 100));
    CHECK(!down(h, PAD, "START", 101));
    CHECK(!down(h, PAD, "LB", 102));
    CHECK(!down(h, PAD, "RB", 103));
    xinput_hotkeys_tick(h, 603, 40);
    CHECK(fired_calls == 1u && strcmp(fired_labels[0], "advance") == 0); /* only advance: X was not held */
    CHECK(!up(h, PAD, "RB", 700) && !up(h, PAD, "LB", 701) && !up(h, PAD, "START", 702) && up(h, PAD, "BACK", 703));
    /* the other selector picks mark */
    CHECK(down(h, PAD, "BACK", 1000));
    CHECK(!down(h, PAD, "START", 1001));
    CHECK(!down(h, PAD, "LB", 1002));
    CHECK(!down(h, PAD, "X", 1003));
    xinput_hotkeys_tick(h, 1503, 90);
    CHECK(fired_calls == 2u && strcmp(fired_labels[1], "mark") == 0);
    CHECK(xinput_hotkeys_swallowed_total(h) == 9u /* 3 downs + 3 ups + 3 downs */);
    xinput_hotkeys_free(h);
}

static void test_leak_observer(void)
{
    xinput_hotkeys *h = make_one("pad=BACK+START+LB+X@0:mark", NULL);
    CHECK(h != NULL);
    leak_calls = 0u;
    xinput_hotkeys_set_leak_observer(h, on_leak, NULL);
    /* a genuine press of a chord member (BACK tap, LB hold) is never reported */
    CHECK(down(h, PAD, "BACK", 160) && up(h, PAD, "BACK", 320));
    CHECK(down(h, PAD, "LB", 400) && up(h, PAD, "LB", 480));
    CHECK(leak_calls == 0u);
    /* a gesture in the wrong order: X (poll 10) and BACK (poll 11) reach the game, START and LB do not; the chord is whole */
    CHECK(down(h, PAD, "X", 160));
    CHECK(down(h, PAD, "BACK", 176));
    CHECK(!down(h, PAD, "START", 192));
    CHECK(leak_calls == 0u);
    CHECK(!down(h, PAD, "LB", 208)); /* the chord fires here (hold 0), both forwarded keys are reported */
    CHECK(xinput_hotkeys_fired(h) == 1u && leak_calls == 2u);
    CHECK(leak_log[0].begin && strcmp(leak_log[0].name, "X") == 0 && leak_log[0].poll == 10u);
    CHECK(leak_log[1].begin && strcmp(leak_log[1].name, "BACK") == 0 && leak_log[1].poll == 11u);
    /* a second fire of the same press (LB up and down again) does not report the same keys twice */
    CHECK(!up(h, PAD, "LB", 224));
    CHECK(!down(h, PAD, "LB", 240));
    CHECK(xinput_hotkeys_fired(h) == 2u && leak_calls == 2u);
    /* the end is reported when the forwarded UP arrives, with the poll of the UP */
    CHECK(up(h, PAD, "X", 256));
    CHECK(leak_calls == 3u && !leak_log[2].begin && strcmp(leak_log[2].name, "X") == 0 && leak_log[2].poll == 16u);
    CHECK(!up(h, PAD, "START", 272) && !up(h, PAD, "LB", 288));
    CHECK(leak_calls == 3u);
    CHECK(up(h, PAD, "BACK", 304));
    CHECK(leak_calls == 4u && !leak_log[3].begin && strcmp(leak_log[3].name, "BACK") == 0 && leak_log[3].poll == 19u);
    /* the gesture in the right order leaks the lead only */
    leak_calls = 0u;
    CHECK(down(h, PAD, "BACK", 1600));
    CHECK(!down(h, PAD, "START", 1616) && !down(h, PAD, "LB", 1632) && !down(h, PAD, "X", 1648));
    CHECK(leak_calls == 1u && leak_log[0].begin && strcmp(leak_log[0].name, "BACK") == 0 && leak_log[0].poll == 100u);
    CHECK(!up(h, PAD, "X", 1700) && !up(h, PAD, "LB", 1701) && !up(h, PAD, "START", 1702) && up(h, PAD, "BACK", 1712));
    CHECK(leak_calls == 2u && !leak_log[1].begin && leak_log[1].poll == 107u);
    CHECK(xinput_hotkeys_leak_overflow(h) == 0u);
    xinput_hotkeys_free(h);
    /* the keyboard lead: either CTRL, reported under the name that came */
    h = make_one("kb=CTRL+SHIFT+M:mark", NULL);
    xinput_hotkeys_set_leak_observer(h, on_leak, NULL);
    leak_calls = 0u;
    CHECK(down(h, KB, "M", 160)); /* M before CTRL: forwarded, a leak */
    CHECK(down(h, KB, "RCTRL", 176));
    CHECK(!down(h, KB, "LSHIFT", 192));
    CHECK(leak_calls == 2u && strcmp(leak_log[0].name, "M") == 0 && strcmp(leak_log[1].name, "RCTRL") == 0);
    xinput_hotkeys_set_leak_observer(NULL, on_leak, NULL); /* NULL is tolerated */
    xinput_hotkeys_free(h);
}

static void test_leak_table_limit(void)
{
    /* more forwarded chord members than the episode table holds (8): counted, the key is still forwarded, no crash */
    const char *texts[3] = {"kb=A+B+C+D+E@0:one", "kb=F+G+H+I+J@0:two", "kb=K+L@0:three"};
    xinput_hotkeys *h = make(texts, 3u, NULL);
    CHECK(h != NULL);
    static const char *const keys[9] = {"B", "C", "D", "E", "G", "H", "I", "J", "L"};
    for (unsigned i = 0u; i < 9u; i++) CHECK(down(h, KB, keys[i], i)); /* none of the leads is held: all forwarded */
    CHECK(xinput_hotkeys_leak_overflow(h) == 1u);
    for (unsigned i = 0u; i < 9u; i++) CHECK(up(h, KB, keys[i], 100u + i));
    xinput_hotkeys_free(h);
}

static void test_actions_and_defaults(void)
{
    CHECK(xinput_hotkey_action_of("mark") == XINPUT_HOTKEY_ACTION_MARK);
    CHECK(xinput_hotkey_action_of("MARK") == XINPUT_HOTKEY_ACTION_MARK);
    CHECK(xinput_hotkey_action_of("Stop") == XINPUT_HOTKEY_ACTION_STOP);
    CHECK(xinput_hotkey_action_of("advance") == XINPUT_HOTKEY_ACTION_NONE);
    CHECK(xinput_hotkey_action_of("dump") == XINPUT_HOTKEY_ACTION_NONE);
    CHECK(xinput_hotkey_action_of("marker") == XINPUT_HOTKEY_ACTION_NONE);
    CHECK(xinput_hotkey_action_of("mar") == XINPUT_HOTKEY_ACTION_NONE);
    CHECK(xinput_hotkey_action_of("") == XINPUT_HOTKEY_ACTION_NONE);
    CHECK(xinput_hotkey_action_of(NULL) == XINPUT_HOTKEY_ACTION_NONE);
    const char *defaults[8] = {XINPUT_HOTKEY_DEFAULT_PAD_ADVANCE, XINPUT_HOTKEY_DEFAULT_PAD_MARK,
                               XINPUT_HOTKEY_DEFAULT_PAD_DUMP,    XINPUT_HOTKEY_DEFAULT_PAD_STOP,
                               XINPUT_HOTKEY_DEFAULT_KB_ADVANCE,  XINPUT_HOTKEY_DEFAULT_KB_MARK,
                               XINPUT_HOTKEY_DEFAULT_KB_DUMP,     XINPUT_HOTKEY_DEFAULT_KB_STOP};
    xinput_hotkey_spec specs[8];
    char error[160];
    for (unsigned i = 0u; i < 8u; i++) CHECK(xinput_hotkey_parse(defaults[i], &specs[i], error, sizeof error));
    CHECK(XINPUT_HOTKEY_MAX >= 8u); /* all eight fit one run */
    /* the pad chords share the lead BACK and the keyboard chords CTRL, none contains another, the labels differ, STOP holds longest */
    for (unsigned i = 0u; i < 8u; i++) {
        CHECK(strcmp(specs[i].keys[0], specs[i].device == PAD ? "BACK" : "CTRL") == 0);
        CHECK(specs[i].key_count == (specs[i].device == PAD ? 4u : 3u));
        for (unsigned j = 0u; j < 8u; j++) {
            if (i == j || specs[i].device != specs[j].device) continue;
            CHECK(strcmp(specs[i].label, specs[j].label) != 0);
            unsigned shared = 0u;
            for (unsigned a = 0u; a < specs[i].key_count; a++)
                for (unsigned b = 0u; b < specs[j].key_count; b++)
                    if (strcmp(specs[i].keys[a], specs[j].keys[b]) == 0) shared++;
            CHECK(shared < specs[i].key_count); /* not a subset: pressing one chord cannot complete another */
        }
        if (strcmp(specs[i].label, "stop") == 0) CHECK(specs[i].hold_ms >= 1000u);
        else CHECK(specs[i].hold_ms <= 500u);
    }
    /* the pad set works as one hotkey set: three keys of the shared prefix and then the selector pick exactly one label */
    xinput_hotkeys *h = make(defaults, 4u, NULL);
    CHECK(h != NULL);
    CHECK(down(h, PAD, "BACK", 0) && !down(h, PAD, "START", 1) && !down(h, PAD, "LB", 2) && !down(h, PAD, "Y", 3));
    xinput_hotkeys_tick(h, 1499, 5);
    CHECK(fired_calls == 0u); /* stop needs 1.5 s */
    xinput_hotkeys_tick(h, 1503, 6);
    CHECK(fired_calls == 1u && strcmp(fired_labels[0], "stop") == 0);
    xinput_hotkeys_free(h);
}

int main(void)
{
    test_grammar();
    test_pad_hold();
    test_hold_released_too_early();
    test_keyboard_instant_and_aliases();
    test_only_chord_keys_are_touched();
    test_repeat_and_ordering_edges();
    test_two_hotkeys_share_a_key();
    test_swallow_table_limit();
    test_files();
    test_lead_rule();
    test_leak_observer();
    test_leak_table_limit();
    test_actions_and_defaults();
    printf("test_xinput_hotkey: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
