/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Registry-shaped, REGISTER-EXACT replacements of the four authenticated x87 roots (T1510),
 * converted from the hand drafts in docs/evidence/t1510/hand_roots.inc. Included by x87_roots.c.
 * Proof status and limits: docs/replace-x87-t1510.md. All four are cdecl; the original `ret`
 * pops only the return slot.
 *
 * Why register-exact (GAME_REPLACE_EXACT, no scratch narrowing): the first v2 proof attempt
 * (caller audit, docs/evidence/t1510/v2/audit-refusal-cb3b0.json) found the hand drafts' declared scratch
 * eax/ecx/edx WRONG. The originals leave data-dependent values in all three that real callers
 * read (CB3B0 leaves eax = object, ecx = [object+0x254]). Each body below therefore reproduces
 * the original's eax, ecx and edx on every exit path by performing the same loads in the same
 * order. ebx, esi, edi, ebp are saved and restored by the originals and untouched here.
 * Only the dead stack (the original pushes below the entry esp) and EFLAGS are not reproduced.
 */
#define X87_OBJECT_SCALE_FLAG 0x254u
#define X87_OBJECT_SCALE_MODE 0x258u
#define X87_SCALE_ONE_GLOBAL 0x478900u
#define X87_SCALE_DEFAULT_GLOBAL 0x4785CCu
#define X87_TABLE_BASE_GLOBAL 0x4F9BACu
#define X87_TABLE_ENTRY_SIZE 0x29Cu
#define X87_OUT_X 0x38Cu
#define X87_OUT_Y 0x390u
#define X87_OUT_Z 0x394u
#define X87_PLAYER_GLOBAL 0x7B0C7Cu
#define X87_CAMERA_GLOBAL 0x7B0CC0u

/* 000CB3B0: push the zoom scale float: constant 478900 when the object has no override
 * (+0x254 == 0) and mode (+0x258) is 1, else 4785CC. Leaves eax = object, ecx = [object+0x254]
 * and the float on the x87 stack for the caller. */
static void game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(uint32_t object)
{
    g_eax = object;
    g_ecx = guest_read32(object + X87_OBJECT_SCALE_FLAG);
    int plain = g_ecx == 0u && guest_read32(object + X87_OBJECT_SCALE_MODE) == 1u;
    x87_fld32(plain ? X87_SCALE_ONE_GLOBAL : X87_SCALE_DEFAULT_GLOBAL);
}

GAME_REPLACE_EXACT(000CB3B0, cdecl, 1, void,
                   game_camera_default_zoom_scale_constant_by_fields_0x254_0x258)
{
    x87_frame frame = x87_frame_open();
    game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(game_stack_arg(0u));
    x87_checked_return(frame, 1);
}

/* 001B7D00: scale the three factors (+0x5C +0x60 +0x64 of the 0x29C-byte table entry) by the
 * record's object zoom scale into the object's +0x38C +0x390 +0x394. Every [record+8] is
 * re-read where the original re-reads it, so an aliasing store is observed identically. */
static void game_object_scale_floats_0x38c_0x390_0x394_by_record_29c_0x5c_0x60_0x64(
    uint32_t record, uint32_t index)
{
    g_eax = guest_read32(record + 8u);
    if (g_eax == 0u) {
        return;
    }
    uint32_t entry = guest_read32(X87_TABLE_BASE_GLOBAL) + index * X87_TABLE_ENTRY_SIZE;
    game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(g_eax);
    g_eax = guest_read32(record + 8u);
    x87_fmul32(entry + 0x5Cu);
    x87_fstp32(g_eax + X87_OUT_X);
    g_ecx = guest_read32(record + 8u);
    game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(g_ecx);
    x87_fmul32(entry + 0x60u);
    g_edx = guest_read32(record + 8u);
    x87_fstp32(g_edx + X87_OUT_Y);
    g_eax = guest_read32(record + 8u);
    game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(g_eax);
    x87_fmul32(entry + 0x64u);
    g_ecx = guest_read32(record + 8u);
    x87_fstp32(g_ecx + X87_OUT_Z);
}

GAME_REPLACE_EXACT(001B7D00, cdecl, 2, void,
                   game_object_scale_floats_0x38c_0x390_0x394_by_record_29c_0x5c_0x60_0x64)
{
    x87_frame frame = x87_frame_open();
    game_object_scale_floats_0x38c_0x390_0x394_by_record_29c_0x5c_0x60_0x64(
        game_stack_arg(0u), game_stack_arg(1u));
    x87_checked_return(frame, 0);
}

/* 001B7D70: scale through 001B7D00, or (flag 0x40 of the table entry set and other+0x38 != 1)
 * store one scaled value into Y and copy it to Z then X. */
static void game_object_scale_child_floats_0x390_0x394_0x38c_by_record_29c_or_set_equal(
    uint32_t record, uint32_t other)
{
    g_ecx = guest_read32(record + 8u);
    if (g_ecx == 0u) {
        return;
    }
    g_edx = other;
    g_eax = guest_read32(other + 0x30u);
    if (g_eax == 0xFFFFFFFFu) {
        return;
    }
    uint32_t index = g_eax;
    uint8_t entry_flags = guest_read8(guest_read32(X87_TABLE_BASE_GLOBAL) +
                                      index * X87_TABLE_ENTRY_SIZE + 0xCu);
    if ((entry_flags & 0x40u) == 0u || guest_read32(other + 0x38u) == 1u) {
        game_object_scale_floats_0x38c_0x390_0x394_by_record_29c_0x5c_0x60_0x64(record,
                                                                               index);
        return;
    }
    game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(g_ecx);
    g_eax = guest_read32(record + 8u);
    x87_fstp32(g_eax + X87_OUT_Y);
    g_eax = guest_read32(record + 8u);
    g_ecx = guest_read32(g_eax + X87_OUT_Y);
    guest_write32(g_eax + X87_OUT_Z, g_ecx);
    uint32_t target = guest_read32(record + 8u);
    g_edx = guest_read32(target + X87_OUT_Z);
    guest_write32(target + X87_OUT_X, g_edx);
}

GAME_REPLACE_EXACT(001B7D70, cdecl, 2, void,
                   game_object_scale_child_floats_0x390_0x394_0x38c_by_record_29c_or_set_equal)
{
    x87_frame frame = x87_frame_open();
    game_object_scale_child_floats_0x390_0x394_0x38c_by_record_29c_or_set_equal(
        game_stack_arg(0u), game_stack_arg(1u));
    x87_checked_return(frame, 0);
}

/* 00259DF0: rotate camera float +0x388 into +0x38C and +0x384, refresh +0x388 from the zoom
 * scale, then clear bits 0x20 and 0x40 of the player flags (+0xC). */
static void game_player_7b0c7c_clear_flags_0x20_0x40_and_rotate_7b0cc0_field_388_history(void)
{
    g_eax = guest_read32(X87_CAMERA_GLOBAL);
    if (g_eax != 0u) {
        g_ecx = guest_read32(g_eax + 0x388u);
        guest_write32(g_eax + 0x38Cu, g_ecx);
        g_eax = guest_read32(X87_CAMERA_GLOBAL);
        g_edx = guest_read32(g_eax + 0x388u);
        guest_write32(g_eax + 0x384u, g_edx);
        g_eax = guest_read32(X87_CAMERA_GLOBAL);
        game_camera_default_zoom_scale_constant_by_fields_0x254_0x258(g_eax);
        g_ecx = guest_read32(X87_CAMERA_GLOBAL);
        x87_fstp32(g_ecx + 0x388u);
    }
    g_eax = guest_read32(X87_PLAYER_GLOBAL);
    guest_write32(g_eax + 0xCu, guest_read32(g_eax + 0xCu) & 0xFFFFFFDFu);
    g_eax = guest_read32(X87_PLAYER_GLOBAL);
    guest_write32(g_eax + 0xCu, guest_read32(g_eax + 0xCu) & 0xFFFFFFBFu);
}

GAME_REPLACE_EXACT(00259DF0, cdecl, 0, void,
                   game_player_7b0c7c_clear_flags_0x20_0x40_and_rotate_7b0cc0_field_388_history)
{
    x87_frame frame = x87_frame_open();
    game_player_7b0c7c_clear_flags_0x20_0x40_and_rotate_7b0cc0_field_388_history();
    x87_checked_return(frame, 0);
}

/* 003C9DE8: the CRT fmod core, `fxch st1; L: fprem; fnstsw ax; sahf; jp L; fstp st1; ret`. ST(0)
 * is the divisor and ST(1) the dividend on entry; on return the remainder replaces the dividend
 * and the divisor is popped (net TOP + 1). Each loop pass stores the FPREM status word in AX (the
 * low 16 bits of eax, the high half is kept), so eax leaves holding the last iteration's status.
 * Reached only through the function table at 0x54D820, so the caller audit has no direct site to
 * inspect. EFLAGS (the original leaves sahf's flags) are not reproduced. */
static void game_crt_x87_fprem_loop_until_complete_and_pop_divisor(void)
{
    uint32_t status;
    x87_fxch1();
    do {
        status = x87_fprem_step();
        g_eax = (g_eax & 0xFFFF0000u) | status;
    } while ((status & 0x400u) != 0u);
    x87_fstp1();
}

GAME_REPLACE_EXACT(003C9DE8, cdecl, 0, void,
                   game_crt_x87_fprem_loop_until_complete_and_pop_divisor)
{
    x87_frame frame = x87_frame_open();
    game_crt_x87_fprem_loop_until_complete_and_pop_divisor();
    x87_checked_return(frame, -1);
}
