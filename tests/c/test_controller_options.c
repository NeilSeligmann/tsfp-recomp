/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_options.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    options o;
    char *defaults[] = {"host", "game.xbe"};
    CHECK(parse_options(2, defaults, &o)); CHECK(!o.controllers); CHECK(o.controller_config == NULL);
    char *enabled[] = {"host", "game.xbe", "--controllers", "--present", "window", "--controller-config", "pads.conf", "--controller-mappings", "local.txt", "--controller-save", "saved.conf"};
    CHECK(parse_options(11, enabled, &o)); CHECK(o.controllers); CHECK(strcmp(o.controller_config, "pads.conf") == 0);
    char *keyboard[] = {"host", "game.xbe", "--controllers", "--present", "window", "--pad-source", "keyboard"};
    CHECK(parse_options(7, keyboard, &o));
    char *script[] = {"host", "game.xbe", "--controllers", "--present", "window", "--pad-script", "input.txt"};
    CHECK(parse_options(7, script, &o));
    char *no_window[] = {"host", "game.xbe", "--controllers"}; CHECK(!parse_options(3, no_window, &o));
    char *disabled_file[] = {"host", "game.xbe", "--controller-config", "pads.conf"}; CHECK(!parse_options(4, disabled_file, &o));
    char *duplicate_provider[] = {"host", "game.xbe", "--controllers", "--present", "window", "--pad-source", "gamepad"}; CHECK(!parse_options(7, duplicate_provider, &o));
    char *fake_feed[] = {"host", "game.xbe", "--controllers", "--present", "window", "--pad-feed", "events.txt"}; CHECK(!parse_options(7, fake_feed, &o));
    char *missing[] = {"host", "game.xbe", "--controllers", "--present", "window", "--controller-save"}; CHECK(!parse_options(6, missing, &o));
    printf("T942 controller options: %u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
}
