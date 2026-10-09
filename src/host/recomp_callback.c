/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_callback.h"
#include "recomp_abi.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <fenv.h>
#include <string.h>
#include <stdlib.h>

/* Declared here too so the host builds against a lift made before the flag bridge. */
extern _Thread_local uint32_t g_flag_bridge_eflags, g_flag_bridge_mask;

static _Thread_local bool active;
static bool dispatch_level;
typedef struct {
    uint32_t r[10]; int df,top,cmp; uint16_t cw,cc; uint32_t bridge_flags,bridge_mask;
    double fp[8]; RecompXmm xmm[8]; RecompMmx mm[8]; fenv_t env;
} context;
static void capture(context *s) {
    uint32_t r[]={g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_fs_base,g_seh_ebp};
    RecompXmm x[]={g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
    RecompMmx m[]={g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7};
    memcpy(s->r,r,sizeof r); memcpy(s->xmm,x,sizeof x);memcpy(s->mm,m,sizeof m);
    memcpy(s->fp,g_fp_stack,sizeof s->fp);s->df=g_df;s->top=g_fp_top;
    s->cmp=g_fp_cmp;s->cw=g_fp_control_word;s->cc=g_fp_cc;
    s->bridge_flags=g_flag_bridge_eflags;s->bridge_mask=g_flag_bridge_mask;
}
static void restore(const context *s) {
    g_eax=s->r[0];g_ecx=s->r[1];g_edx=s->r[2];g_esp=s->r[3];
    g_ebx=s->r[4];g_esi=s->r[5];g_edi=s->r[6];g_ebp=s->r[7];
    g_fs_base=s->r[8];g_seh_ebp=s->r[9];g_df=s->df;g_fp_top=s->top;
    g_fp_cmp=s->cmp;g_fp_control_word=s->cw;g_fp_cc=s->cc;
    g_flag_bridge_eflags=s->bridge_flags;g_flag_bridge_mask=s->bridge_mask;
    memcpy(g_fp_stack,s->fp,sizeof s->fp);
    g_xmm0=s->xmm[0];g_xmm1=s->xmm[1];g_xmm2=s->xmm[2];g_xmm3=s->xmm[3];
    g_xmm4=s->xmm[4];g_xmm5=s->xmm[5];g_xmm6=s->xmm[6];g_xmm7=s->xmm[7];
    g_mm0=s->mm[0];g_mm1=s->mm[1];g_mm2=s->mm[2];g_mm3=s->mm[3];
    g_mm4=s->mm[4];g_mm5=s->mm[5];g_mm6=s->mm[6];g_mm7=s->mm[7];
    if(fesetenv(&s->env)!=0)abort();
}
void recomp_callback_set_dispatch_level(bool enabled) { dispatch_level=enabled; }
void recomp_callback_machine_snapshot(recomp_machine_snapshot *out) {
    const uint32_t r[8]={g_eax,g_ecx,g_edx,g_ebx,g_esi,g_edi,g_ebp,g_esp};
    const RecompXmm x[8]={g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
    const RecompMmx m[8]={g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7};
    memcpy(out->registers,r,sizeof r);
    for(int i=0;i<8;i++){out->mmx[i]=m[i].q;memcpy(out->xmm[i],&x[i],16u);}
    memcpy(out->x87,g_fp_stack,sizeof out->x87);
    out->x87_top=(uint32_t)g_fp_top;out->x87_control=g_fp_control_word;
    out->x87_compare=(uint32_t)g_fp_cmp;out->x87_condition=g_fp_cc;
}
bool recomp_callback_run(uint32_t address,uint32_t low,uint32_t high,const uint32_t payload[3]) {
    extern int g_force_return;
    if(active||!host_run_armed()||g_force_return||address!=0x22020u||!payload||
       !low||high<=low||high-low<64u||(high&15u)||!g_fs_base||
       (g_fs_base&3u)||g_fs_base>UINT32_MAX-KERNEL_THREAD_CONTROL_BYTES||
       (g_fs_base<high&&g_fs_base+KERNEL_THREAD_CONTROL_BYTES>low)||(g_esp>=low&&g_esp<high)||
       (dispatch_level&&kernel_sync_current_irql()>KERNEL_IRQL_DISPATCH))return false;
    recomp_func_t fn=recomp_lookup(address);if(!fn)return false;
    context saved;capture(&saved);if(fegetenv(&saved.env)!=0)return false;
    /* Same-byte writes preflight permissions without changing guest content.
     * Exclusive/quiescent mappings keep these permissions stable through cleanup.
     * Only the measured leaf is admitted; it needs the 20-byte frame, not a
     * general stack-depth or exception-unwinding model. */
    uint32_t head,frame[5]={0u,high-12u,payload[0],payload[1],payload[2]};
    uint32_t at=high-20u;unsigned char probe[20];
    if(!kernel_guest_read_bytes(g_fs_base,&head,4)||
       !kernel_guest_write_bytes(g_fs_base,&head,4)||
       !kernel_guest_read_bytes(at,probe,sizeof probe)||
       !kernel_guest_write_bytes(at,probe,sizeof probe))return false;
    volatile host_run_scope scope=HOST_RUN_SCOPE_INITIALIZER;
    volatile bool raised=false;volatile uint32_t previous_irql=KERNEL_IRQL_PASSIVE;
    if(!host_run_scope_init(&scope))return false;
    if(sigsetjmp(*host_run_scope_jmp(&scope),0)!=0){
        const host_stop stopped=*host_run_result();
        if(!kernel_guest_write_bytes(saved.r[8],&head,4))abort();
        restore(&saved);active=false;
        if(raised)kernel_sync_restore_irql(previous_irql);
        if(!host_run_scope_pop(&scope))abort();
        host_run_rethrow(&stopped);
    }
    if(!host_run_scope_push(&scope))return false;
    uint32_t isolated=UINT32_MAX;
    bool ok=kernel_guest_write_bytes(at,frame,sizeof frame)&&
            kernel_guest_write_bytes(saved.r[8],&isolated,4);
    if(ok){
        active=true;
        /* T370: the original DPC path runs the helper and callback at DISPATCH_LEVEL. */
        if(dispatch_level){previous_irql=kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);raised=true;}
        g_esp=at;fn();ok=g_esp==at+4u;
    }
    if(!kernel_guest_write_bytes(saved.r[8],&head,4))abort();
    restore(&saved);active=false;
    if(raised)kernel_sync_restore_irql(previous_irql);
    if(!host_run_scope_pop(&scope))abort();return ok;
}

bool recomp_callback_run_xnet_dpc(uint32_t address, uint32_t low, uint32_t high,
                                 const uint32_t arguments[4], host_stop *stopped, bool *captured_stop)
{
    extern int g_force_return;
    if (!stopped || !captured_stop) return false;
    *captured_stop = false;
    memset(stopped, 0, sizeof(*stopped));
    if (active || !host_run_armed() || g_force_return || address != 0x43A184u ||
        !arguments || !low || high <= low || high - low < 64u || (high & 15u) ||
        !g_fs_base || (g_fs_base & 3u) ||
        g_fs_base > UINT32_MAX - KERNEL_THREAD_CONTROL_BYTES ||
        (g_fs_base < high && g_fs_base + KERNEL_THREAD_CONTROL_BYTES > low) ||
        (g_esp >= low && g_esp < high) ||
        kernel_sync_current_irql() > KERNEL_IRQL_DISPATCH) return false;
    recomp_func_t fn = recomp_lookup(address);
    if (!fn) return false;
    context saved;
    capture(&saved);
    if (fegetenv(&saved.env) != 0) return false;
    uint32_t head;
    if (!kernel_guest_read_bytes(g_fs_base, &head, 4u) ||
        !kernel_guest_write_bytes(g_fs_base, &head, 4u)) return false;
    /* Full supplied stack is required, not just a frame for a guessed leaf. */
    unsigned char probe[64];
    for (uint32_t at = low; at < high;) {
        const uint32_t bytes = high - at < sizeof(probe) ? high - at : sizeof(probe);
        if (!kernel_guest_read_bytes(at, probe, bytes) ||
            !kernel_guest_write_bytes(at, probe, bytes)) return false;
        at += bytes;
    }
    const uint32_t at = high - 20u;
    const uint32_t frame[5] = {0u, arguments[0], arguments[1], arguments[2], arguments[3]};
    volatile host_run_scope scope = HOST_RUN_SCOPE_INITIALIZER;
    volatile bool raised = false;
    volatile uint32_t previous_irql = KERNEL_IRQL_PASSIVE;
    if (!host_run_scope_init(&scope)) return false;
    if (sigsetjmp(*host_run_scope_jmp(&scope), 0) != 0) {
        *stopped = *host_run_result();
        *captured_stop = true;
        if (!kernel_guest_write_bytes(saved.r[8], &head, 4u)) abort();
        restore(&saved);
        active = false;
        if (raised) kernel_sync_restore_irql(previous_irql);
        if (!host_run_scope_pop(&scope)) abort();
        return false;
    }
    if (!host_run_scope_push(&scope)) return false;
    const uint32_t isolated = UINT32_MAX;
    bool ok = kernel_guest_write_bytes(at, frame, sizeof(frame)) &&
              kernel_guest_write_bytes(saved.r[8], &isolated, 4u);
    if (ok) {
        active = true;
        previous_irql = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);
        raised = true;
        g_esp = at;
        fn();
        ok = g_esp == high; /* RET16 removes return address and four DWORDs. */
    }
    if (!kernel_guest_write_bytes(saved.r[8], &head, 4u)) abort();
    restore(&saved);
    active = false;
    if (raised) kernel_sync_restore_irql(previous_irql);
    if (!host_run_scope_pop(&scope)) abort();
    return ok;
}
