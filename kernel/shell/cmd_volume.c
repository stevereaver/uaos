/* cmd_volume.c — C:volume — get/set master output volume and mute
 *
 *   volume           print current volume + backend
 *   volume N         set volume 0-100
 *   volume MUTE      toggle mute
 */

#include "cmd_internal.h"
#include "../audio/audio.h"
#include "../display/desktop.h"

void Cmd_Volume(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE];

    if (args && *args) {
        while (*args == ' ') args++;
        if (cmd_seq_ci(args, "mute")) {
            audio_mute_toggle();
        } else {
            uint64_t v;
            if (!cmd_parse_uint(args, &v)) {
                PRINT("usage: volume [0-100 | MUTE]");
                return;
            }
            audio_set_volume((int)v);
        }
        Desktop_MenubarInvalidate();
    }

    cmd_scopy(line, "volume: ", CMD_MAX_LINE);
    if (audio_get_mute()) {
        cmd_scat(line, "MUTE", CMD_MAX_LINE);
    } else {
        char num[8];
        cmd_uint_to_dec((uint32_t)audio_get_volume(), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
    }
    cmd_scat(line, " (backend ", CMD_MAX_LINE);
    const char *name = audio_backend_name();
    cmd_scat(line, name ? name : "none", CMD_MAX_LINE);
    cmd_scat(line, ")", CMD_MAX_LINE);
    PRINT(line);
}
