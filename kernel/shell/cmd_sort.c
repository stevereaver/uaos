/* cmd_sort.c — C:sort — sort lines of a file */

#include "cmd_internal.h"

#define MAX_SORT_LINES 256
#define MAX_SORT_LINE  CMD_MAX_LINE

static char g_sort_buf[MAX_SORT_LINES][MAX_SORT_LINE];
static int  g_sort_count;

static int sort_col_start = 0;
static int sort_col_end   = MAX_SORT_LINE;

static int sort_extract_col(const char *line, int col, char *out, int max)
{
    int i = 0, c = 0;
    /* Skip leading spaces */
    while (line[i] && line[i] == ' ') i++;
    while (c < col && line[i]) {
        while (line[i] && line[i] != ' ') i++;
        while (line[i] && line[i] == ' ') i++;
        c++;
    }
    int j = 0;
    while (line[i] && line[i] != ' ' && j < max - 1) out[j++] = line[i++];
    out[j] = '\0';
    return j;
}

static int sort_cmp_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i]; if (ac >= 'A' && ac <= 'Z') ac += 32;
        char bc = b[i]; if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return ac - bc;
        i++;
    }
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static int sort_cmp_num(const char *a, const char *b)
{
    int na = 0, nb = 0;
    int ha = 0, hb = 0;
    /* Try to extract leading numbers */
    const char *pa = a;
    const char *pb = b;
    while (*pa && (*pa < '0' || *pa > '9')) pa++;
    while (*pb && (*pb < '0' || *pb > '9')) pb++;
    while (*pa >= '0' && *pa <= '9') { na = na * 10 + (*pa - '0'); ha = 1; pa++; }
    while (*pb >= '0' && *pb <= '9') { nb = nb * 10 + (*pb - '0'); hb = 1; pb++; }
    if (ha && hb) {
        if (na != nb) return na - nb;
    }
    return sort_cmp_ci(a, b);
}

static int sort_cmp(const char *a, const char *b, int numeric, int case_sens)
{
    if (numeric) return sort_cmp_num(a, b);
    if (!case_sens) return sort_cmp_ci(a, b);
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return a[i] - b[i];
        i++;
    }
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static void sort_swap(int i, int j)
{
    char tmp[MAX_SORT_LINE];
    cmd_scopy(tmp, g_sort_buf[i], MAX_SORT_LINE);
    cmd_scopy(g_sort_buf[i], g_sort_buf[j], MAX_SORT_LINE);
    cmd_scopy(g_sort_buf[j], tmp, MAX_SORT_LINE);
}

void Cmd_Sort(NativeCmdCtx *ctx, const char *args)
{
    int case_sens = 0;
    int numeric = 0;
    int col = 0;
    const char *file_arg = NULL;

    if (ctx->template) {
        case_sens = CmdTemplate_GetSwitch(ctx->template, "CASE");
        numeric   = CmdTemplate_GetSwitch(ctx->template, "NUMERIC");
        file_arg  = CmdTemplate_GetString(ctx->template, "FILE");
        int cval = 0;
        if (CmdTemplate_GetInt(ctx->template, "COL", &cval)) {
            col = cval;
        }
    } else {
        /* Legacy manual parsing when no template is present */
        case_sens = cmd_kw_find(args, "CASE");
        numeric   = cmd_kw_find(args, "NUMERIC");
        {
            const char *p = args;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                const char *start = p;
                while (*p && *p != ' ') p++;
                int len = (int)(p - start);
                if (len == 3 &&
                    ((start[0]=='C'||start[0]=='c') && (start[1]=='O'||start[1]=='o') &&
                     (start[2]=='L'||start[2]=='l'))) {
                    while (*p == ' ') p++;
                    col = 0;
                    while (*p >= '0' && *p <= '9') { col = col * 10 + (*p - '0'); p++; }
                    break;
                }
            }
        }
        /* Strip flags to get filename */
        char clean[CMD_MAX_LINE];
        clean[0] = '\0';
        cmd_kw_strip(args, "CASE", NULL, clean, CMD_MAX_LINE);
        cmd_kw_strip(clean, "NUMERIC", NULL, clean, CMD_MAX_LINE);
        if (col >= 0) {
            char ckw[CMD_MAX_LINE];
            ckw[0] = '\0';
            cmd_scat(ckw, "COL ", CMD_MAX_LINE);
            char cnum[8];
            cmd_uint_to_dec((uint32_t)col, cnum, 8);
            cmd_scat(ckw, cnum, CMD_MAX_LINE);
            cmd_kw_strip(clean, ckw, NULL, clean, CMD_MAX_LINE);
        }
        if (clean[0]) file_arg = clean;
    }

    if (!file_arg && (!args || !*args) && !ctx->pipe_file) {
        PRINT("Usage: sort <file> [COL <n>] [CASE] [NUMERIC]");
        return;
    }

    char path[CMD_MAX_PATH];
    if (file_arg && *file_arg) {
        cmd_make_abs(ctx->cwd, file_arg, path, CMD_MAX_PATH);
    } else if ((!args || !*args) && ctx->pipe_file) {
        cmd_scopy(path, ctx->pipe_file, CMD_MAX_PATH);
    } else {
        cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);
    }

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open: ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    g_sort_count = 0;
    uint32_t pos = 0;
    uint32_t sz = VFS_Size(&fh);

    while (pos < sz && g_sort_count < MAX_SORT_LINES) {
        int col = 0;
        while (pos < sz && col < MAX_SORT_LINE - 1) {
            uint8_t c;
            if (VFS_Read(&fh, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') g_sort_buf[g_sort_count][col++] = (char)c;
        }
        g_sort_buf[g_sort_count][col] = '\0';
        g_sort_count++;
    }
    VFS_Close(&fh);

    /* Bubble sort */
    for (int i = 0; i < g_sort_count - 1; i++) {
        for (int j = 0; j < g_sort_count - 1 - i; j++) {
            char key_a[MAX_SORT_LINE];
            char key_b[MAX_SORT_LINE];
            if (col > 0) {
                sort_extract_col(g_sort_buf[j],     col, key_a, MAX_SORT_LINE);
                sort_extract_col(g_sort_buf[j + 1], col, key_b, MAX_SORT_LINE);
            } else {
                cmd_scopy(key_a, g_sort_buf[j],     MAX_SORT_LINE);
                cmd_scopy(key_b, g_sort_buf[j + 1], MAX_SORT_LINE);
            }
            if (sort_cmp(key_a, key_b, numeric, case_sens) > 0) {
                sort_swap(j, j + 1);
            }
        }
    }

    for (int i = 0; i < g_sort_count; i++) {
        PRINT(g_sort_buf[i]);
    }
}
