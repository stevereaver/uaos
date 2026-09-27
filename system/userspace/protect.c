/* protect.c — UAOS x86-64 userspace 'protect' command
 *
 * AmigaDOS C:Protect — set file protection attributes.
 *   protect <file> [+|-][hsparwed] [ALL] [QUIET]
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

static int protect_bit(char c)
{
    switch (c) {
        case 'h': case 'H': return UAOS_FIBF_HOLD;
        case 's': case 'S': return UAOS_FIBF_SCRIPT;
        case 'p': case 'P': return UAOS_FIBF_PURE;
        case 'a': case 'A': return UAOS_FIBF_ARCHIVE;
        case 'r': case 'R': return UAOS_FIBF_READ;
        case 'w': case 'W': return UAOS_FIBF_WRITE;
        case 'e': case 'E': return UAOS_FIBF_EXECUTE;
        case 'd': case 'D': return UAOS_FIBF_DELETE;
    }
    return 0;
}

static void protect_one(const char *path,
                        uint16_t set_bits, uint16_t clear_bits, int quiet)
{
    long cur = uaos_getprotection(path);
    uint16_t current = (uint16_t)cur;
    uint16_t final = (uint16_t)((current & ~clear_bits) | set_bits);

    if (uaos_setprotection(path, final) == 0) {
        if (!quiet) { put_s("Protected: "); put_line(path); }
    } else {
        if (!quiet) { put_s("Failed: "); put_line(path); }
    }
}

static void protect_dir(const char *path,
                        uint16_t set_bits, uint16_t clear_bits,
                        int quiet, const char *pat)
{
    if (!pat || !pat[0]) protect_one(path, set_bits, clear_bits, quiet);

    long dd = uaos_opendir(path);
    if (dd < 0) return;
    struct uaos_dirent ent;
    while (uaos_readdir((int)dd, &ent) > 0) {
        if (ent.name[0] == '\0') continue;
        if (pat && pat[0] && !cmd_pattern_match(ent.name, pat)) continue;

        char sub[UAOS_CMD_PATH_MAX];
        cmd_join_path(path, ent.name, sub, sizeof(sub));

        if (ent.is_dir) {
            protect_dir(sub, set_bits, clear_bits, quiet, pat);
        } else {
            protect_one(sub, set_bits, clear_bits, quiet);
        }
    }
    uaos_closedir((int)dd);
}

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    /* Pre-scan for +/- flag groups before the template runs: the
     * positional FLAGS item would otherwise swallow tokens like "-d",
     * and a leading group ("protect -d FILE") would even land in FILE.
     * A token is a flag group only when every character after the first
     * sign is a flag letter or another sign — otherwise it stays as an
     * ordinary argument (e.g. a filename). */
    char clean[UAOS_TMPL_MAX_VAL];
    int ci = 0;
    uint16_t delta_set = 0, delta_clear = 0;
    {
        const char *p = args;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            const char *ts = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int tl = (int)(p - ts);
            int is_group = tl >= 2 && (ts[0] == '+' || ts[0] == '-');
            if (is_group) {
                for (int k = 1; k < tl; k++) {
                    if (ts[k] != '+' && ts[k] != '-' && !protect_bit(ts[k])) {
                        is_group = 0;
                        break;
                    }
                }
            }
            if (is_group) {
                char sign = '+';
                for (int k = 0; k < tl; k++) {
                    char c = ts[k];
                    if (c == '+' || c == '-') { sign = c; continue; }
                    int bit = protect_bit(c);
                    if (bit) {
                        if (sign == '+') delta_set   |= (uint16_t)bit;
                        else             delta_clear |= (uint16_t)bit;
                    }
                }
            } else {
                if (ci && ci < (int)sizeof(clean) - 1) clean[ci++] = ' ';
                for (int k = 0; k < tl && ci < (int)sizeof(clean) - 1; k++)
                    clean[ci++] = ts[k];
            }
        }
        clean[ci] = '\0';
    }

    UaosTmpl t;
    uaos_tmpl_run("FILE/A,FLAGS,ADD/S,SUB/S,ALL/S,QUIET/S", &t, clean);
    if (t.error[0]) { put_s("protect: "); put_line(t.error); return 20; }

    const char *file_arg = uaos_tmpl_string(&t, "FILE");
    if (!file_arg || !file_arg[0]) {
        put_line("Usage: protect <file> [+|-][hsparwed] [ALL] [ADD] [SUB] [QUIET]");
        put_line("       protect <file> FLAGS <hsparwed> [ADD|SUB] [ALL] [QUIET]");
        return 5;
    }

    int all   = uaos_tmpl_switch(&t, "ALL");
    int quiet = uaos_tmpl_switch(&t, "QUIET");
    int add   = uaos_tmpl_switch(&t, "ADD");
    int sub   = uaos_tmpl_switch(&t, "SUB");

    uint16_t set_bits = 0, clear_bits = 0;

    /* Check for a FLAGS argument (AmigaDOS 3.1 style).  A sign char
     * switches the remainder of the word into delta mode, matching the
     * pre-scanned +/- groups: + sets the bit (denies the operation),
     * - clears it (allows), per the inverted FIBF convention. */
    const char *flags_str = uaos_tmpl_string(&t, "FLAGS");
    if (flags_str && flags_str[0] &&
        (flags_str[0] == '+' || flags_str[0] == '-')) {
        char sign = '+';
        for (int i = 0; flags_str[i]; i++) {
            char c = flags_str[i];
            if (c == '+' || c == '-') { sign = c; continue; }
            int bit = protect_bit(c);
            if (bit) {
                if (sign == '+') set_bits   |= (uint16_t)bit;
                else             clear_bits |= (uint16_t)bit;
            }
        }
    } else if (flags_str && flags_str[0]) {
        /* FLAGS specifies the complete protection word.  With ADD, the
         * bits are OR'd in; with SUB, they're cleared.  Without either,
         * the protection is set to exactly these bits. */
        if (add) {
            for (int i = 0; flags_str[i]; i++) {
                int bit = protect_bit(flags_str[i]);
                if (bit) set_bits |= (uint16_t)bit;
            }
        } else if (sub) {
            for (int i = 0; flags_str[i]; i++) {
                int bit = protect_bit(flags_str[i]);
                if (bit) clear_bits |= (uint16_t)bit;
            }
        } else {
            /* Set protection to exactly these bits. */
            uint16_t new_prot = 0;
            for (int i = 0; flags_str[i]; i++) {
                int bit = protect_bit(flags_str[i]);
                if (bit) new_prot |= (uint16_t)bit;
            }
            /* Set all bits not in new_prot as "to clear", and all bits
             * in new_prot as "to set".  This effectively replaces the
             * protection word. */
            set_bits = new_prot;
            clear_bits = (uint16_t)~new_prot;
        }
    }

    /* Apply any +/- groups collected by the pre-scan; they override the
     * FLAGS result so `protect file rwed -w` clears w after the exact
     * set. */
    set_bits   |= delta_set;
    clear_bits |= delta_clear;

    char path[UAOS_CMD_PATH_MAX], pat[UAOS_CMD_PATH_MAX];
    cmd_split_path_pat(file_arg, path, pat);

    if (!path[0]) {
        put_line("Usage: protect <file> [+|-][hsparwed] [ALL] [ADD] [SUB] [QUIET]");
        return 5;
    }

    if (all || pat[0]) {
        protect_dir(path, set_bits, clear_bits, quiet, pat);
    } else {
        protect_one(path, set_bits, clear_bits, quiet);
    }
    return 0;
}
