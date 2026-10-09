/* Independent finite routing controls; callback bodies are FABRICATED. */
#define main t1104_author_fixture_main
#include "test_xmv_original.c"
#undef main
#include "host_options.h"

/* This header is copied verbatim from current main's XMV readiness block by the runner. */
#include "t1104_main_policy.inc"

static options parse_disc(bool explicit_route, bool optout)
{
    char *argv[] = {"host", "--disc", "unopened-synthetic.iso", "--native-xmv", "game.xbe", NULL};
    options opts;
    if (optout) argv[3] = "--no-native-xmv";
    if (!explicit_route && !optout) argv[3] = "game.xbe";
    CHECK(parse_options((explicit_route || optout) ? 5 : 4, argv, &opts));
    return opts;
}

static void reset_registers(unsigned index)
{
    g_eax = 0xD1910000u + index; g_ecx = 0x92C00000u + index;
    g_edx = 0x39F00000u + index; g_ebx = 0xDF110000u + index;
    g_esp = 0x00E30000u + index * 32u; g_esi = 0x5A990000u + index;
    g_edi = 0x70B30000u + index; g_ebp = 0x89AA0000u + index;
}

int main(void)
{
    xdk_dispatch_entry entries[7];
    for (unsigned i=0; i<7; ++i) entries[i] = (xdk_dispatch_entry){addresses[i], NULL, XDK_MODULE_NONE};
    CHECK(xdk_thunk_init(entries, 7));
    xdk_thunk_set_stop_on_missing(true);
    char *no_disc[] = {"host", "game.xbe"};
    options opts;
    CHECK(parse_options(2, no_disc, &opts) && !opts.native_xmv && !opts.native_xmv_explicit);
    char *trace_optout[] = {"host", "--disc", "unopened.iso", "--trace-xmv", "--no-native-xmv", "game.xbe"};
    CHECK(!parse_options(6, trace_optout, &opts));
    char *contradiction[] = {"host", "--disc", "unopened.iso", "--native-xmv", "--no-native-xmv", "game.xbe"};
    CHECK(!parse_options(6, contradiction, &opts));
    char *reverse[] = {"host", "--no-native-xmv", "game.xbe", "--disc", "unopened.iso"};
    CHECK(parse_options(5, reverse, &opts) && !opts.native_xmv && opts.native_xmv_off);
    for (unsigned pass=0; pass<2; ++pass) {
        opts=parse_disc(pass!=0, false);
        CHECK(opts.native_xmv && opts.native_xmv_explicit==(pass!=0) && !opts.native_xmv_off);
        CHECK(t1104_policy(&opts)==0 && opts.native_xmv);
        for (unsigned j=0; j<7; ++j) {
            unsigned i=pass ? 6-j : j;
            reset_registers(i);
            route(addresses[i]);
            CHECK(returned);
            CHECK(g_eax==0xBEEF0000u+i && g_ecx==0xAA550000u+i);
            CHECK(g_esp==0x00E30004u+i*32u+pops[i]);
            CHECK(g_edx==0x39F00000u+i && g_ebx==0xDF110000u+i);
            CHECK(g_esi==0x5A990000u+i && g_edi==0x70B30000u+i && g_ebp==0x89AA0000u+i);
        }
    }
    opts=parse_disc(false,true);
    CHECK(t1104_policy(&opts)==0 && !opts.native_xmv);
    unsigned before=atomic_load(&calls);
    for (unsigned i=0; i<7; ++i) {
        reset_registers(i);
        route(addresses[i]);
        CHECK(!returned && stopped.reason==HOST_STOP_XDK_UNROUTED);
        CHECK(g_eax==0xD1910000u+i && g_ecx==0x92C00000u+i && g_esp==0x00E30000u+i*32u);
    }
    CHECK(atomic_load(&calls)==before);
    for (unsigned i=0; i<7; ++i) {
        missing_index=i;
        CHECK(!xmv_original_ready());
        opts=parse_disc(false,false);
        CHECK(t1104_policy(&opts)==0 && !opts.native_xmv);
        opts=parse_disc(true,false);
        CHECK(t1104_policy(&opts)==2);
        CHECK(!xmv_original_configure(true));
    }
    missing_index=99;
    identity="xmv-original-v1-not-authenticated";
    CHECK(!xmv_original_ready());
    opts=parse_disc(false,false);
    CHECK(t1104_policy(&opts)==0 && !opts.native_xmv);
    opts=parse_disc(true,false);
    CHECK(t1104_policy(&opts)==2);
    identity="xmv-original-v1";
    CHECK(xmv_original_configure(true));
    for(unsigned i=0; i<7; ++i) {
        missing_index=i;
        CHECK(!xmv_original_configure(true));
        route(addresses[i]);
        CHECK(returned && g_eax==0xBEEF0000u+i);
    }
    missing_index=99;
    CHECK(!xmv_original_dispatch(0x00445260u));
    CHECK(xmv_original_configure(false));
    xdk_thunk_shutdown();
    printf("T1104 independent routing: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
