/* ptx.c — GNU coreutils 'ptx' for UAOS gnu: layer
 *
 * Produce a permuted index of file contents.
 *   ptx [OPTION]... [INPUT [OUTPUT]]
 * Options: -G, --traditional, -A, --auto-reference, -r, --references,
 *          -w N, --width=N, -g N, --gap-size=N, -O, --format=roff
 *
 * Simplified implementation: produces a basic KWIC (Key-Word-In-Context) index.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

#define PTX_MAX_LINES 512
#define PTX_LINE_MAX  (UAOS_CMD_LINE_MAX * 2)
#define PTX_MAX_WORDS 64
#define PTX_WORD_MAX  32

static int opt_width = 72;
static int opt_traditional = 0;

typedef struct {
    char word[PTX_WORD_MAX];
    int  word_pos;     /* position in line (word index) */
    int  char_pos;     /* character position in line */
    int  line_idx;     /* which line this came from */
} PtxWord;

static PtxWord g_words[PTX_MAX_LINES * PTX_MAX_WORDS];
static int g_word_count = 0;
static char g_lines[PTX_MAX_LINES][PTX_LINE_MAX];
static int g_line_count = 0;

static int ptx_word_cmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i], bc = b[i];
        if (ac >= 'A' && ac <= 'Z') ac += 32;
        if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return ac - bc;
        i++;
    }
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static void ptx_sort_words(void)
{
    /* sort by word (case-insensitive), then by line */
    for (int i = 0; i < g_word_count - 1; i++) {
        for (int j = 0; j < g_word_count - 1 - i; j++) {
            int c = ptx_word_cmp(g_words[j].word, g_words[j + 1].word);
            if (c > 0 || (c == 0 && g_words[j].line_idx > g_words[j + 1].line_idx)) {
                PtxWord tmp = g_words[j];
                g_words[j] = g_words[j + 1];
                g_words[j + 1] = tmp;
            }
        }
    }
}

static void ptx_fd(int fd, uint32_t sz, int is_stdin)
{
    int col = 0;
    uint32_t pos = 0;
    for (;;) {
        uint8_t ch; long n;
        if (is_stdin) { n = uaos_read(fd, &ch, 1); }
        else { if (pos >= sz) break; n = uaos_read_file(fd, &ch, 1); pos++; }
        if (n <= 0) break;
        if (ch == '\n') {
            if (g_line_count < PTX_MAX_LINES) {
                g_lines[g_line_count][col] = '\0';
                /* extract words */
                int i = 0, word_start = -1, word_idx = 0;
                while (g_lines[g_line_count][i]) {
                    char c = g_lines[g_line_count][i];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9')) {
                        if (word_start < 0) word_start = i;
                    } else {
                        if (word_start >= 0 && g_word_count < (int)(sizeof(g_words)/sizeof(g_words[0]))) {
                            int wl = i - word_start;
                            if (wl >= PTX_WORD_MAX) wl = PTX_WORD_MAX - 1;
                            uaos_memcpy(g_words[g_word_count].word, g_lines[g_line_count] + word_start, wl);
                            g_words[g_word_count].word[wl] = '\0';
                            g_words[g_word_count].word_pos = word_idx;
                            g_words[g_word_count].char_pos = word_start;
                            g_words[g_word_count].line_idx = g_line_count;
                            g_word_count++;
                            word_idx++;
                        }
                        word_start = -1;
                    }
                    i++;
                }
                if (word_start >= 0 && g_word_count < (int)(sizeof(g_words)/sizeof(g_words[0]))) {
                    int wl = i - word_start;
                    if (wl >= PTX_WORD_MAX) wl = PTX_WORD_MAX - 1;
                    uaos_memcpy(g_words[g_word_count].word, g_lines[g_line_count] + word_start, wl);
                    g_words[g_word_count].word[wl] = '\0';
                    g_words[g_word_count].word_pos = word_idx;
                    g_words[g_word_count].char_pos = word_start;
                    g_words[g_word_count].line_idx = g_line_count;
                    g_word_count++;
                }
                g_line_count++;
            }
            col = 0;
        } else if (ch != '\r' && col < PTX_LINE_MAX - 1) {
            g_lines[g_line_count][col++] = (char)ch;
        }
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"traditional", 'G', no_argument},
        {"width",       'w', required_argument},
        {"auto-reference", 'A', no_argument},
        {"references",  'r', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "Gw:Ar", long_opts, &li)) != -1) {
        switch (opt) {
            case 'G': opt_traditional = 1; break;
            case 'w': { long v; if (uaos_optarg_long(&v) && v > 0) opt_width = (int)v; } break;
            default:  break;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops == 0) {
        ptx_fd(0, 0, 1);
    } else {
        for (int i = 0; i < nops; i++) {
            const char *fname = uaos_operand(argc, argv, i);
            if (!fname) continue;
            if (fname[0] == '-' && fname[1] == '\0') { ptx_fd(0, 0, 1); continue; }
            char path[UAOS_CMD_PATH_MAX]; cmd_make_abs(fname, path, sizeof(path));
            long fd = uaos_open(path, UAOS_O_RDONLY);
            if (fd < 0) { put_s("ptx: "); put_s(fname); put_line(": No such file"); continue; }
            struct uaos_stat st; uint32_t sz = 0;
            if (uaos_stat(path, &st) == 0) sz = st.size;
            ptx_fd((int)fd, sz, 0);
            uaos_close((int)fd);
        }
    }

    ptx_sort_words();

    /* Output KWIC index */
    int half = opt_width / 2;
    for (int i = 0; i < g_word_count; i++) {
        const char *line = g_lines[g_words[i].line_idx];
        int cpos = g_words[i].char_pos;
        int linelen = (int)uaos_strlen(line);

        /* left context */
        int left_start = cpos - half;
        if (left_start < 0) left_start = 0;
        int left_len = cpos - left_start;

        /* right context */
        int right_end = cpos + half;
        if (right_end > linelen) right_end = linelen;

        /* print left-justified left context */
        for (int p = 0; p < half - left_len; p++) put_c(' ');
        for (int p = left_start; p < cpos; p++) put_c(line[p]);
        put_c('|');
        for (int p = cpos; p < right_end; p++) put_c(line[p]);
        put_c('\n');
    }
    return 0;
}
