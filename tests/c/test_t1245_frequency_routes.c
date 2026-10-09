/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_completion.h"
#include "dsound_audio_runtime.h"
#include "dsound_hle.h"
#include "dsound_stream.h"
#include "host_runtime.h"
#include "kernel_clock.h"
#include "recomp_abi.h"
/* Exact TLS declarations from the generated ABI, without its inline helpers. */
extern TSFP_RECOMP_TLS uint32_t g_seh_ebp;
extern TSFP_RECOMP_TLS int g_fp_top;
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
static uint64_t fixture_now;
static uint64_t fixed_clock(void) {return fixture_now;}
static bool control_word(uint16_t *out) {*out=g_fp_control_word;return true;}
static bool irql(uint8_t *out) {*out=0u;return true;}
/* Constructed full game-caller fixture; not a positive retail-run observation. */
static void complete_game_caller(uint32_t stream,uint32_t stack)
{
    map_fixed(0x580000u,0xC0000u);map_fixed(0x565000u,4096u);
    map_fixed(0x4CA000u,4096u);
    const uint32_t table=0x580000u,identifier=0x10000018u;
    store(0x5836C0u,0x583690u);store(0x5659F4u,0x5659C4u);
    store(0x4CA9B4u,table);
    const uint16_t normalized[3]={1881u,2736u,4096u};
    const uint32_t selectors[3]={20u,29u,2u};
    store(0x475CACu,0u);store(0x4761D0u,0x39800000u); /* exact original 1/4096 */
    const uint32_t slots[3]={0u,5u,9u},ratios[3]={0x3F000000u,0x3F800000u,0x3FC00000u};
    const uint32_t expected[3][3]={{11015u,22042u,33058u},{16031u,32062u,48093u},{24000u,48000u,72000u}};
    const uint16_t cw[3]={0x003Fu,0x027Fu,0x037Fu};
    for(unsigned n=0u;n<3u;n++)for(unsigned c=0u;c<3u;c++)for(unsigned s=0u;s<3u;s++)for(unsigned r=0u;r<3u;r++) {
        store(table+64u,selectors[n]);
        CHECK(kernel_guest_write_bytes(0x4CA9B8u+selectors[n]*2u,&normalized[n],sizeof(normalized[n])));
        for(unsigned i=0u;i<10u;i++)store(0x581990u+i*4u,0xFFFFFFFFu);
        const uint32_t wrapper=0x5836D0u+slots[s]*0x14094u;
        store(0x581990u+slots[s]*4u,identifier);store(wrapper+4u,stream);
        store(wrapper+0x24u,0x12345678u);store(wrapper+0x28u,1u);store(wrapper+0x4Cu,3u);
        store(stack,0x12345678u);store(stack+4u,identifier);store(stack+8u,ratios[r]);
        g_esp=stack;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=0u;g_seh_ebp=0u;
        g_fp_control_word=cw[c];g_fp_top=0u;thunk_trace_reset();
        const uint64_t before=dsound_stream_frequency_set_count();
        recomp_func_t fn=recomp_lookup(0x274A0u);CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();fn();}
        else {CHECK(false);host_run_disarm();return;}
        host_run_disarm();
        CHECK(g_esp==stack+4u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==0u);
        CHECK(g_fp_control_word==cw[c]);CHECK_EQ_U32(dsound_stream_frequency_set_count(),before+1u);
        CHECK_EQ_U32(load(wrapper+0x24u),0u);CHECK_EQ_U32(load(wrapper+0x28u),0u);
        dsound_stream_snapshot snap;CHECK(dsound_stream_get_snapshot(stream,&snap));
        CHECK_EQ_U32(snap.source_rate_hz,expected[n][r]);
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==0x4085CFu&&trace[0].return_address==0x29B35u);
        CHECK(count==1u&&trace[0].implemented&&trace[0].result_known);
    }
    /* Actual sample consumption through the shared PCM seam after the generated
     * caller event. Resampling is INFERRED; this is not original APU execution. */
    dsound_stream_snapshot live;CHECK(dsound_stream_get_snapshot(stream,&live));
    dsound_stream_routing unity;dsound_stream_routing_init(&unity);
    dsound_stream_routing_headroom(&unity,0u);
    CHECK(dsound_audio_runtime_route_stream(stream,live.lease.serial,0u,&unity));
    CHECK(dsound_audio_runtime_set_stream_volume(stream,0));
    CHECK(dsound_audio_runtime_set_stream_running(stream,0u,true));
    int16_t ramp[128];
    for(unsigned i=0u;i<64u;i++)ramp[2u*i]=ramp[2u*i+1u]=(int16_t)(200u*i);
    CHECK(dsound_audio_runtime_submit_pcm16_stereo(stream,0u,44100u,ramp,64u));
    fixture_now=16u;
    const uint32_t wrapper=0x5836D0u+9u*0x14094u;
    store(wrapper+0x28u,1u);store(stack,0x12345678u);
    store(stack+4u,identifier);store(stack+8u,ratios[0]);g_esp=stack;
    recomp_func_t fn=recomp_lookup(0x274A0u);CHECK(fn!=NULL);
    if(sigsetjmp(*host_run_jmp(),1)==0){host_run_arm();fn();}
    else {CHECK(false);host_run_disarm();return;}
    host_run_disarm();
    int16_t output[96];size_t written=0u;
    CHECK(dsound_audio_runtime_render(48u,output,48u,&written));CHECK(written==48u);
    for(unsigned i=0u;i<48u;i++) {
        /* Linear ramp makes half-frame interpolation independently observable:
         * 1.5 source frames/output before16, then0.5 after the caller event. */
        const int16_t sample=(int16_t)(i<16u?300u*i:4800u+100u*(i-16u));
        /* Pre-event Q32 source phase then linear interpolation may truncate
         * less than one PCM unit; the interior post-event FIR ramp is exact. */
        const int error=(int)output[2u*i]-(int)sample;
        CHECK(output[2u*i]==output[2u*i+1u]);
        CHECK(i<16u?(error>=-1&&error<=0):error==0);
    }
    fixture_now=48u; /* Subsequent direct routes must not rewind rendered audio. */
}
static void fatal(uint32_t address,const char *reason)
{host_run_stop(HOST_STOP_UNIMPLEMENTED,address,0u,reason);}
int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);dsound_hle_init();
    map_fixed(0x412000u,4096u);map_fixed(0x4A1000u,4096u);
    for(unsigned i=0u;i<15u;i++)store(0x4A1CF0u+i*4u,0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u,SCRATCH_DATA,0u),0u);
    const uint32_t device=load(SCRATCH_DATA)-8u,desc=SCRATCH_DATA+256u,format=SCRATCH_DATA+320u;
    for(unsigned profile=2u;profile<3u;profile++) {
    const uint32_t d[6]={profile==0u?0x10u:0u,3u,format,0u,0u,0u};
    const uint16_t initial[2]={0x69u,profile==0u?1u:2u},rest[4]={profile==0u?36u:72u,4u,2u,64u};
    const uint32_t rates[2]={44100u,profile==0u?24806u:49612u};uint8_t f[20];
    memcpy(f,initial,4u);memcpy(f+4u,rates,8u);memcpy(f+12u,rest,8u);
    CHECK(kernel_guest_write_bytes(desc,d,sizeof(d)));CHECK(kernel_guest_write_bytes(format,f,sizeof(f)));
    dsound_stream_set_enabled(true);dsound_stream_set_irql_provider(irql);dsound_stream_set_fatal(fatal);
    dsound_completion_reset();dsound_completion_set_clock(fixed_clock,48000u);
    dsound_completion_set_enabled(true);
    CHECK(dsound_audio_runtime_start(48000u,48000u));
    dsound_stream_set_completion(true,dsound_completion_note_pause);
    dsound_stream_set_routing_note(dsound_completion_stream_routing);
    dsound_stream_set_frequency_note(dsound_completion_note_frequency,control_word);
    CHECK_EQ_U32(dsound_stream_create(desc,SCRATCH_DATA+128u),0u);
    const uint32_t stream=load(SCRATCH_DATA+128u);
    uint8_t header[40],parent[44];
    CHECK(kernel_guest_read_bytes(stream,header,sizeof(header)));
    CHECK(kernel_guest_read_bytes(device,parent,sizeof(parent)));


    const uint32_t params=SCRATCH_DATA+512u;
    const uint32_t p[9]={0u,0u,0xFFFFF448u,0u,0u,0u,0u,0u,0u};
    CHECK(kernel_guest_write_bytes(params,p,sizeof(p)));
    if(profile==0u) {
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream,params,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream,0x3F800000u,0u),0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream,0x4B914Cu,4u,0u),0u);
    }
    if(profile==2u) {
        CHECK_EQ_U32(dsound_stream_cache_pause(stream,1u),0u);
        CHECK_EQ_U32(dsound_stream_cache_flush_ex(stream,0u,0u,1u),0u);
        CHECK_EQ_U32(dsound_stream_cache_discontinuity(stream),0u);
    }
    CHECK_EQ_U32(dsound_stream_cache_volume(stream,-10000),0u);
    const xdk_dispatch_entry row={0x4085CFu,"IDirectSoundStream_SetFrequency",XDK_MODULE_DSOUND};
    CHECK(xdk_thunk_init(&row,1u));CHECK(xdk_thunk_declare_abi(row.address,XDK_CC_STDCALL,2u,0u));
    CHECK_EQ_U32(dsound_stream_register(), 14u);
    guest_region_request request={.bytes=8192u,.alignment=4096u,.protect=PAGE_READWRITE,.state=MEM_COMMIT};
    nt_status status;const uint32_t allocation=guest_region_alloc(&request,&status);
    CHECK(allocation!=0u);const uint32_t stack=allocation+4096u-12u;
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_NONE)==0);
    complete_game_caller(stream,allocation+3000u);
    for(unsigned route=0u;route<7u;route++) {
        const bool accepted=route<2u||route==6u;
        const uint32_t frame[3]={route==3u?0x29B30u:0x29B35u,route==5u?0xDEADBEEFu:stream,route==2u?7999u:route==1u?0u:88200u};
        CHECK(kernel_guest_write_bytes(stack,frame,sizeof(frame)));
        const uint64_t accepted_before=dsound_stream_frequency_set_count();
        dsound_stream_snapshot before;CHECK(dsound_stream_get_snapshot(stream,&before));
        g_eax=1u;g_ecx=2u;g_edx=3u;g_ebx=4u;g_esi=5u;g_edi=6u;g_ebp=7u;
        g_esp=stack;g_fs_base=8u;g_fp_control_word=route==4u?0x0C7Fu:route==6u?0x003Fu:route==1u?0x037Fu:0x027Fu;
        const uint16_t saved_cw=g_fp_control_word;thunk_trace_reset();const uint64_t clock=kernel_clock_peek();
        store(0x4124A8u,route==5u?1u:0u);
        recomp_func_t fn=route==1u?recomp_lookup_manual(row.address):recomp_lookup(row.address);
        CHECK(fn!=NULL);
        if(sigsetjmp(*host_run_jmp(),1)==0) {
            host_run_arm();fn();CHECK(accepted||route==5u);
            CHECK(g_eax==(route==5u?0x80004005u:0u)&&g_esp==stack+12u);
        } else {
            CHECK(route>=2u&&route<5u&&host_run_result()->reason==HOST_STOP_UNIMPLEMENTED);
            CHECK(host_run_result()->guest_address==row.address&&g_eax==1u&&g_esp==stack);
        }
        host_run_disarm();store(0x4124A8u,0u);
        CHECK(g_ecx==2u&&g_edx==3u&&g_ebx==4u&&g_esi==5u&&g_edi==6u&&g_ebp==7u&&g_fs_base==8u);
        CHECK_EQ_U32(dsound_stream_frequency_set_count(),accepted_before+(accepted?1u:0u));
        CHECK(kernel_clock_peek()==clock);CHECK(g_fp_control_word==saved_cw);
        uint32_t saved[3];CHECK(kernel_guest_read_bytes(stack,saved,sizeof(saved)));
        CHECK(memcmp(saved,frame,sizeof(frame))==0);
        uint8_t actual[44];CHECK(kernel_guest_read_bytes(stream,actual,40u)&&memcmp(actual,header,40u)==0);
        CHECK(kernel_guest_read_bytes(device,actual,44u)&&memcmp(actual,parent,44u)==0);
        dsound_stream_snapshot after;CHECK(dsound_stream_get_snapshot(stream,&after));
        if(!accepted) {CHECK(memcmp(&after,&before,sizeof(after))==0);}
        else {
            CHECK_EQ_U32(after.source_rate_hz,route==1u?44100u:88200u);
            CHECK_EQ_U32(after.frequency_sets,before.frequency_sets+1u);
            after.source_rate_hz=before.source_rate_hz;
            after.frequency_sets=before.frequency_sets;
            after.frequency_pitch=before.frequency_pitch;
            CHECK(memcmp(&after,&before,sizeof(after))==0);
        }
        size_t count;const thunk_trace_entry *trace=thunk_trace_entries(&count);
        CHECK(count==1u&&trace[0].address==row.address&&trace[0].return_address==frame[0]);
        CHECK(trace[0].implemented&&trace[0].result_known==(accepted||route==5u));
    }
    dsound_stream_set_frequency_note(NULL,NULL);dsound_stream_set_routing_note(NULL);
    dsound_audio_runtime_stop();dsound_completion_reset();
    CHECK(dsound_stream_reset_checked());
    CHECK(mprotect((void *)(uintptr_t)(allocation+4096u),4096u,PROT_READ|PROT_WRITE)==0);
    CHECK(guest_region_free(allocation));xdk_thunk_shutdown();
    }
    CHECK(dsound_device_reset_checked());environment_end();
    printf("T1245 frequency thunk routes: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
