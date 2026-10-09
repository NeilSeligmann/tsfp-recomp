/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1226: opt-in per draw signature dump of the live renderer, observation only. TSFP_LIVE_DRAW_DUMP=PATH appends one text line per
 * draw (D), per refused draw (R) and per texture stage (T). Nothing changes when the variable is unset. Header only (static), so no
 * test link list needs a new object. Every translation unit appends through its own FILE with line buffering.
 */
#ifndef TSFP_LIVE_DRAW_DUMP_H
#define TSFP_LIVE_DRAW_DUMP_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static inline FILE *live_draw_dump_file(void)
{
    static FILE *file;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *path = getenv("TSFP_LIVE_DRAW_DUMP");
        if (path != NULL && path[0] != '\0') {
            file = fopen(path, "a");
            if (file != NULL) setvbuf(file, NULL, _IOLBF, 0);
        }
    }
    return file;
}

static inline void live_draw_dump_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static inline void live_draw_dump_line(const char *format, ...)
{
    FILE *file = live_draw_dump_file();
    if (file == NULL) return;
    va_list arguments;
    va_start(arguments, format);
    (void)vfprintf(file, format, arguments);
    va_end(arguments);
    (void)fputc('\n', file);
}

#endif
