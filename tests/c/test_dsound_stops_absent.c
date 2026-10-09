/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T452: a build whose host does not define recomp_has_stop_boundary must report the buffer stop
 * boundaries as NOT ready, never call through the absent weak symbol. Every other buffer suite
 * defines the symbol, so only this binary reaches the NULL guard (mutant dsd-buf-stops-missing). */
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
int main(void)
{
    CHECK(!dsound_buffer_stops_ready());
    printf("dsound stops absent: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
