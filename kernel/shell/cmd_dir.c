/* cmd_dir.c — C:dir — list a directory */

#include "cmd_internal.h"
#include "dos/vfs.h"
#include "../net/ntp.h"

#define DIR_MAX_ENTRIES 256

static VfsDirEnt g_dir_entries[DIR_MAX_ENTRIES];

static const char *k_months_short[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void fmt_mtime_dir(uint32_t ts, char *out, int max)
{
    if (ts == 0) {
        cmd_scopy(out, "--/--/--", max);
        return;
    }
    uint16_t year; uint8_t month, day, hour, min, sec;
    ntp_unix_to_datetime(ts, &year, &month, &day, &hour, &min, &sec);
    if (month < 1 || month > 12) month = 1;
    /* DD-Mon-YYYY HH:MM */
    out[0] = (char)('0' + day / 10);
    out[1] = (char)('0' + day % 10);
    out[2] = '-';
    const char *m = k_months_short[month - 1];
    out[3] = m[0]; out[4] = m[1]; out[5] = m[2];
    out[6] = '-';
    out[7] = (char)('0' + (year / 1000) % 10);
    out[8] = (char)('0' + (year / 100) % 10);
    out[9] = (char)('0' + (year / 10) % 10);
    out[10] = (char)('0' + year % 10);
    out[11] = ' ';
    out[12] = (char)('0' + hour / 10);
    out[13] = (char)('0' + hour % 10);
    out[14] = ':';
    out[15] = (char)('0' + min / 10);
    out[16] = (char)('0' + min % 10);
    out[17] = '\0';
    (void)sec;
    (void)max;
}

static int dir_cmp_name(const void *a, const void *b)
{
    const VfsDirEnt *na = (const VfsDirEnt *)a;
    const VfsDirEnt *nb = (const VfsDirEnt *)b;
    const char *pa = na->name, *pb = nb->name;
    while (*pa && *pb) {
        char ca = *pa; if (ca >= 'A' && ca <= 'Z') ca += 32;
        char cb = *pb; if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        pa++; pb++;
    }
    return (unsigned char)*pa - (unsigned char)*pb;
}

static int dir_cmp_dirfirst(const void *a, const void *b)
{
    const VfsDirEnt *na = (const VfsDirEnt *)a;
    const VfsDirEnt *nb = (const VfsDirEnt *)b;
    if (na->is_dir && !nb->is_dir) return -1;
    if (!na->is_dir && nb->is_dir) return 1;
    return dir_cmp_name(a, b);
}

static void dir_print_entry(NativeCmdCtx *ctx, const VfsDirEnt *ent,
                            int dates, int *lines)
{
    char line[CMD_MAX_LINE];
    line[0] = '\0';

    if (ent->is_dir) {
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, ent->name, CMD_MAX_LINE);
        cmd_scat(line, "  (dir)", CMD_MAX_LINE);
    } else {
        char sz[12];
        cmd_uint_to_dec(ent->size, sz, 12);
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, ent->name, CMD_MAX_LINE);
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, sz, CMD_MAX_LINE);
        cmd_scat(line, " bytes", CMD_MAX_LINE);
    }

    if (dates) {
        char dstr[20];
        fmt_mtime_dir(0, dstr, sizeof(dstr));  /* timestamps not in VfsDirEnt yet */
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, dstr, CMD_MAX_LINE);
    }

    PRINT(line);
    if (lines) (*lines)++;
}

static void dir_list(NativeCmdCtx *ctx, const char *path, const char *pat,
                     int all, int dates, int inter, int keys,
                     int opt_alpha, int opt_dirfirst,
                     int *total_lines)
{
    VfsDirEnt *ents = g_dir_entries;
    int count = VFS_ReadDir(path, ents, DIR_MAX_ENTRIES);
    if (count == 0) return;

    /* Sort */
    if (opt_dirfirst) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - 1 - i; j++) {
                if (dir_cmp_dirfirst(&ents[j], &ents[j+1]) > 0) {
                    VfsDirEnt tmp = ents[j];
                    ents[j] = ents[j+1];
                    ents[j+1] = tmp;
                }
            }
        }
    } else if (opt_alpha) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - 1 - i; j++) {
                if (dir_cmp_name(&ents[j], &ents[j+1]) > 0) {
                    VfsDirEnt tmp = ents[j];
                    ents[j] = ents[j+1];
                    ents[j+1] = tmp;
                }
            }
        }
    }

    for (int i = 0; i < count; i++) {
        const VfsDirEnt *ent = &ents[i];

        if (inter) {
            char prompt[CMD_MAX_LINE];
            cmd_scopy(prompt, "List ", CMD_MAX_LINE);
            cmd_scat(prompt, ent->name, CMD_MAX_LINE);
            if (!cmd_prompt_yn(ctx, prompt)) continue;
        }

        dir_print_entry(ctx, ent, dates, total_lines);

        if (keys && total_lines && (*total_lines) % 20 == 0) {
            PRINT("-- Press any key --");
            CMD_READ_KEY(ctx);
        }

        if (all && ent->is_dir) {
            char sub[CMD_MAX_PATH];
            cmd_scopy(sub, path, CMD_MAX_PATH);
            int sl = cmd_slen(sub);
            if (sl > 0 && sub[sl - 1] != ':' && sub[sl - 1] != '/') {
                if (sl < CMD_MAX_PATH - 1) { sub[sl] = '/'; sub[sl + 1] = '\0'; }
            }
            cmd_scat(sub, ent->name, CMD_MAX_PATH);
            dir_list(ctx, sub, pat, all, dates, inter, keys, opt_alpha, opt_dirfirst, total_lines);
        }
    }
}

void Cmd_Dir(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    int all         = CmdTemplate_GetSwitch(ctx->template, "ALL");
    int dates       = CmdTemplate_GetSwitch(ctx->template, "DATES");
    int inter       = CmdTemplate_GetSwitch(ctx->template, "INTER");
    int keys        = CmdTemplate_GetSwitch(ctx->template, "KEYS");
    int opt_alpha   = 0;
    int opt_dirfirst = 0;

    const char *opt_str = CmdTemplate_GetString(ctx->template, "OPT");
    if (opt_str) {
        for (int i = 0; opt_str[i]; i++) {
            char c = opt_str[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c == 'a') opt_alpha = 1;
            if (c == 'd') opt_dirfirst = 1;
        }
    }

    char path[CMD_MAX_PATH];
    char pat[CMD_MAX_PATH];
    const char *dir = CmdTemplate_GetString(ctx->template, "DIR");
    if (dir) {
        cmd_split_path_pat(ctx->cwd, dir, path, pat);
    } else {
        cmd_scopy(path, ctx->cwd, CMD_MAX_PATH);
        pat[0] = '\0';
    }

    char hdr[CMD_MAX_LINE];
    cmd_scopy(hdr, "Directory of ", CMD_MAX_LINE);
    cmd_scat(hdr, path, CMD_MAX_LINE);
    PRINT(hdr);

    int total_lines = 0;
    dir_list(ctx, path, pat, all, dates, inter, keys,
             opt_alpha, opt_dirfirst, &total_lines);

    if (!all) {
        char sum[CMD_MAX_LINE];
        cmd_scopy(sum, "  ", CMD_MAX_LINE);
        char nstr[12];
        cmd_uint_to_dec((uint32_t)total_lines, nstr, sizeof(nstr));
        cmd_scat(sum, nstr, CMD_MAX_LINE);
        cmd_scat(sum, " item(s)", CMD_MAX_LINE);
        PRINT(sum);
    }
}
