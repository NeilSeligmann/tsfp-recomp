/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller_config.h"
#include "xinput_source.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CONTROLLER_TEST_RENAME_FAILURE
static bool fail_rename;
int __real_rename(const char *old_path, const char *new_path);
int __wrap_rename(const char *old_path, const char *new_path)
{
    if (fail_rename) { errno = EIO; return -1; }
    return __real_rename(old_path, new_path);
}
#endif

static void refuses(const char *text)
{
    controller_config config, before;
    controller_config_defaults(&config);
    before = config;
    char error[256] = {0};
    assert(!controller_config_parse(text, strlen(text), &config, error, sizeof(error)));
    assert(error[0] != '\0');
    assert(memcmp(&config, &before, sizeof(config)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    controller_config config, loaded;
    char error[256];
    controller_config_defaults(&config);
    assert(controller_config_validate(&config, error, sizeof(error)));
    for (unsigned p = 0; p < 4; p++) {
        assert(config.selectors[p].enabled && config.selectors[p].occurrence == -1);
        assert(config.selectors[p].guid[0] == '\0');
    }
    bool buttons[15] = {0};
    buttons[0] = buttons[6] = buttons[8] = buttons[10] = buttons[14] = true;
    int16_t axes[6] = {-32768, -32768, 32767, 32767, 1, 32767};
    xinput_pad_state pad;
    controller_profile_apply(&config.profiles[0], buttons, axes, &pad);
    assert(pad.analog[0] == 255 && pad.analog[7] == 255 && pad.analog[6] == 0);
    assert(pad.digital_buttons == (XINPUT_BUTTON_START | XINPUT_BUTTON_LEFT_THUMB | XINPUT_BUTTON_DPAD_UP));
    assert(pad.thumb_left_x == -32768 && pad.thumb_left_y == 32767);
    assert(pad.thumb_right_x == 32767 && pad.thumb_right_y == -32767);

    controller_profile *profile = &config.profiles[0];
    profile->button_map[0] = 1; profile->button_map[1] = 0;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.analog[0] == 0 && pad.analog[1] == 255);
    profile->calibration[0].deadzone = 2000;
    axes[0] = 1000;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.thumb_left_x == 0);
    axes[0] = -32768;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.thumb_left_x == -32768);
    axes[0] = 32767;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.thumb_left_x == 32767);
    profile->axis_map[0] = 2;
    profile->calibration[2] = (controller_axis_calibration){-100, 0, 100, 0, false};
    axes[2] = 50;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.thumb_left_x == 16383);
    axes[2] = -200;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.thumb_left_x == -32768);
    profile->calibration[4] = (controller_axis_calibration){100, 100, 1000, 0, true};
    axes[4] = 100;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.analog[6] == 255);
    axes[4] = 1000;
    controller_profile_apply(profile, buttons, axes, &pad);
    assert(pad.analog[6] == 0);
    profile->axis_map[0] = 900;
    memset(&pad, 0xff, sizeof(pad));
    controller_profile_apply(profile, buttons, axes, &pad);
    xinput_pad_state zero = {0};
    assert(memcmp(&pad, &zero, sizeof(pad)) == 0);

    const char *bad[] = {
        "", "version=2", "version=1\nversion=1", "version=1\nmapping_file=unquoted",
        "version=1\nport.4.guid=auto", "version=1\nport.0.guid=bogus", "version=1\nport.0.occurrence=1",
        "version=1\nport.0.axes=0,1,2,3,4,6", "version=1\nport.0.axes=4,1,2,3,4,5",
        "version=1\nport.0.axis.0=0,0,100,0,0", "version=1\nport.0.axis.4=0,1,100,0,0",
        "version=1\nport.0.axis.0=-32768,0,32767,32767,0", "version=1\nport.0.axis.0=-32768,0,32767,-1,0",
        "version=1\nport.0.axis.0=-32768,0,32767,0,2", "version=1\nport.0.axis.0=-32768,0,32767,0,true",
        "version=1\nport.0.axis.0=-32768,0,32767,0,0,1", "version=1\nport.0.axes=0,1,2,3,4",
        "version=1\nport.0.buttons=-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0",
        "version=1\nport.0.guid=00000000000000000000000000000001\nport.1.guid=00000000000000000000000000000001",
        "version=1\nport.0.guid=00000000000000000000000000000001\nport.0.occurrence=0\nport.1.guid=00000000000000000000000000000001\nport.1.occurrence=0",
        "version=1\nport.0.occurrence=9999999999999999999999", "version=1\nunknown=1",
        "version=1\nmapping_file=\"bad\\npath\"", "version=1\nport.0.guid=disabled\nport.0.occurrence=0"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) refuses(bad[i]);
    char nul[] = "version=1\0unknown=1";
    assert(!controller_config_parse(nul, sizeof(nul)-1, &config, error, sizeof(error)));
    char *huge = malloc(65537);
    assert(huge != NULL); memset(huge, ' ', 65537);
    assert(!controller_config_parse(huge, 65537, &config, error, sizeof(error))); free(huge);

    const char *good = "# local profile\nversion=1\r\nport.0.guid=ABCDEF00000000000000000000000001\nport.0.occurrence=0\nport.1.guid=abcdef00000000000000000000000001\nport.1.occurrence=1\nport.3.guid=disabled\nport.0.buttons=1,0,2,3,4,5,6,7,8,9,10,11,12,13,-1\nport.0.axis.0=-100,0,100,10,1\nmapping_file=\"dir with spaces/game\\\"pad\\\\.txt\"\n";
    assert(controller_config_parse(good, strlen(good), &config, error, sizeof(error)));
    assert(strcmp(config.selectors[0].guid, "abcdef00000000000000000000000001") == 0);
    assert(!config.selectors[3].enabled);
    assert(controller_config_save(argv[1], &config, error, sizeof(error)));
    struct stat status;
    assert(stat(argv[1], &status) == 0 && (status.st_mode & 0777) == 0600);
    assert(controller_config_load(argv[1], &loaded, error, sizeof(error)));
    assert(strcmp(config.mapping_file, loaded.mapping_file) == 0);
    assert(loaded.profiles[0].button_map[0] == 1 && loaded.profiles[0].calibration[0].invert);
    assert(loaded.profiles[0].calibration[0].min == -100 && loaded.profiles[0].calibration[0].deadzone == 10);
    assert(loaded.selectors[0].occurrence == 0 && loaded.selectors[1].occurrence == 1);
    assert(!loaded.selectors[3].enabled);
    config.version = 99;
    assert(!controller_config_save(argv[1], &config, error, sizeof(error)));
    assert(controller_config_load(argv[1], &loaded, error, sizeof(error)) && loaded.version == 1);
    assert(!controller_config_load("/nonexistent/tsfp-controller-config", &loaded, error, sizeof(error)));
    assert(!controller_config_save("/nonexistent/tsfp-controller-config", &loaded, error, sizeof(error)));
    char *bad_path = malloc(strlen(argv[1]) + 5);
    assert(bad_path != NULL);
    sprintf(bad_path, "%s.bad", argv[1]);
    FILE *bad_file = fopen(bad_path, "wb");
    assert(bad_file != NULL);
    assert(fputs("version=99\n", bad_file) >= 0);
    assert(fclose(bad_file) == 0);
    controller_config before_load = loaded;
    assert(!controller_config_load(bad_path, &loaded, error, sizeof(error)));
    assert(memcmp(&loaded, &before_load, sizeof(loaded)) == 0);
    assert(remove(bad_path) == 0);
    free(bad_path);
#ifdef CONTROLLER_TEST_RENAME_FAILURE
    controller_config replacement = loaded;
    strcpy(replacement.mapping_file, "must-not-replace-old-path.txt");
    fail_rename = true;
    assert(!controller_config_save(argv[1], &replacement, error, sizeof(error)));
    assert(error[0] != '\0');
    fail_rename = false;
    assert(controller_config_load(argv[1], &replacement, error, sizeof(error)));
    assert(strcmp(replacement.mapping_file, loaded.mapping_file) == 0);
#endif
    puts("controller config: defaults/remaps/calibration/refusals/atomic persistence PASS");
    return 0;
}
