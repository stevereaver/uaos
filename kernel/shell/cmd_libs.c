/* cmd_libs.c — C:libs — list loaded kernel ROM libraries */

#include "cmd_internal.h"
#include "../exec/rom_modules.h"

void Cmd_Libs(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    char *names[64];
    uint16_t versions[64];
    int count = UAOS_ROM_ListAll(names, versions, 64);

    if (count == 0) {
        PRINT("No kernel libraries loaded.");
        return;
    }

    char hdr[CMD_MAX_LINE];
    char num[12];
    cmd_scopy(hdr, "Loaded kernel libraries (", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)count, num, 12);
    cmd_scat(hdr, num, CMD_MAX_LINE);
    cmd_scat(hdr, "):", CMD_MAX_LINE);
    PRINT(hdr);

    for (int i = 0; i < count; i++) {
        char line[CMD_MAX_LINE];
        cmd_scopy(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, names[i], CMD_MAX_LINE);
        cmd_scat(line, " v", CMD_MAX_LINE);
        cmd_uint_to_dec(versions[i], num, 12);
        cmd_scat(line, num, CMD_MAX_LINE);
        PRINT(line);
    }
}
