/* cmd_list.c — C:list — detailed directory listing */

#include "cmd_internal.h"
#include "../net/ntp.h"

#define LIST_MAX_ENTRIES 256

static const char *k_months_short[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void fmt_mtime(uint32_t ts, char *out, int max)
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

static int list_cmp_name(const void *a, const void *b)
{
    const RamFsNode *na = *(const RamFsNode **)a;
    const RamFsNode *nb = *(const RamFsNode **)b;
    const char *pa = na->name, *pb = nb->name;
    while (*pa && *pb) {
        char ca = *pa; if (ca >= 'A' && ca <= 'Z') ca += 32;
        char cb = *pb; if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        pa++; pb++;
    }
    return (unsigned char)*pa - (unsigned char)*pb;
}

static void list_format_line(char *out, int max, const char *fmt,
                             RamFsNode *node, int line_no)
{
    out[0] = '\0';
    const char *p = fmt;
    while (*p && cmd_slen(out) < max - 1) {
        if (*p == '%' && p[1]) {
            char spec = p[1];
            p += 2;
            switch (spec) {
                case 'N': case 'n':
                    cmd_scat(out, node->name, max);
                    break;
                case 'S': case 's': {
                    char sz[12];
                    if (node->type == RAMFS_TYPE_DIR)
                        cmd_scopy(sz, "<dir>", 12);
                    else
                        cmd_uint_to_dec(node->size, sz, 12);
                    cmd_scat(out, sz, max);
                    break;
                }
                case 'T': case 't':
                    cmd_scat(out, node->type == RAMFS_TYPE_DIR ? "Dir" : "File", max);
                    break;
                case 'P': case 'p': {
                    char prot[9] = "rwed----";
                    if (node->attrs & RAMFS_ATTR_READONLY) {
                        prot[0] = '-'; prot[1] = '-';
                    }
                    if (node->attrs & RAMFS_ATTR_HIDDEN) {
                        prot[4] = 'h';
                    }
                    cmd_scat(out, prot, max);
                    break;
                }
                case 'C': case 'c':
                    cmd_scat(out, node->comment, max);
                    break;
                case 'D': case 'd': {
                    char dstr[20];
                    fmt_mtime(node->mtime, dstr, sizeof(dstr));
                    cmd_scat(out, dstr, max);
                    break;
                }
                case 'L': case 'l': {
                    char lnum[8];
                    cmd_uint_to_dec((uint32_t)line_no, lnum, 8);
                    cmd_scat(out, lnum, max);
                    break;
                }
                default:
                    break;
            }
        } else {
            int li = cmd_slen(out);
            if (li < max - 1) { out[li] = *p; out[li + 1] = '\0'; }
            p++;
        }
    }
}

static void list_print_entry(NativeCmdCtx *ctx, RamFsNode *node,
                             int dates, int lformat, const char *fmt,
                             int line_no, int *lines)
{
    char line[CMD_MAX_LINE];
    if (lformat && fmt[0]) {
        list_format_line(line, CMD_MAX_LINE, fmt, node, line_no);
        PRINT(line);
        if (lines) (*lines)++;
        return;
    }

    line[0] = '\0';

    /* Protection flags */
    char prot[9] = "rwed----";
    if (node->attrs & RAMFS_ATTR_READONLY) {
        prot[0] = '-'; prot[1] = '-';
    }
    if (node->attrs & RAMFS_ATTR_HIDDEN) {
        prot[4] = 'h';
    }

    /* Size */
    char sz[12];
    if (node->type == RAMFS_TYPE_DIR) {
        cmd_scopy(sz, "   <dir>", 12);
    } else {
        cmd_uint_to_dec(node->size, sz, 12);
    }

    /* Build line */
    cmd_scat(line, "  ", CMD_MAX_LINE);
    cmd_scat(line, prot, CMD_MAX_LINE);
    cmd_scat(line, "  ", CMD_MAX_LINE);
    cmd_scat(line, sz, CMD_MAX_LINE);
    cmd_scat(line, "  ", CMD_MAX_LINE);
    cmd_scat(line, node->name, CMD_MAX_LINE);

    if (dates) {
        char dstr[20];
        fmt_mtime(node->mtime, dstr, sizeof(dstr));
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, dstr, CMD_MAX_LINE);
    }

    /* Comment */
    if (node->comment[0]) {
        cmd_scat(line, "  (", CMD_MAX_LINE);
        cmd_scat(line, node->comment, CMD_MAX_LINE);
        cmd_scat(line, ")", CMD_MAX_LINE);
    }

    PRINT(line);
    if (lines) (*lines)++;
}

static void list_dir(NativeCmdCtx *ctx, const char *path,
                     int all, int dates, int inter, int keys,
                     int lformat, const char *fmt,
                     int *total_lines, int *total_files,
                     int *total_dirs, uint32_t *total_size)
{
    RamFsNode *child = VFS_OpenDir(path);
    if (!child) return;

    /* Collect entries */
    RamFsNode *ents[LIST_MAX_ENTRIES];
    int count = 0;
    while (child && count < LIST_MAX_ENTRIES) {
        ents[count++] = child;
        child = child->next_sibling;
    }

    /* Sort alphabetically */
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - 1 - i; j++) {
            if (list_cmp_name(&ents[j], &ents[j+1]) > 0) {
                RamFsNode *tmp = ents[j];
                ents[j] = ents[j+1];
                ents[j+1] = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        RamFsNode *node = ents[i];

        if (inter) {
            char prompt[CMD_MAX_LINE];
            cmd_scopy(prompt, "List ", CMD_MAX_LINE);
            cmd_scat(prompt, node->name, CMD_MAX_LINE);
            if (!cmd_prompt_yn(ctx, prompt)) continue;
        }

        (*total_files)++;
        if (node->type == RAMFS_TYPE_DIR) {
            (*total_dirs)++;
        } else {
            *total_size += node->size;
        }

        list_print_entry(ctx, node, dates, lformat, fmt, *total_files, total_lines);

        if (keys && total_lines && (*total_lines) % 20 == 0) {
            PRINT("-- Press any key --");
            CMD_READ_KEY(ctx);
        }

        if (all && node->type == RAMFS_TYPE_DIR) {
            char sub[CMD_MAX_PATH];
            cmd_scopy(sub, path, CMD_MAX_PATH);
            int sl = cmd_slen(sub);
            if (sl > 0 && sub[sl - 1] != ':' && sub[sl - 1] != '/') {
                if (sl < CMD_MAX_PATH - 1) { sub[sl] = '/'; sub[sl + 1] = '\0'; }
            }
            cmd_scat(sub, node->name, CMD_MAX_PATH);
            list_dir(ctx, sub, all, dates, inter, keys, lformat, fmt,
                     total_lines, total_files, total_dirs, total_size);
        }
    }
}

void Cmd_List(NativeCmdCtx *ctx, const char *args)
{
    int all     = cmd_kw_find(args, "ALL");
    int dates   = cmd_kw_find(args, "DATES");
    int inter   = cmd_kw_find(args, "INTER");
    int keys    = cmd_kw_find(args, "KEYS");
    int nohead  = cmd_kw_find(args, "NOHEAD");
    int lformat = 0;
    char fmt[CMD_MAX_LINE];
    fmt[0] = '\0';

    /* Parse LFORMAT "format" */
    {
        const char *p = args;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            int len = (int)(p - start);
            if (len == 6 &&
                ((start[0]=='L'||start[0]=='l') && (start[1]=='F'||start[1]=='f') &&
                 (start[2]=='O'||start[2]=='o') && (start[3]=='R'||start[3]=='r') &&
                 (start[4]=='M'||start[4]=='m') && (start[5]=='A'||start[5]=='a') &&
                 (start[6]=='T'||start[6]=='t'))) {
                while (*p == ' ') p++;
                if (*p == '"') {
                    p++;
                    int i = 0;
                    while (*p && *p != '"' && i < CMD_MAX_LINE - 1) fmt[i++] = *p++;
                    fmt[i] = '\0';
                    if (*p == '"') p++;
                    lformat = 1;
                } else {
                    int i = 0;
                    while (*p && *p != ' ' && i < CMD_MAX_LINE - 1) fmt[i++] = *p++;
                    fmt[i] = '\0';
                    lformat = 1;
                }
                break;
            }
        }
    }

    char clean[CMD_MAX_LINE];
    cmd_kw_strip(args, "ALL", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "DATES", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "INTER", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "KEYS", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "NOHEAD", NULL, clean, CMD_MAX_LINE);
    if (lformat) {
        char lfkw[CMD_MAX_LINE + 16];
        lfkw[0] = '\0';
        cmd_scat(lfkw, "LFORMAT ", CMD_MAX_LINE + 16);
        cmd_scat(lfkw, fmt, CMD_MAX_LINE + 16);
        cmd_kw_strip(clean, lfkw, NULL, clean, CMD_MAX_LINE);
    }

    char path[CMD_MAX_PATH];
    if (clean[0])
        cmd_make_abs(ctx->cwd, clean, path, CMD_MAX_PATH);
    else
        cmd_scopy(path, ctx->cwd, CMD_MAX_PATH);

    if (!nohead) {
        char hdr[CMD_MAX_LINE];
        cmd_scopy(hdr, "Directory of ", CMD_MAX_LINE);
        cmd_scat(hdr, path, CMD_MAX_LINE);
        PRINT(hdr);
        PRINT("");
    }

    RamFsNode *child = VFS_OpenDir(path);
    if (!child) {
        PRINT("  (empty or not found)");
        PRINT("");
        return;
    }

    int lines = nohead ? 0 : 2;
    int files = 0, dirs = 0;
    uint32_t total_size = 0;

    list_dir(ctx, path, all, dates, inter, keys, lformat, fmt,
             &lines, &files, &dirs, &total_size);

    PRINT("");
    char summary[CMD_MAX_LINE];
    char cn[8];
    cmd_uint_to_dec((uint32_t)(files + dirs), cn, 8);
    cmd_scopy(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " entries (", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)files, cn, 8);
    cmd_scat(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " files, ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)dirs, cn, 8);
    cmd_scat(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " dirs)", CMD_MAX_LINE);
    PRINT(summary);

    char ts[CMD_MAX_LINE];
    cmd_scopy(ts, "Total bytes: ", CMD_MAX_LINE);
    cmd_uint_to_dec(total_size, cn, 8);
    cmd_scat(ts, cn, CMD_MAX_LINE);
    PRINT(ts);
}
