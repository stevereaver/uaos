/* cmd_dir.c — C:dir — list a directory */

#include "cmd_internal.h"

#define DIR_MAX_ENTRIES 256

static int dir_cmp_name(const void *a, const void *b)
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

static int dir_cmp_dirfirst(const void *a, const void *b)
{
    const RamFsNode *na = *(const RamFsNode **)a;
    const RamFsNode *nb = *(const RamFsNode **)b;
    if (na->type == RAMFS_TYPE_DIR && nb->type != RAMFS_TYPE_DIR) return -1;
    if (na->type != RAMFS_TYPE_DIR && nb->type == RAMFS_TYPE_DIR) return 1;
    return dir_cmp_name(a, b);
}

static void dir_print_entry(NativeCmdCtx *ctx, RamFsNode *node,
                            int dates, int *lines)
{
    char line[CMD_MAX_LINE];
    line[0] = '\0';

    if (node->type == RAMFS_TYPE_DIR) {
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, node->name, CMD_MAX_LINE);
        cmd_scat(line, "  (dir)", CMD_MAX_LINE);
    } else {
        char sz[12];
        cmd_uint_to_dec(node->size, sz, 12);
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, node->name, CMD_MAX_LINE);
        cmd_scat(line, "  ", CMD_MAX_LINE);
        cmd_scat(line, sz, CMD_MAX_LINE);
        cmd_scat(line, " bytes", CMD_MAX_LINE);
    }

    if (dates) {
        cmd_scat(line, "  --/--/--", CMD_MAX_LINE);
    }

    PRINT(line);
    if (lines) (*lines)++;
}

static void dir_list(NativeCmdCtx *ctx, const char *path,
                     int all, int dates, int inter, int keys,
                     int opt_alpha, int opt_dirfirst,
                     int *total_lines)
{
    RamFsNode *child = VFS_OpenDir(path);
    if (!child) return;

    /* Collect entries */
    RamFsNode *ents[DIR_MAX_ENTRIES];
    int count = 0;
    while (child && count < DIR_MAX_ENTRIES) {
        ents[count++] = child;
        child = child->next_sibling;
    }

    /* Sort */
    if (opt_dirfirst) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - 1 - i; j++) {
                if (dir_cmp_dirfirst(&ents[j], &ents[j+1]) > 0) {
                    RamFsNode *tmp = ents[j];
                    ents[j] = ents[j+1];
                    ents[j+1] = tmp;
                }
            }
        }
    } else if (opt_alpha) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - 1 - i; j++) {
                if (dir_cmp_name(&ents[j], &ents[j+1]) > 0) {
                    RamFsNode *tmp = ents[j];
                    ents[j] = ents[j+1];
                    ents[j+1] = tmp;
                }
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

        dir_print_entry(ctx, node, dates, total_lines);

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
            dir_list(ctx, sub, all, dates, inter, keys, opt_alpha, opt_dirfirst, total_lines);
        }
    }
}

void Cmd_Dir(NativeCmdCtx *ctx, const char *args)
{
    int all         = cmd_kw_find(args, "ALL");
    int dates       = cmd_kw_find(args, "DATES");
    int inter       = cmd_kw_find(args, "INTER");
    int keys        = cmd_kw_find(args, "KEYS");
    int opt_alpha   = 0;
    int opt_dirfirst = 0;

    /* Parse OPT keyword */
    char opt_str[8];
    opt_str[0] = '\0';
    {
        const char *p = args;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            int len = (int)(p - start);
            if (len == 3 &&
                ((start[0]=='O'||start[0]=='o') && (start[1]=='P'||start[1]=='p') && (start[2]=='T'||start[2]=='t'))) {
                while (*p == ' ') p++;
                int i = 0;
                while (*p && *p != ' ' && i < 7) opt_str[i++] = *p++;
                opt_str[i] = '\0';
                break;
            }
        }
    }
    if (opt_str[0]) {
        for (int i = 0; opt_str[i]; i++) {
            char c = opt_str[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c == 'a') opt_alpha = 1;
            if (c == 'd') opt_dirfirst = 1;
        }
    }

    char clean[CMD_MAX_LINE];
    cmd_kw_strip(args, "ALL", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "DATES", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "INTER", NULL, clean, CMD_MAX_LINE);
    cmd_kw_strip(clean, "KEYS", NULL, clean, CMD_MAX_LINE);
    if (opt_str[0]) {
        char opt_kw[CMD_MAX_LINE];
        opt_kw[0] = '\0';
        cmd_scat(opt_kw, "OPT ", CMD_MAX_LINE);
        cmd_scat(opt_kw, opt_str, CMD_MAX_LINE);
        cmd_kw_strip(clean, opt_kw, NULL, clean, CMD_MAX_LINE);
    }

    char path[CMD_MAX_PATH];
    if (clean[0])
        cmd_make_abs(ctx->cwd, clean, path, CMD_MAX_PATH);
    else
        cmd_scopy(path, ctx->cwd, CMD_MAX_PATH);

    char hdr[CMD_MAX_LINE];
    cmd_scopy(hdr, "Directory of ", CMD_MAX_LINE);
    cmd_scat(hdr, path, CMD_MAX_LINE);
    PRINT(hdr);
    PRINT("");

    RamFsNode *child = VFS_OpenDir(path);
    if (!child) {
        PRINT("  (empty or not found)");
        PRINT("");
        return;
    }

    int lines = 2; /* header + blank */
    dir_list(ctx, path, all, dates, inter, keys, opt_alpha, opt_dirfirst, &lines);

    PRINT("");

    /* Compute bytes used by files in this directory */
    uint32_t bytes_used = 0;
    int count = 0;
    RamFsNode *n = child;
    while (n) {
        count++;
        if (n->type == RAMFS_TYPE_FILE) bytes_used += n->size;
        n = n->next_sibling;
    }

    uint32_t total = 0, used = 0;
    VFS_GetVolumeInfo(path, &total, &used);
    uint32_t free_bytes = (total > used) ? (total - used) : 0;

    char summary[CMD_MAX_LINE];
    char cn[8]; cn[0] = '\0';
    cmd_uint_to_dec((uint32_t)count, cn, 8);
    cmd_scopy(summary, cn, CMD_MAX_LINE);
    cmd_scat(summary, " item(s)  ", CMD_MAX_LINE);

    char bu[12]; bu[0] = '\0';
    cmd_uint_to_dec(bytes_used, bu, 12);
    cmd_scat(summary, bu, CMD_MAX_LINE);
    cmd_scat(summary, " bytes used  ", CMD_MAX_LINE);

    char bf[12]; bf[0] = '\0';
    cmd_uint_to_dec(free_bytes, bf, 12);
    cmd_scat(summary, bf, CMD_MAX_LINE);
    cmd_scat(summary, " bytes free", CMD_MAX_LINE);
    PRINT(summary);
}
