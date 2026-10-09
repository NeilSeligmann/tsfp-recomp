/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native control for 0x0012D1E0 (cdecl, 0 args, live call closure), derived from the original
 * listing (188 bytes, two calls to 0x1CDAE0, a tail `jmp 0x3567C0`):
 *   xor eax,eax ; [0x7490AC..0x7490D8 step 4] = eax (12 stores) ; push esi ; [0x7490DC] = eax ;
 *   mov esi,1 ; [0x7490E0] = eax ; [0x7490B0] = esi ; call 1CDAE0 (ret PC 0x12D239) ;
 *   xor ecx,ecx ; cmp eax,2 ; setle cl ; [B8] = [BC] = [D8] = [C0] = [E0] = esi ;
 *   lea ecx,[ecx+ecx+1] ; [B4] = ecx ; call 1CDAE0 (ret PC 0x12D26E) ; xor edx,edx ;
 *   cmp eax,2 ; setle dl ; [C8] = [CC] = [D0] = [D4] = [DC] = esi ; pop esi ; [C4] = edx ; ret.
 * The callee 0x1CDAE0 is `jmp 0x3567C0`:
 *   3567C0: call 3533F0 (mov eax,[76B108] ; ret) ; test eax,eax ; mov ecx,[790950] ; je L ;
 *           mov eax,[774028] ; test eax,eax ; je L ; mov eax,[eax+0x20] ; cmp eax,ecx ; jl R ;
 *        L: cmp ecx,4 ; mov eax,[5236C4] ; jg R ; mov eax,ecx ; R: ret.
 * Two stub modes, both installed through recomp_lookup(0x1CDAE0):
 *   mode 0 (sequence): returns a scripted EAX per call and clobbers ECX and EDX with junk, so the
 *     boundary of the signed `<= 2` test and the register independence are controlled directly.
 *   mode 1 (chain): a faithful transcription of 3567C0 and 3533F0 above, including the return PC
 *     the nested CALL writes below the thunk's frame and its ECX clobber (exit ECX = [790950]).
 * An independent shadow-memory model of the whole listing predicts registers, every guest write
 * (record, pushed ESI slot, both return-PC slots, the nested return-PC slot) and the entry state
 * of both callee invocations. The literal rows come from executing the ORIGINAL bytes of
 * 0x12D1E0, 0x1CDAE0, 0x3567C0 and 0x3533F0 in Unicorn 2.1.4 on the named entry state (stopped
 * at the ret, original only, never the draft). */
#define T1477A_12D1E0_REC_LO 0x007490ACu
#define T1477A_12D1E0_REC_WORDS 14u
#define T1477A_12D1E0_WIN_LO 0x007490A0u
#define T1477A_12D1E0_WIN_HI 0x007490F0u
#define T1477A_12D1E0_RET1 0x0012D239u
#define T1477A_12D1E0_RET2 0x0012D26Eu
#define T1477A_12D1E0_NESTED_RET 0x003567C5u

typedef struct {
    const char *name;
    uint32_t esp, esi, ecx, edx, session, obj, objfield, k, v;
    uint32_t eax_out, ecx_out, edx_out, esi_out;
    uint32_t rec[T1477A_12D1E0_REC_WORDS];
    uint32_t stk[5];
} T1477A12D1E0Row;

typedef struct {
    uint32_t esp, esi, ecx, edx;
    uint32_t session, obj, objfield, k, v;
    int mode;
    uint32_t seq[2];
} T1477A12D1E0State;

typedef struct {
    uint32_t eax, ecx, edx, esi;
    uint32_t ret[2], entry_esp[2], entry_eax[2], entry_ecx[2], entry_edx[2], entry_esi[2];
} T1477A12D1E0Expect;

typedef struct {
    uint32_t address[160];
    uint32_t value[160];
    unsigned count;
} T1477A12D1E0Shadow;

static uint32_t t1477a_12d1e0_rd(const T1477A12D1E0Shadow *shadow, uint32_t address)
{
    for (unsigned i = shadow->count; i-- > 0;)
        if (shadow->address[i] == address) return shadow->value[i];
    return guest_read32(address);
}

static void t1477a_12d1e0_wr(T1477A12D1E0Shadow *shadow, uint32_t address, uint32_t value)
{
    shadow->address[shadow->count] = address;
    shadow->value[shadow->count] = value;
    shadow->count++;
}

/* ---- Callee stubs (real run) ---- */
static int t1477a_12d1e0_stub_mode;
static uint32_t t1477a_12d1e0_stub_seq[2];
static unsigned t1477a_12d1e0_stub_calls;
static uint32_t t1477a_12d1e0_seen_ret[2], t1477a_12d1e0_seen_esp[2], t1477a_12d1e0_seen_eax[2];
static uint32_t t1477a_12d1e0_seen_ecx[2], t1477a_12d1e0_seen_edx[2], t1477a_12d1e0_seen_esi[2];
static uint32_t t1477a_12d1e0_seen_ebx[2], t1477a_12d1e0_seen_edi[2], t1477a_12d1e0_seen_ebp[2];

static void t1477a_12d1e0_stub_1cdae0(void)
{
    const unsigned index = t1477a_12d1e0_stub_calls < 2u ? t1477a_12d1e0_stub_calls : 1u;
    t1477a_12d1e0_stub_calls++;
    t1477a_12d1e0_seen_ret[index] = guest_read32(g_esp);
    t1477a_12d1e0_seen_esp[index] = g_esp;
    t1477a_12d1e0_seen_eax[index] = g_eax;
    t1477a_12d1e0_seen_ecx[index] = g_ecx;
    t1477a_12d1e0_seen_edx[index] = g_edx;
    t1477a_12d1e0_seen_esi[index] = g_esi;
    t1477a_12d1e0_seen_ebx[index] = g_ebx;
    t1477a_12d1e0_seen_edi[index] = g_edi;
    t1477a_12d1e0_seen_ebp[index] = g_ebp;
    if (t1477a_12d1e0_stub_mode == 0) {
        g_eax = t1477a_12d1e0_stub_seq[index];
        g_ecx = 0xC0C0C000u + index;
        g_edx = 0xD0D0D000u + index;
        g_esp += 4u;
        return;
    }
    /* 0x1CDAE0: jmp 0x3567C0 ; 0x3567C0 as listed, 0x3533F0 inlined with its CALL slot */
    guest_write32(g_esp - 4u, T1477A_12D1E0_NESTED_RET);   /* call 3533F0 */
    g_eax = guest_read32(0x0076B108u);                     /* 3533F0: mov eax,[76B108] */
    g_ecx = guest_read32(0x00790950u);
    if (g_eax != 0u) {
        g_eax = guest_read32(0x00774028u);
        if (g_eax != 0u) {
            g_eax = guest_read32(g_eax + 0x20u);
            if ((int32_t)g_eax < (int32_t)g_ecx) { g_esp += 4u; return; }
        }
    }
    g_eax = guest_read32(0x005236C4u);
    if (!((int32_t)g_ecx > 4)) g_eax = g_ecx;
    g_esp += 4u;
}

/* Standalone: the control owns recomp_lookup. Embedded in test_game_replacements.c: that file's
 * recomp_lookup consults t1477a_lookup while t1477a_active is set by the test below. */
#ifndef T1477_APPENDIX_EMBEDDED
game_guest_function recomp_lookup(uint32_t xbox_va)
{
    if (xbox_va == 0x001CDAE0u) return t1477a_12d1e0_stub_1cdae0;
    return NULL;
}
#else
static game_guest_function t1477a_lookup(uint32_t xbox_va)
{
    return xbox_va == 0x001CDAE0u ? t1477a_12d1e0_stub_1cdae0 : NULL;
}
#endif

/* ---- Model of the whole listing over the shadow ---- */
static void t1477a_12d1e0_model_callee(const T1477A12D1E0State *state, T1477A12D1E0Shadow *shadow,
                                       unsigned index, uint32_t *eax, uint32_t *ecx, uint32_t *edx,
                                       uint32_t esp)
{
    if (state->mode == 0) {
        *eax = state->seq[index];
        *ecx = 0xC0C0C000u + index;
        *edx = 0xD0D0D000u + index;
        return;
    }
    t1477a_12d1e0_wr(shadow, esp - 4u, T1477A_12D1E0_NESTED_RET);
    uint32_t value = t1477a_12d1e0_rd(shadow, 0x0076B108u);
    uint32_t count = t1477a_12d1e0_rd(shadow, 0x00790950u);
    int taken_return = 0;
    if (value != 0u) {
        const uint32_t object = t1477a_12d1e0_rd(shadow, 0x00774028u);
        if (object != 0u) {
            value = t1477a_12d1e0_rd(shadow, object + 0x20u);
            if ((int32_t)value < (int32_t)count) taken_return = 1;
        }
    }
    if (!taken_return) {
        value = t1477a_12d1e0_rd(shadow, 0x005236C4u);
        if (!((int32_t)count > 4)) value = count;
    }
    *eax = value;
    *ecx = count;
}

static T1477A12D1E0Expect t1477a_12d1e0_model(const T1477A12D1E0State *state,
                                              T1477A12D1E0Shadow *shadow)
{
    T1477A12D1E0Expect x;
    uint32_t eax = 0u, ecx = state->ecx, edx = state->edx, esi = state->esi, esp = state->esp;
    uint32_t cl, dl;
    memset(&x, 0, sizeof x);
    for (uint32_t address = 0x007490ACu; address <= 0x007490D8u; address += 4u)
        t1477a_12d1e0_wr(shadow, address, eax);
    esp -= 4u;
    t1477a_12d1e0_wr(shadow, esp, esi);
    t1477a_12d1e0_wr(shadow, 0x007490DCu, eax);
    esi = 1u;
    t1477a_12d1e0_wr(shadow, 0x007490E0u, eax);
    t1477a_12d1e0_wr(shadow, 0x007490B0u, esi);
    esp -= 4u;                                       /* call 1 */
    t1477a_12d1e0_wr(shadow, esp, T1477A_12D1E0_RET1);
    x.ret[0] = T1477A_12D1E0_RET1; x.entry_esp[0] = esp;
    x.entry_eax[0] = eax; x.entry_ecx[0] = ecx; x.entry_edx[0] = edx; x.entry_esi[0] = esi;
    t1477a_12d1e0_model_callee(state, shadow, 0u, &eax, &ecx, &edx, esp);
    esp += 4u;
    cl = (int32_t)eax <= 2 ? 1u : 0u;
    ecx = cl;
    t1477a_12d1e0_wr(shadow, 0x007490B8u, esi);
    t1477a_12d1e0_wr(shadow, 0x007490BCu, esi);
    t1477a_12d1e0_wr(shadow, 0x007490D8u, esi);
    t1477a_12d1e0_wr(shadow, 0x007490C0u, esi);
    t1477a_12d1e0_wr(shadow, 0x007490E0u, esi);
    ecx = 2u * ecx + 1u;
    t1477a_12d1e0_wr(shadow, 0x007490B4u, ecx);
    esp -= 4u;                                       /* call 2 */
    t1477a_12d1e0_wr(shadow, esp, T1477A_12D1E0_RET2);
    x.ret[1] = T1477A_12D1E0_RET2; x.entry_esp[1] = esp;
    x.entry_eax[1] = eax; x.entry_ecx[1] = ecx; x.entry_edx[1] = edx; x.entry_esi[1] = esi;
    t1477a_12d1e0_model_callee(state, shadow, 1u, &eax, &ecx, &edx, esp);
    esp += 4u;
    dl = (int32_t)eax <= 2 ? 1u : 0u;
    edx = dl;
    t1477a_12d1e0_wr(shadow, 0x007490C8u, esi);
    t1477a_12d1e0_wr(shadow, 0x007490CCu, esi);
    t1477a_12d1e0_wr(shadow, 0x007490D0u, esi);
    t1477a_12d1e0_wr(shadow, 0x007490D4u, esi);
    t1477a_12d1e0_wr(shadow, 0x007490DCu, esi);
    esi = t1477a_12d1e0_rd(shadow, esp);             /* pop esi */
    esp += 4u;
    t1477a_12d1e0_wr(shadow, 0x007490C4u, edx);
    x.eax = eax; x.ecx = ecx; x.edx = edx; x.esi = esi;
    return x;
}

static uint32_t t1477a_12d1e0_canary(uint32_t address)
{
    return 0xC0DE0000u ^ (address * 2654435761u);
}

/* Builds the state, predicts, runs, compares registers, callee entry observations and memory. */
static T1477A12D1E0Expect t1477a_12d1e0_run(const T1477A12D1E0State *state)
{
    uint32_t window[(T1477A_12D1E0_WIN_HI - T1477A_12D1E0_WIN_LO) / 4u + 10u];
    uint32_t window_address[sizeof window / sizeof window[0]];
    const unsigned record_words = (T1477A_12D1E0_WIN_HI - T1477A_12D1E0_WIN_LO) / 4u;
    prepare();
    for (uint32_t a = T1477A_12D1E0_WIN_LO; a < T1477A_12D1E0_WIN_HI; a += 4u)
        guest_write32(a, t1477a_12d1e0_canary(a));
    for (uint32_t a = state->esp - 0x20u; a < state->esp + 8u; a += 4u)
        guest_write32(a, t1477a_12d1e0_canary(a));
    g_esp = state->esp;
    guest_write32(g_esp, 0x00010000u);
    guest_write32(0x0076B108u, state->session);
    guest_write32(0x00774028u, state->obj);
    guest_write32(0x00790950u, state->k);
    guest_write32(0x005236C4u, state->v);
    if (state->obj != 0u && !(state->obj + 0x20u >= T1477A_12D1E0_WIN_LO &&
                              state->obj + 0x20u < T1477A_12D1E0_WIN_HI))
        guest_write32(state->obj + 0x20u, state->objfield);
    g_eax = 0x11111111u; g_ecx = state->ecx; g_edx = state->edx; g_esi = state->esi;
    g_ebx = 0xB1B1B1B1u; g_edi = 0xD1D1D1D1u; g_ebp = 0xBBBB0001u;
    t1477a_12d1e0_stub_mode = state->mode;
    t1477a_12d1e0_stub_seq[0] = state->seq[0];
    t1477a_12d1e0_stub_seq[1] = state->seq[1];
    t1477a_12d1e0_stub_calls = 0u;
    for (unsigned i = 0; i < record_words; ++i) {
        window_address[i] = T1477A_12D1E0_WIN_LO + 4u * i;
        window[i] = guest_read32(window_address[i]);
    }
    for (unsigned i = 0; i < 10u; ++i) {
        window_address[record_words + i] = state->esp - 0x20u + 4u * i;
        window[record_words + i] = guest_read32(window_address[record_words + i]);
    }
    T1477A12D1E0Shadow shadow;
    shadow.count = 0u;
    const T1477A12D1E0Expect x = t1477a_12d1e0_model(state, &shadow);

    invoke(0x0012D1E0u, 4u);
    CHECK(g_eax == x.eax);
    CHECK(g_ecx == x.ecx);
    CHECK(g_edx == x.edx);
    CHECK(g_esi == x.esi);
    CHECK(g_ebx == 0xB1B1B1B1u && g_edi == 0xD1D1D1D1u && g_ebp == 0xBBBB0001u);
    CHECK(t1477a_12d1e0_stub_calls == 2u);
    for (unsigned i = 0; i < 2u; ++i) {
        CHECK(t1477a_12d1e0_seen_ret[i] == x.ret[i]);
        CHECK(t1477a_12d1e0_seen_esp[i] == x.entry_esp[i]);
        CHECK(t1477a_12d1e0_seen_eax[i] == x.entry_eax[i]);
        CHECK(t1477a_12d1e0_seen_ecx[i] == x.entry_ecx[i]);
        CHECK(t1477a_12d1e0_seen_edx[i] == x.entry_edx[i]);
        CHECK(t1477a_12d1e0_seen_esi[i] == x.entry_esi[i]);
        CHECK(t1477a_12d1e0_seen_ebx[i] == 0xB1B1B1B1u && t1477a_12d1e0_seen_edi[i] == 0xD1D1D1D1u);
        CHECK(t1477a_12d1e0_seen_ebp[i] == 0xBBBB0001u);
    }
    for (unsigned i = 0; i < sizeof window / sizeof window[0]; ++i) {
        uint32_t want = window[i];
        for (unsigned k = 0; k < shadow.count; ++k)
            if (shadow.address[k] == window_address[i]) want = shadow.value[k];
        CHECK(guest_read32(window_address[i]) == want);
    }
    CHECK(guest_read32(0x0076B108u) == state->session && guest_read32(0x00790950u) == state->k);
    CHECK(guest_read32(0x005236C4u) == state->v);
    return x;
}

static void t1477a_12d1e0_check_row(const T1477A12D1E0Row *row)
{
    T1477A12D1E0State state = {row->esp, row->esi, row->ecx, row->edx, row->session, row->obj,
                               row->objfield, row->k, row->v, 1, {0u, 0u}};
    t1477a_12d1e0_run(&state);
    CHECK(g_eax == row->eax_out && g_ecx == row->ecx_out && g_edx == row->edx_out);
    CHECK(g_esi == row->esi_out);
    for (unsigned i = 0; i < T1477A_12D1E0_REC_WORDS; ++i)
        CHECK(guest_read32(T1477A_12D1E0_REC_LO + 4u * i) == row->rec[i]);
    for (unsigned i = 0; i < 5u; ++i)
        CHECK(guest_read32(row->esp - 16u + 4u * i) == row->stk[i]);
}

/* Rows from the ORIGINAL bytes in Unicorn (mode 1 chain), see the header comment. rec[] is
 * 0x7490AC..0x7490E0, stk[] is the five dwords at esp-16 .. esp (esp = entry ESP, the root's
 * own return slot), all read at the ret. */
static const T1477A12D1E0Row t1477a_12d1e0_rows[] = {
    {"k3_nosession", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"k2_tie", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x2u, 0x7u,
     0x2u, 0x2u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"k5_v2", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x5u, 0x2u,
     0x2u, 0x5u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"k5_v3", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x5u, 0x3u,
     0x3u, 0x5u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"k_neg", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x80000000u, 0x9u,
     0x80000000u, 0x80000000u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"k_big_vneg", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x7FFFFFFFu, 0x80000000u,
     0x80000000u, 0x7FFFFFFFu, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"k4_edge", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x4u, 0x9u,
     0x4u, 0x4u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"session_noobj", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x0u, 0x0u, 0x5u, 0x3u,
     0x3u, 0x5u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"session_field_lt", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x7u, 0x30000u, 0x1u, 0x2u, 0x9u,
     0x1u, 0x2u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"session_field_ge", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x7u, 0x30000u, 0x5u, 0x2u, 0x9u,
     0x2u, 0x2u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"session_field_negone", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x30000u, 0xFFFFFFFFu, 0x1u, 0x9u,
     0xFFFFFFFFu, 0x1u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"session_field_3", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x30000u, 0x3u, 0x4u, 0x9u,
     0x3u, 0x4u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"obj_alias_b4", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x749094u, 0x0u, 0x5u, 0x9u,
     0x3u, 0x5u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"obj_alias_b8", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x749098u, 0x0u, 0x5u, 0x9u,
     0x1u, 0x5u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"obj_alias_b0", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x749090u, 0x0u, 0x2u, 0x9u,
     0x1u, 0x2u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"obj_alias_c4", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x7490A4u, 0x0u, 0x5u, 0x9u,
     0x0u, 0x5u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"obj_alias_e0", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x1u, 0x7490C0u, 0x0u, 0x5u, 0x9u,
     0x1u, 0x5u, 0x1u, 0x51515151u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"sess_zero_flag_obj", 0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x30000u, 0x1u, 0x5u, 0x9u,
     0x9u, 0x5u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x10000u}},
    {"stk_saved_ac", 0x7490B0u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x51515151u,
     {0x51515151u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0xFB8D9EA0u, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x1u}},
    {"stk_saved_dc", 0x7490E0u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x1u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x12D26Eu, 0x1u, 0x1u},
     {0x1u, 0x1u, 0x12D26Eu, 0x1u, 0x1u}},
    {"stk_saved_e0", 0x7490E4u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x1u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x3567C5u, 0x1u, 0x1u},
     {0x1u, 0x3567C5u, 0x1u, 0x1u, 0x10000u}},
    {"stk_saved_c4", 0x7490C8u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x3567C5u, 0x12D26Eu, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x1u, 0x3567C5u, 0x12D26Eu, 0x0u, 0x1u}},
    {"stk_saved_b0", 0x7490B4u, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x2u, 0x7u,
     0x2u, 0x2u, 0x1u, 0x1u,
     {0x12D26Eu, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x74EF8564u, 0x3567C5u, 0x12D26Eu, 0x1u, 0x3u}},
    {"stk_saved_d8", 0x7490DCu, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x2u, 0x7u,
     0x2u, 0x2u, 0x1u, 0x1u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x1u, 0x1u, 0x1u, 0x1u, 0x1u}},
    {"stk_ret_ac", 0x7490ACu, 0x51515151u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x51515151u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x2ABB7DCu, 0x3567C5u, 0x12D26Eu, 0x51515151u, 0x0u}},
    {"stk_ret_e0", 0x7490E0u, 0x61616161u, 0x22222222u, 0x33333333u, 0x1u, 0x749094u, 0x0u, 0x5u, 0x9u,
     0x3u, 0x5u, 0x0u, 0x1u,
     {0x0u, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x12D26Eu, 0x1u, 0x1u},
     {0x1u, 0x1u, 0x12D26Eu, 0x1u, 0x1u}},
    {"stk_ret_b4", 0x7490B4u, 0x61616161u, 0x22222222u, 0x33333333u, 0x1u, 0x749094u, 0x0u, 0x5u, 0x9u,
     0x3u, 0x5u, 0x0u, 0x1u,
     {0x12D26Eu, 0x1u, 0x3u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x74EF8564u, 0x3567C5u, 0x12D26Eu, 0x1u, 0x3u}},
    {"stk_below_rec", 0x7490A8u, 0x61616161u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x61616161u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x8949D118u, 0x3567C5u, 0x12D26Eu, 0x61616161u, 0x10000u}},
    {"stk_above_rec", 0x7490F4u, 0x61616161u, 0x22222222u, 0x33333333u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x61616161u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x82D1F1A4u, 0x3567C5u, 0x12D26Eu, 0x61616161u, 0x10000u}},
    {"regs_other", 0x100u, 0x0u, 0xFFFFFFFFu, 0x80000000u, 0x0u, 0x0u, 0x0u, 0x3u, 0x7u,
     0x3u, 0x3u, 0x0u, 0x0u,
     {0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x0u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u, 0x1u},
     {0x94DC15F0u, 0x3567C5u, 0x12D26Eu, 0x0u, 0x10000u}},
};

static void test_t1477_appendix_0012d1e0(void)
{
#ifdef T1477_APPENDIX_EMBEDDED
    t1477a_active = 1;
#endif
    /* ---- Original-derived literal rows ---- */
    for (unsigned i = 0; i < sizeof t1477a_12d1e0_rows / sizeof t1477a_12d1e0_rows[0]; ++i)
        t1477a_12d1e0_check_row(&t1477a_12d1e0_rows[i]);

    /* Hand-derived literals: mode 0 scripted counts around the signed `<= 2` boundary.
     * n1 = 2 -> ECX seed 1 -> [B4] = 3 ; n1 = 3 -> [B4] = 1 ; n2 = 2 -> EDX 1, [C4] = 1. */
    {
        T1477A12D1E0State s = {0x100u, 0x51515151u, 0x22222222u, 0x33333333u, 0u, 0u, 0u, 0u, 0u,
                               0, {2u, 2u}};
        t1477a_12d1e0_run(&s);
        CHECK(g_eax == 2u && g_edx == 1u && g_ecx == 0xC0C0C001u && g_esi == 0x51515151u);
        CHECK(guest_read32(0x7490B4u) == 3u && guest_read32(0x7490C4u) == 1u);
        s.seq[0] = 3u; s.seq[1] = 3u;
        t1477a_12d1e0_run(&s);
        CHECK(g_eax == 3u && g_edx == 0u);
        CHECK(guest_read32(0x7490B4u) == 1u && guest_read32(0x7490C4u) == 0u);
        s.seq[0] = 0x80000000u; s.seq[1] = 0xFFFFFFFFu;     /* negative counts are <= 2 */
        t1477a_12d1e0_run(&s);
        CHECK(g_eax == 0xFFFFFFFFu && g_edx == 1u);
        CHECK(guest_read32(0x7490B4u) == 3u && guest_read32(0x7490C4u) == 1u);
        s.seq[0] = 0x7FFFFFFFu; s.seq[1] = 0u;
        t1477a_12d1e0_run(&s);
        CHECK(g_eax == 0u && g_edx == 1u);
        CHECK(guest_read32(0x7490B4u) == 1u && guest_read32(0x7490C4u) == 1u);
    }

    /* ---- Sequence stub: counts around the boundary, several stacks (alias or not) ---- */
    {
        const uint32_t counts[] = {0x80000000u, 0xFFFFFFFFu, 0u, 1u, 2u, 3u, 4u, 0x7FFFFFFFu};
        const uint32_t stacks[] = {0x100u, 0x7490E4u, 0x7490E0u, 0x7490DCu, 0x7490B4u, 0x7490B0u,
                                   0x7490C8u, 0x7490ACu, 0x7490F4u};
        const uint32_t esis[] = {0x51515151u, 0u, 1u, 0xFFFFFFFFu};
        for (unsigned a = 0; a < sizeof stacks / sizeof stacks[0]; ++a)
            for (unsigned b = 0; b < sizeof counts / sizeof counts[0]; ++b)
                for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; ++c)
                    for (unsigned d = 0; d < sizeof esis / sizeof esis[0]; ++d) {
                        T1477A12D1E0State s = {stacks[a], esis[d], 0x22222222u, 0x33333333u,
                                               0u, 0u, 0u, 0u, 0u, 0, {counts[b], counts[c]}};
                        t1477a_12d1e0_run(&s);
                    }
    }

    /* ---- Chain stub: every stack position over the record, count and session variants ---- */
    {
        const uint32_t ks[] = {0x80000000u, 0u, 2u, 3u, 4u, 5u, 0x7FFFFFFFu};
        const uint32_t vs[] = {0x80000000u, 1u, 2u, 3u, 0x7FFFFFFFu};
        const uint32_t sessions[] = {0u, 1u, 0x80000000u};
        const uint32_t objects[] = {0u, 0x30000u, 0x749094u, 0x749098u, 0x749090u, 0x7490A4u,
                                    0x7490C0u};
        const uint32_t fields[] = {0u, 1u, 5u, 0xFFFFFFFFu, 0x80000000u};
        for (uint32_t esp = 0x7490A0u; esp <= 0x7490F8u; esp += 4u)
            for (unsigned k = 0; k < sizeof ks / sizeof ks[0]; ++k)
                for (unsigned v = 0; v < sizeof vs / sizeof vs[0]; ++v)
                    for (unsigned s = 0; s < sizeof sessions / sizeof sessions[0]; ++s) {
                        T1477A12D1E0State state = {esp, 0x51515151u, 0x22222222u, 0x33333333u,
                                                   sessions[s], 0u, 0u, ks[k], vs[v], 1,
                                                   {0u, 0u}};
                        t1477a_12d1e0_run(&state);
                    }
        for (unsigned o = 0; o < sizeof objects / sizeof objects[0]; ++o)
            for (unsigned f = 0; f < sizeof fields / sizeof fields[0]; ++f)
                for (unsigned k = 0; k < sizeof ks / sizeof ks[0]; ++k)
                    for (unsigned s = 1; s < sizeof sessions / sizeof sessions[0]; ++s) {
                        T1477A12D1E0State state = {0x100u, 0x51515151u, 0x22222222u, 0x33333333u,
                                                   sessions[s], objects[o], fields[f], ks[k], 9u,
                                                   1, {0u, 0u}};
                        t1477a_12d1e0_run(&state);
                    }
    }
#ifdef T1477_APPENDIX_EMBEDDED
    t1477a_active = 0;
#endif
}
