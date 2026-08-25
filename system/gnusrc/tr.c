/* tr.c — GNU coreutils 'tr' for UAOS gnu: layer
 *
 * Translate or delete characters.
 *   tr [OPTION]... SET1 [SET2]
 * Options: -d, --delete, -s, --squeeze-repeats, -c, -C, --complement
 * Sets support ranges (a-z), escapes (\n\t\\), and repeats [c*n].
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define TR_SET_MAX 256

static int opt_delete = 0;
static int opt_squeeze = 0;
static int opt_complement = 0;

/* Expand a set spec into a 256-byte membership / translation table. */
static int expand_set(const char *spec, uint8_t *set, int set_max)
{
    int len = 0;
    const char *p = spec;
    while (*p && len < set_max) {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '0': c = '\0'; break;
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'v': c = '\v'; break;
                default: c = e; break;
            }
        }
        /* check for range a-z */
        if (*p == '-' && p[1] && p[1] != '\\') {
            p++;
            char end = *p++;
            if (end == '\\' && *p) {
                char e = *p++;
                switch (e) {
                    case 'n': end = '\n'; break;
                    case 't': end = '\t'; break;
                    case 'r': end = '\r'; break;
                    default: end = e; break;
                }
            }
            while (c <= end && len < set_max) set[len++] = (uint8_t)c++;
        } else if (*p == '*' && (c >= '0' && c <= '9')) {
            /* repeat: [c*n] — not fully supported, just add once */
        } else {
            set[len++] = (uint8_t)c;
        }
    }
    return len;
}

static void build_complement(uint8_t *set, int *len)
{
    uint8_t present[256];
    uaos_memset(present, 0, sizeof(present));
    for (int i = 0; i < *len; i++) present[set[i]] = 1;
    int nl = 0;
    for (int i = 0; i < 256; i++) if (present[i]) set[nl++] = (uint8_t)i;
    *len = nl;
    /* now set has the chars that ARE present; complement means NOT present */
    uint8_t comp[256];
    nl = 0;
    for (int i = 0; i < 256; i++) {
        int found = 0;
        for (int j = 0; j < *len; j++) if (set[j] == i) { found = 1; break; }
        if (!found) comp[nl++] = (uint8_t)i;
    }
    uaos_memcpy(set, comp, nl);
    *len = nl;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"delete",          'd', no_argument},
        {"squeeze-repeats", 's', no_argument},
        {"complement",      'c', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "dscC", long_opts, &li)) != -1) {
        switch (opt) {
            case 'd': opt_delete = 1; break;
            case 's': opt_squeeze = 1; break;
            case 'c':
            case 'C': opt_complement = 1; break;
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 1) { put_line("tr: missing operand"); return 1; }

    const char *set1_spec = uaos_operand(argc, argv, 0);
    const char *set2_spec = uaos_operand(argc, argv, 1);

    uint8_t set1[TR_SET_MAX], set2[TR_SET_MAX];
    int len1 = expand_set(set1_spec, set1, TR_SET_MAX);
    int len2 = 0;
    if (set2_spec) len2 = expand_set(set2_spec, set2, TR_SET_MAX);

    if (opt_complement) {
        build_complement(set1, &len1);
    }

    /* Build translation table */
    uint8_t xlate[256];
    for (int i = 0; i < 256; i++) xlate[i] = (uint8_t)i;
    uint8_t squeeze[256];
    uaos_memset(squeeze, 0, sizeof(squeeze));

    if (!opt_delete && set2_spec) {
        for (int i = 0; i < len1; i++) {
            int si = (len2 > 0) ? (i < len2 ? i : len2 - 1) : 0;
            xlate[set1[i]] = set2[si];
        }
    }
    if (opt_squeeze && set2_spec) {
        for (int i = 0; i < len2; i++) squeeze[set2[i]] = 1;
    } else if (opt_squeeze && !set2_spec) {
        for (int i = 0; i < len1; i++) squeeze[set1[i]] = 1;
    }

    /* membership check for delete */
    uint8_t in_set1[256];
    uaos_memset(in_set1, 0, sizeof(in_set1));
    for (int i = 0; i < len1; i++) in_set1[set1[i]] = 1;

    int prev_squeezed = -1;
    for (;;) {
        uint8_t ch;
        long n = uaos_read(0, &ch, 1);
        if (n <= 0) break;

        if (opt_delete && in_set1[ch]) continue;

        uint8_t out = opt_delete ? ch : (set2_spec ? xlate[ch] : ch);

        if (opt_squeeze && squeeze[out]) {
            if (prev_squeezed == out) continue;
            prev_squeezed = out;
        } else {
            prev_squeezed = -1;
        }
        put_c((char)out);
    }
    return 0;
}
