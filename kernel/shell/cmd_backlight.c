/* cmd_backlight.c — C:backlight — panel brightness via GPU PWM (UAOS-139)
 *
 *   backlight            — print current brightness (0-100)
 *   backlight <percent>  — set brightness
 *
 * Drives the NVIDIA NV50+ SOR PWM (nv50bl.c) on bare metal.  On machines
 * without a supported GPU the driver is absent and the command reports
 * that cleanly.
 */

#include "cmd_internal.h"

extern int NV50BL_Present(void);
extern int NV50BL_GetBrightness(void);
extern int NV50BL_SetBrightness(int pct);

void Cmd_Backlight(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE];

    if (!NV50BL_Present()) {
        PRINT("backlight: no supported GPU PWM found");
        return;
    }

    /* No argument — report current level. */
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        char num[12];
        cmd_uint_to_dec(NV50BL_GetBrightness(), num, 12);
        cmd_scopy(line, "backlight: ", CMD_MAX_LINE);
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " %", CMD_MAX_LINE);
        PRINT(line);
        return;
    }

    uint64_t v;
    if (!cmd_parse_uint(args, &v)) {
        PRINT("usage: backlight [0-100]");
        return;
    }

    int duty = NV50BL_SetBrightness((int)v);
    if (duty < 0) {
        PRINT("backlight: set failed");
        return;
    }
    char num[12];
    cmd_scopy(line, "backlight: set to ", CMD_MAX_LINE);
    cmd_uint_to_dec(NV50BL_GetBrightness(), num, 12);
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " % (duty ", CMD_MAX_LINE);
    cmd_uint_to_dec(duty, num, 12);
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, ")", CMD_MAX_LINE);
    PRINT(line);
}
