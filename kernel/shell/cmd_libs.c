/* cmd_libs.c — C:libs — list loaded kernel ROM and loadable libraries */

#include "cmd_internal.h"
#include "../exec/rom_modules.h"
#include "../exec/loadable_lib.h"

void Cmd_Libs(NativeCmdCtx *ctx, const char *args)
{
    (void)ctx; (void)args;
    char *names[64];
    uint16_t versions[64];
    int rom_count = UAOS_ROM_ListAll(names, versions, 64);
    int load_count = UAOS_LoadableLib_ListAll(names + rom_count,
                                                versions + rom_count,
                                                64 - rom_count);
    int total = rom_count + load_count;

    if (total == 0) {
        PRINT("No kernel libraries loaded.");
        return;
    }

    char hdr[CMD_MAX_LINE];
    char num[12];
    cmd_scopy(hdr, "Loaded kernel libraries (", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)total, num, 12);
    cmd_scat(hdr, num, CMD_MAX_LINE);
    cmd_scat(hdr, "):", CMD_MAX_LINE);
    PRINT(hdr);

    int idx = 0;
    if (rom_count > 0) {
        PRINT("  [ROM]");
        for (int i = 0; i < rom_count; i++, idx++) {
            char line[CMD_MAX_LINE];
            cmd_scopy(line, "    ", CMD_MAX_LINE);
            cmd_scat(line, names[idx], CMD_MAX_LINE);
            cmd_scat(line, " v", CMD_MAX_LINE);
            cmd_uint_to_dec(versions[idx], num, 12);
            cmd_scat(line, num, CMD_MAX_LINE);
            PRINT(line);
        }
    }

    if (load_count > 0) {
        PRINT("  [Loadable]");
        for (int i = 0; i < load_count; i++, idx++) {
            char line[CMD_MAX_LINE];
            cmd_scopy(line, "    ", CMD_MAX_LINE);
            cmd_scat(line, names[idx], CMD_MAX_LINE);
            cmd_scat(line, " v", CMD_MAX_LINE);
            cmd_uint_to_dec(versions[idx], num, 12);
            cmd_scat(line, num, CMD_MAX_LINE);
            PRINT(line);
        }
    }
}
