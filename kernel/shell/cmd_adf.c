/* cmd_adf.c — C:adf — ADF floppy insert/eject/list
 *
 * Syntax: adf [list]
 *         adf insert <unit> <path>
 *         adf eject  <unit> [discard]
 *
 * <unit> accepts 0-3, DF0-DF3 or DF0:-DF3: (case-insensitive).
 * <path> may be double-quoted; a path without a volume name is resolved
 * against the shell's cwd so a later eject can find the file from any
 * shell.  insert loads a 901120-byte ADF file into the drive and mounts
 * it; eject unmounts, writes a dirty disk back to its source file
 * (unless discard is given) and removes the disk.
 *
 * Return codes: 0 = success, 10 = occupied/volume-busy/empty,
 *               20 = other errors.
 */

#include "cmd_internal.h"
#include "dos/adf_mount.h"
#include "chipset/floppy.h"

/* Parse "0"-"3", "DF0"-"DF3" or "DF0:"-"DF3:" -> unit index, or -1. */
static int adf_unit_arg(const char *s)
{
    if (!s || !*s) return -1;
    if ((s[0] == 'D' || s[0] == 'd') && (s[1] == 'F' || s[1] == 'f')) s += 2;
    if (s[0] < '0' || s[0] > '0' + FLOPPY_UNITS - 1) return -1;
    int u = s[0] - '0';
    s++;
    if (*s == ':') s++;
    return *s ? -1 : u;
}

/* Copy one argument (optionally double-quoted) into dst; returns chars
 * consumed from src, or 0 if the argument is empty. */
static int adf_take_arg(const char *p, char *dst, int max)
{
    int n = 0, i = 0;
    if (*p == '"') {
        n = 1;
        while (p[n] && p[n] != '"' && i < max - 1)
            dst[i++] = p[n++];
        if (p[n] == '"') n++;
    } else {
        while (p[n] && p[n] != ' ' && i < max - 1)
            dst[i++] = p[n++];
        n = i;
    }
    dst[i] = '\0';
    return n;
}

static void adf_list(NativeCmdCtx *ctx)
{
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        AdfUnitInfo inf;
        AdfMount_UnitInfo(u, &inf);

        char line[CMD_MAX_LINE];
        char num[8];
        cmd_uint_to_dec((uint32_t)u, num, sizeof(num));
        cmd_scopy(line, "DF", CMD_MAX_LINE);
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, ": ", CMD_MAX_LINE);

        if (!inf.loaded) {
            cmd_scat(line, "empty", CMD_MAX_LINE);
        } else if (inf.mounted) {
            cmd_scat(line, inf.volume, CMD_MAX_LINE);
            if (inf.path[0]) {
                cmd_scat(line, " (", CMD_MAX_LINE);
                cmd_scat(line, inf.path, CMD_MAX_LINE);
                cmd_scat(line, ")", CMD_MAX_LINE);
            }
            if (inf.dirty) cmd_scat(line, " modified", CMD_MAX_LINE);
        } else {
            if (inf.volume[0]) cmd_scat(line, inf.volume, CMD_MAX_LINE);
            else               cmd_scat(line, "inserted", CMD_MAX_LINE);
            if (inf.path[0]) {
                cmd_scat(line, " (", CMD_MAX_LINE);
                cmd_scat(line, inf.path, CMD_MAX_LINE);
                cmd_scat(line, ")", CMD_MAX_LINE);
            }
            if (inf.dirty) cmd_scat(line, " modified", CMD_MAX_LINE);
            cmd_scat(line, " not mounted", CMD_MAX_LINE);
        }
        if (inf.loaded && inf.write_protected)
            cmd_scat(line, " write-protected", CMD_MAX_LINE);
        PRINT(line);
    }
}

static int adf_rc_for(int err)
{
    if (err == ADF_OK) return 0;
    if (err == ADF_ERR_OCCUPIED || err == ADF_ERR_VOLBUSY ||
        err == ADF_ERR_EMPTY)
        return 10;
    return 20;
}

void Cmd_Adf(NativeCmdCtx *ctx, const char *args)
{
    char sub[16] = {0};
    const char *p = args ? args : "";
    while (*p == ' ') p++;
    p += adf_take_arg(p, sub, sizeof(sub));

    if (!sub[0] || cmd_seq_ci(sub, "list")) {
        adf_list(ctx);
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 0);
        return;
    }

    int do_insert = cmd_seq_ci(sub, "insert");
    int do_eject  = cmd_seq_ci(sub, "eject");
    if (!do_insert && !do_eject) {
        PRINT("Usage: adf [list]");
        PRINT("       adf insert <unit> <path>");
        PRINT("       adf eject  <unit> [discard]");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    while (*p == ' ') p++;
    char unit_arg[16] = {0};
    p += adf_take_arg(p, unit_arg, sizeof(unit_arg));
    int unit = adf_unit_arg(unit_arg);
    if (unit < 0) {
        PRINT("adf: invalid unit (use DF0-DF3)");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
        return;
    }

    char msg[CMD_MAX_LINE];
    int err;

    if (do_insert) {
        while (*p == ' ') p++;
        char path[128] = {0};
        adf_take_arg(p, path, sizeof(path));
        if (!path[0]) {
            PRINT("adf insert: no file specified");
            if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
            return;
        }
        /* Anchor relative paths at the shell cwd so a later eject (from
         * any shell, or a disk icon) finds the source file again. */
        char abspath[128];
        cmd_make_abs(ctx->cwd, path, abspath, sizeof(abspath));
        err = AdfMount_Insert(unit, abspath);
        if (err == ADF_OK) {
            AdfUnitInfo inf;
            AdfMount_UnitInfo(unit, &inf);
            cmd_scopy(msg, "adf: inserted ", CMD_MAX_LINE);
            cmd_scat(msg, inf.volume, CMD_MAX_LINE);
            cmd_scat(msg, " in DF", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)unit, unit_arg, sizeof(unit_arg));
            cmd_scat(msg, unit_arg, CMD_MAX_LINE);
            PRINT(msg);
        }
    } else {
        while (*p == ' ') p++;
        char kw[16] = {0};
        adf_take_arg(p, kw, sizeof(kw));
        if (kw[0] && !cmd_seq_ci(kw, "discard")) {
            PRINT("Usage: adf eject <unit> [discard]");
            if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
            return;
        }
        err = AdfMount_Eject(unit, kw[0] ? 1 : 0);
        if (err == ADF_OK) {
            cmd_scopy(msg, "adf: DF", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)unit, unit_arg, sizeof(unit_arg));
            cmd_scat(msg, unit_arg, CMD_MAX_LINE);
            cmd_scat(msg, " ejected", CMD_MAX_LINE);
            PRINT(msg);
        }
    }

    if (err != ADF_OK) {
        cmd_scopy(msg, "adf: ", CMD_MAX_LINE);
        cmd_scat(msg, AdfMount_ErrorString(err), CMD_MAX_LINE);
        PRINT(msg);
    }
    if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, adf_rc_for(err));
}
