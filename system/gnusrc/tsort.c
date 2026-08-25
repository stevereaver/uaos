/* tsort.c — GNU coreutils 'tsort' for UAOS gnu: layer
 *
 * Topological sort of a directed graph from pairs of strings.
 *   tsort [FILE]
 * Reads whitespace-separated pairs; each pair (a b) means a must come before b.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define TSORT_MAX_NODES 512
#define TSORT_NAME_MAX 32

static char nodes[TSORT_MAX_NODES][TSORT_NAME_MAX];
static int  node_count = 0;
static uint8_t adj[TSORT_MAX_NODES][TSORT_MAX_NODES / 8 + 1]; /* bit matrix */
static int  indeg[TSORT_MAX_NODES];

static int find_or_add(const char *name)
{
    for (int i = 0; i < node_count; i++) {
        if (uaos_strcmp(nodes[i], name) == 0) return i;
    }
    if (node_count >= TSORT_MAX_NODES) return -1;
    uaos_strcpy(nodes[node_count], name);
    uaos_memset(adj[node_count], 0, sizeof(adj[0]));
    indeg[node_count] = 0;
    return node_count++;
}

static int bit_get(int r, int c)
{
    return (adj[r][c / 8] >> (c % 8)) & 1;
}

static void bit_set(int r, int c)
{
    adj[r][c / 8] |= (uint8_t)(1 << (c % 8));
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = { {NULL, 0, 0} };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        return 1;
    }

    int nfiles = uaos_operands_count(argc);
    int fd = 0;
    uint32_t sz = 0;
    int is_stdin = 1;
    if (nfiles >= 1) {
        const char *fname = uaos_operand(argc, argv, 0);
        if (!(fname[0] == '-' && fname[1] == '\0')) {
            char path[UAOS_CMD_PATH_MAX];
            cmd_make_abs(fname, path, sizeof(path));
            fd = (int)uaos_open(path, UAOS_O_RDONLY);
            if (fd < 0) { put_s("tsort: "); put_s(fname); put_line(": No such file"); return 1; }
            struct uaos_stat st; if (uaos_stat(path, &st) == 0) sz = st.size;
            is_stdin = 0;
        }
    }

    /* Read tokens, process pairs */
    char tok[TSORT_NAME_MAX];
    int tlen = 0;
    int pair_first = -1;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch;
        long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;

        if (uaos_isspace(ch)) {
            if (tlen > 0) {
                tok[tlen] = '\0';
                int idx = find_or_add(tok);
                if (idx >= 0) {
                    if (pair_first < 0) {
                        pair_first = idx;
                    } else {
                        if (pair_first != idx && !bit_get(pair_first, idx)) {
                            bit_set(pair_first, idx);
                            indeg[idx]++;
                        }
                        pair_first = -1;
                    }
                }
                tlen = 0;
            }
        } else {
            if (tlen < TSORT_NAME_MAX - 1) tok[tlen++] = (char)ch;
        }
    }
    if (tlen > 0) {
        tok[tlen] = '\0';
        int idx = find_or_add(tok);
        if (idx >= 0 && pair_first >= 0) {
            if (pair_first != idx && !bit_get(pair_first, idx)) {
                bit_set(pair_first, idx);
                indeg[idx]++;
            }
        }
    }
    if (fd > 0) uaos_close(fd);

    /* Kahn's algorithm */
    int visited = 0;
    while (visited < node_count) {
        int found = -1;
        for (int i = 0; i < node_count; i++) {
            if (indeg[i] == 0) { found = i; break; }
        }
        if (found < 0) {
            put_line("tsort: input contains a cycle");
            return 1;
        }
        indeg[found] = -1; /* mark visited */
        put_s(nodes[found]);
        put_c('\n');
        visited++;
        for (int j = 0; j < node_count; j++) {
            if (bit_get(found, j)) {
                indeg[j]--;
            }
        }
    }
    return 0;
}
