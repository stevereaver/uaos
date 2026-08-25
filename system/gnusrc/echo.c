/* echo.c — GNU coreutils 'echo' for UAOS gnu: layer
 *
 * Display a line of text.
 *   echo [SHORT-OPTION]... [STRING]...
 *   echo LONG-OPTION
 * Options: -n (no trailing newline), -e (interpret escapes), -E (no escapes)
 * Note: This is the GNU echo for gnu: layer; the AmigaDOS echo remains in C:
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_n = 0;
static int opt_e = 0;

static void echo_escapes(const char *s)
{
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
                case 'n': put_c('\n'); break;
                case 't': put_c('\t'); break;
                case 'r': put_c('\r'); break;
                case '\\': put_c('\\'); break;
                case 'a': put_c('\a'); break;
                case 'b': put_c('\b'); break;
                case 'f': put_c('\f'); break;
                case 'v': put_c('\v'); break;
                case '0': s++; if (*s >= '0' && *s <= '7') {
                    int v = 0;
                    for (int i = 0; i < 3 && *s >= '0' && *s <= '7'; i++) {
                        v = v * 8 + (*s - '0'); s++;
                    }
                    s--;
                    put_c((char)v);
                } else { s--; put_c('\0'); } break;
                case 'c': return; /* stop output */
                default: put_c('\\'); put_c(*s); break;
            }
        } else {
            put_c(*s);
        }
        s++;
    }
}

int main(int argc, const char **argv)
{
    /* echo uses its own option parsing (not getopt) because options can
     * appear after strings in some implementations.  GNU echo parses
     * leading -n, -e, -E only. */
    int i = 1;
    int seen_n = 0, seen_e = 0, seen_E = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *p = argv[i] + 1;
        int valid = 1;
        while (*p) {
            if (*p == 'n') seen_n = 1;
            else if (*p == 'e') seen_e = 1;
            else if (*p == 'E') seen_E = 1;
            else { valid = 0; break; }
            p++;
        }
        if (!valid) break;
        i++;
    }
    opt_n = seen_n;
    opt_e = seen_e && !seen_E;

    int first = 1;
    for (; i < argc; i++) {
        if (!first) put_c(' ');
        first = 0;
        if (opt_e) echo_escapes(argv[i]);
        else put_s(argv[i]);
    }
    if (!opt_n) put_c('\n');
    return 0;
}
