/* cmd_sort.c — C:sort — sort lines of a file */

#include "cmd_internal.h"

#define MAX_SORT_LINES 256
#define MAX_SORT_LINE  CMD_MAX_LINE

static char g_sort_buf[MAX_SORT_LINES][MAX_SORT_LINE];
static int  g_sort_count;

static int sort_cmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i]; if (ac >= 'A' && ac <= 'Z') ac += 32;
        char bc = b[i]; if (bc >= 'B' && bc <= 'Z') bc += 32;
        if (ac != bc) return ac - bc;
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
    if (!args || !*args) {
        PRINT("Usage: sort <file>");
        return;
    }

    char path[CMD_MAX_PATH];
    cmd_make_abs(ctx->cwd, args, path, CMD_MAX_PATH);

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
            if (sort_cmp(g_sort_buf[j], g_sort_buf[j + 1]) > 0) {
                sort_swap(j, j + 1);
            }
        }
    }

    for (int i = 0; i < g_sort_count; i++) {
        PRINT(g_sort_buf[i]);
    }
}
