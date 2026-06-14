/* cmd_more.c — C:more — paginate file output one screen at a time */

#include "cmd_internal.h"

/* -------------------------------------------------------------------------
 * Cmd_More
 *
 * Usage: more <file>
 *
 * Prints the file page by page.  After each page a "--More--" prompt is
 * shown and the command waits for:
 *   Space / Page-Down : advance one full page
 *   Enter             : advance one line
 *   q / Escape        : quit immediately
 *
 * The page size is taken from ctx->visible_rows (set by shell_win.c to the
 * actual number of text rows visible in the history pane) minus one row
 * reserved for the "--More--" line.  Falls back to 20 if not set.
 * ------------------------------------------------------------------------- */
void Cmd_More(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) { PRINT("Usage: more <file>"); return; }

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

    /* Page height: visible rows minus one for the --More-- prompt line.
     * Minimum is 2 so we always show at least one content line. */
    int page_h = ctx->visible_rows > 2 ? ctx->visible_rows - 1 : 20;

    uint32_t pos = 0;
    uint32_t sz  = VFS_Size(&fh);
    int      line_in_page = 0;

    while (pos < sz) {
        /* Read one line */
        uint8_t buf[CMD_MAX_LINE];
        int col = 0;
        while (pos < sz && col < CMD_MAX_LINE - 1) {
            uint8_t c;
            if (VFS_Read(&fh, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = c;
        }
        buf[col] = '\0';

        PRINT((char *)buf);
        line_in_page++;

        /* At the end of each page (and not at EOF) wait for a keypress */
        if (line_in_page >= page_h && pos < sz) {
            PRINT("--More-- [Space=page  Enter=line  q=quit]");

            char k = CMD_READ_KEY(ctx);

            /* Erase the --More-- line from history by printing an empty line */
            PRINT("");

            if (k == 'q' || k == 'Q' || k == 27 /* ESC */) {
                VFS_Close(&fh);
                return;
            } else if (k == '\r' || k == '\n') {
                /* One-line advance: keep all but the last count */
                line_in_page = page_h - 1;
            } else {
                /* Space, Page-Down, or anything else: full page */
                line_in_page = 0;
            }
        }
    }

    VFS_Close(&fh);
}
