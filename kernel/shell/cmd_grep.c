/* cmd_grep.c — C:grep — search file contents for a pattern */

#include "cmd_internal.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Case-insensitive byte compare */
static int grep_ci_eq(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* Return 1 if needle is found anywhere in haystack (length hl).
 * Supports two modes: exact (case-sensitive) and case-insensitive. */
static int grep_match(const char *needle, int nl,
                      const uint8_t *hay, int hl,
                      int ci)
{
    if (nl == 0) return 1;          /* empty pattern matches every line */
    for (int i = 0; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            if (ci) {
                if (!grep_ci_eq((unsigned char)needle[j], hay[i + j]))
                    { ok = 0; break; }
            } else {
                if ((unsigned char)needle[j] != hay[i + j])
                    { ok = 0; break; }
            }
        }
        if (ok) return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Cmd_Grep
 *
 * Usage: grep [-i] <pattern> <file>
 *
 *   -i   case-insensitive matching
 *
 * Prints every line that contains <pattern>.
 * Reports the count of matching lines at the end.
 * ------------------------------------------------------------------------- */
void Cmd_Grep(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: grep [-i] <pattern> <file>");
        return;
    }

    /* ---- parse flags ---- */
    int ci = 0;                     /* case-insensitive flag */
    while (*args == '-') {
        args++;
        while (*args && *args != ' ') {
            if (*args == 'i') ci = 1;
            args++;
        }
        while (*args == ' ') args++;
    }

    /* ---- extract pattern ---- */
    char pattern[CMD_MAX_LINE];
    int pi = 0;
    while (*args && *args != ' ' && pi < CMD_MAX_LINE - 1)
        pattern[pi++] = *args++;
    pattern[pi] = '\0';
    while (*args == ' ') args++;

    if (!pattern[0] || !*args) {
        PRINT("Usage: grep [-i] <pattern> <file>");
        return;
    }

    /* ---- resolve file path ---- */
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

    /* ---- scan file line-by-line ---- */
    int nl       = cmd_slen(pattern);
    uint32_t pos = 0;
    uint32_t sz  = VFS_Size(&fh);
    int hits     = 0;

    while (pos < sz) {
        /* Read one line into buf */
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

        if (grep_match(pattern, nl, buf, col, ci)) {
            PRINT((char *)buf);
            hits++;
        }
    }

    VFS_Close(&fh);

    /* ---- summary ---- */
    char summary[CMD_MAX_LINE];
    cmd_uint_to_dec((uint32_t)hits, summary, CMD_MAX_LINE);
    cmd_scat(summary, hits == 1 ? " match" : " matches", CMD_MAX_LINE);
    PRINT(summary);
}
