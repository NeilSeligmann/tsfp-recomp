/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T1636: load a route through the host's own xinput_replay_load (header, trailer, body, marks) and print what the replay would
 * feed the title: the marks, then one line per poll. tests/test_t1636_route_trim.py compares the dumps of an original route and
 * its trimmed copy. Usage: t1636_replay_dump ROUTE XBE_SHA256 FLAGS_SHA256 FLAGS_LINE */
#include <stdint.h>
#include <stdio.h>

#include "xinput_hle.h"
#include "xinput_record.h"
#include "xinput_source.h"

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s ROUTE XBE_SHA256 FLAGS_SHA256 FLAGS\n", argv[0]); return 2; }
    char error[512] = {0};
    uint64_t total = 0u;
    if (!xinput_replay_load(argv[1], argv[2], argv[3], argv[4], error, sizeof error, &total)) {
        printf("REFUSED %s\n", error);
        return 1;
    }
    const uint64_t *marks = NULL;
    const size_t count = xinput_replay_marks(&marks);
    printf("total %llu marks %zu", (unsigned long long)total, count);
    for (size_t i = 0u; i < count; i++) printf(" %llu", (unsigned long long)marks[i]);
    printf("\n");
    const xinput_script *script = xinput_replay_script();
    for (uint64_t i = 0u; i < total; i++) {
        const xinput_pad_state st = xinput_script_state_at(script, i);
        printf("%llu %04x", (unsigned long long)i, st.digital_buttons);
        for (unsigned k = 0u; k < XINPUT_ANALOG_COUNT; k++) printf(" %02x", st.analog[k]);
        printf(" %d %d %d %d\n", st.thumb_left_x, st.thumb_left_y, st.thumb_right_x, st.thumb_right_y);
    }
    return 0;
}
