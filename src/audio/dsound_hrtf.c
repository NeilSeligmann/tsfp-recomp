/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_hrtf.h"
#include "dsound_hle.h"
#include "kernel_call.h"
#include "kernel_critsec.h"
#include <stdlib.h>
#define ENTRY 0x00406AB6u
#define CS 0x004124B4u
#define TABLE 0x00412B5Cu
static dsound_hrtf_irql_provider irql_provider;
static dsound_hrtf_fatal_fn fatal_callback;
static dsound_hrtf_critical_call enter_call = kernel_critsec_enter_guest;
static dsound_hrtf_critical_call leave_call = kernel_critsec_leave_guest;
void dsound_hrtf_set_irql_provider(dsound_hrtf_irql_provider provider) { irql_provider = provider; }
void dsound_hrtf_set_fatal(dsound_hrtf_fatal_fn fatal) { fatal_callback = fatal; }
void dsound_hrtf_set_critical_calls(dsound_hrtf_critical_call enter, dsound_hrtf_critical_call leave)
{
    enter_call = enter != NULL ? enter : kernel_critsec_enter_guest;
    leave_call = leave != NULL ? leave : kernel_critsec_leave_guest;
}
static _Noreturn void refuse(const char *reason)
{
    dsound_hle_log()("dsound: DirectSoundUseLightHRTF -- REFUSED: %s\n", reason);
    if (fatal_callback != NULL) fatal_callback(ENTRY, reason);
    abort();
}
uint32_t dsound_use_light_hrtf(void)
{
    uint8_t irql;
    if (irql_provider == NULL || !irql_provider(&irql)) refuse("calling thread IRQL byte is unreadable");
    if (kernel_guest_at(TABLE, 44u) == NULL || kernel_guest_at(CS, 28u) == NULL)
        refuse("HRTF table or critical-section span is unmapped");
    if (irql == 0u) {
        (void)enter_call(CS);
        kernel_critsec_info state;
        if (!kernel_critsec_state(CS, &state) || state.owner != kernel_critsec_owner_token() ||
            state.recursion == 0u) refuse("passive critical-section acquisition was not tracked");
    }
    static const uint32_t functions[11] = {
        0x00409D06u, 0x00409D40u, 0x00409A0Bu, 0x00409AA1u, 0x00409AECu,
        0x00409BFDu, 0x00409E28u, 0x00409EDFu, 0x00409C8Du, 0x00409F48u, 4u
    };
    for (unsigned i = 0u; i < 11u; i++)
        (void)kernel_guest_write_u32(TABLE + i * 4u, functions[i]);
    return irql == 0u ? leave_call(CS) : 0u;
}
static uint32_t handler(void *context) { (void)context; return dsound_use_light_hrtf(); }
unsigned dsound_hrtf_register(void) { return (unsigned)dsound_hle_register(ENTRY, handler); }
