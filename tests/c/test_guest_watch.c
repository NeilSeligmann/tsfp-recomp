/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1741: the pure parts of the guest write watch (--watch-write): limits, hit filter, grammar reuse, option flags, help.
 */
#include "guest_watch.h"

#include "host_options.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static void test_filter(void)
{
    CHECK(guest_watch_is_hit(0x1000u, 0x1000u, 4u, false));
    CHECK(guest_watch_is_hit(0x1003u, 0x1000u, 4u, false));
    CHECK(!guest_watch_is_hit(0x1004u, 0x1000u, 4u, false)); /* just past the range, nothing changed */
    CHECK(guest_watch_is_hit(0x1004u, 0x1000u, 4u, true));   /* a straddling store that changed it */
    CHECK(!guest_watch_is_hit(0x0FF1u, 0x1000u, 4u, false)); /* a neighbour store below that left the range alone */
    CHECK(guest_watch_is_hit(0x0FF1u, 0x1000u, 4u, true));   /* a wide store from below that changed it */
    CHECK(!guest_watch_is_hit(0x0FFFu, 0x1000u, 4u, false)); /* one byte below */
    CHECK(!guest_watch_is_hit(0u, 4u, 4u, false));           /* no unsigned wrap near zero */
    CHECK(guest_watch_is_hit(0u, 0u, 4u, false));
    CHECK(!guest_watch_is_hit(0xFFFFFFFFu, 0x10u, 4u, false));
}

static void test_limits(void)
{
    guest_dump_set set;
    char error[64];
    memset(&set, 0, sizeof set);
    CHECK(!guest_watch_check(&set, error, sizeof error) && strcmp(error, "no range") == 0);
    CHECK(guest_dump_parse_append(&set, "**0x7A3580+0+0x1CC:4", NULL, 0u));
    CHECK(guest_watch_check(&set, NULL, 0u));
    CHECK(guest_dump_parse_append(&set, "0x1000:64,0x2000:4", NULL, 0u));
    CHECK(guest_watch_check(&set, NULL, 0u));
    CHECK(guest_dump_parse_append(&set, "0x3000:4", NULL, 0u));
    CHECK(guest_dump_parse_append(&set, "0x4000:4", NULL, 0u));
    CHECK(!guest_watch_check(&set, error, sizeof error) && strcmp(error, "too many ranges") == 0);
    memset(&set, 0, sizeof set);
    CHECK(guest_dump_parse_append(&set, "0x1000:65", NULL, 0u));
    CHECK(!guest_watch_check(&set, error, sizeof error) && strstr(error, "64") != NULL);
}

static void test_options(void)
{
    options options;
    char *good[] = {"host", "--watch-write", "**0x7A3580+0+0x1CC:4", "--watch-write", "0x7A0000:8", "--watch-write-max", "10",
                    "--watch-write-log", "w.log", "game.xbe"};
    CHECK(parse_options(10, good, &options));
    CHECK(options.watch_write_set.count == 2u && options.watch_write_max == 10u && strcmp(options.watch_write_log, "w.log") == 0);
    char *oversize[] = {"host", "--watch-write", "0x1000:65", "game.xbe"};
    CHECK(!parse_options(4, oversize, &options));
    char *zero[] = {"host", "--watch-write-max", "0", "game.xbe"};
    CHECK(!parse_options(4, zero, &options));
    char *huge[] = {"host", "--watch-write-max", "65537", "game.xbe"};
    CHECK(!parse_options(4, huge, &options));
    char *five[] = {"host", "--watch-write", "0x1000:4,0x2000:4,0x3000:4,0x4000:4,0x5000:4", "game.xbe"};
    CHECK(!parse_options(4, five, &options));
    char *none[] = {"host", "game.xbe"};
    CHECK(parse_options(2, none, &options) && options.watch_write_set.count == 0u && options.watch_write_max == 4096u);
}

static void test_help(void)
{
    const char *help = host_options_watch_write_help();
    CHECK(help != NULL && strstr(help, "T1741") != NULL);
    CHECK(strstr(help, "--watch-write-log") != NULL && strstr(help, "--watch-write-max") != NULL);
    CHECK(strstr(help, "PAGE costs two") != NULL);
}

int main(void)
{
    test_filter();
    test_limits();
    test_options();
    test_help();
    printf(failures == 0 ? "OK test_guest_watch\n" : "test_guest_watch: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
