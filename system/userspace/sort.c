/* sort.c — UAOS x86-64 userspace 'sort' command
 *
 * AmigaDOS C:Sort — sort lines of a file.
 *   sort <file> [COL <n>] [CASE] [NUMERIC]
 */

#include "uaos_cmd.h"
#include "uaos_template.h"

#define MAX_SORT_LINES 256
#define MAX_SORT_LINE  UAOS_CMD_LINE_MAX

static int sort_col_start = 0;

static int sort_extract_col(const char *line, int col, char *out, int max)
{
    int i = 0, c = 0;
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
    int na = 0, nb = 0, ha = 0, hb = 0;
    const char *pa = a, *pb = b;
    while (*pa && (*pa < '0' || *pa > '9')) pa++;
    while (*pb && (*pb < '0' || *pb > '9')) pb++;
    while (*pa >= '0' && *pa <= '9') { na = na * 10 + (*pa - '0'); ha = 1; pa++; }
    while (*pb >= '0' && *pb <= '9') { nb = nb * 10 + (*pb - '0'); hb = 1; pb++; }
    if (ha && hb && na != nb) return na - nb;
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

int main(int argc, const char **argv)
{
    char args[UAOS_TMPL_MAX_VAL];
    cmd_build_args(argc, argv, args, sizeof(args));

    UaosTmpl t;
    uaos_tmpl_run("FROM/A,TO/A,COLSTART/K/N,CASE/S,NUMERIC/S", &t, args);
    if (t.error[0]) { put_s("sort: "); put_line(t.error); return 20; }

    const char *file_arg = uaos_tmpl_string(&t, "FROM");
    const char *to_arg   = uaos_tmpl_string(&t, "TO");
    int case_sens = uaos_tmpl_switch(&t, "CASE");
    int numeric   = uaos_tmpl_switch(&t, "NUMERIC");
    int col = 0;
    int cval = 0;
    if (uaos_tmpl_int(&t, "COLSTART", &cval)) col = cval;
    sort_col_start = col;

    if (!file_arg || !file_arg[0]) {
        put_line("Usage: sort <from> <to> [COLSTART <n>] [CASE] [NUMERIC]");
        return 5;
    }
    if (!to_arg || !to_arg[0]) {
        put_line("Usage: sort <from> <to> [COLSTART <n>] [CASE] [NUMERIC]");
        return 5;
    }

    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(file_arg, path, sizeof(path));

    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) { put_s("Cannot open: "); put_line(path); return 5; }

    struct uaos_stat st;
    uint32_t sz = 0;
    if (uaos_stat(path, &st) == 0) sz = st.size;

    /* Allocate the line buffer from the userspace heap. */
    char (*sbuf)[MAX_SORT_LINE] = (char (*)[MAX_SORT_LINE])
        uaos_alloc((long)(MAX_SORT_LINES * MAX_SORT_LINE));
    if (!sbuf) { put_line("sort: out of memory"); uaos_close((int)fd); return 5; }

    int count = 0;
    uint32_t pos = 0;
    while (pos < sz && count < MAX_SORT_LINES) {
        int c = 0;
        while (pos < sz && c < MAX_SORT_LINE - 1) {
            uint8_t ch;
            if (uaos_read_file((int)fd, &ch, 1) == 0) break;
            pos++;
            if (ch == '\n') break;
            if (ch != '\r') sbuf[count][c++] = (char)ch;
        }
        sbuf[count][c] = '\0';
        count++;
    }
    uaos_close((int)fd);

    /* Bubble sort */
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - 1 - i; j++) {
            char key_a[MAX_SORT_LINE], key_b[MAX_SORT_LINE];
            if (sort_col_start > 0) {
                sort_extract_col(sbuf[j],     sort_col_start, key_a, MAX_SORT_LINE);
                sort_extract_col(sbuf[j + 1], sort_col_start, key_b, MAX_SORT_LINE);
            } else {
                uaos_strcpy(key_a, sbuf[j]);
                uaos_strcpy(key_b, sbuf[j + 1]);
            }
            if (sort_cmp(key_a, key_b, numeric, case_sens) > 0) {
                char tmp[MAX_SORT_LINE];
                uaos_strcpy(tmp, sbuf[j]);
                uaos_strcpy(sbuf[j], sbuf[j + 1]);
                uaos_strcpy(sbuf[j + 1], tmp);
            }
        }
    }

    /* Output to TO file (AmigaDOS 3.1 requires a destination). */
    char to_path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(to_arg, to_path, sizeof(to_path));
    long out_fd = uaos_open(to_path, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
    if (out_fd < 0) { put_s("Cannot create: "); put_line(to_path); return 5; }

    for (int i = 0; i < count; i++) {
        uaos_write_file((int)out_fd, (const uint8_t *)sbuf[i],
                        (uint32_t)uaos_strlen(sbuf[i]));
        uaos_write_file((int)out_fd, (const uint8_t *)"\n", 1);
    }
    uaos_close((int)out_fd);

    char msg[UAOS_CMD_LINE_MAX];
    msg[0] = '\0';
    uaos_strlcat(msg, "Sorted ", sizeof(msg));
    char num[8];
    uint_to_dec((uint32_t)count, num, sizeof(num));
    uaos_strlcat(msg, num, sizeof(msg));
    uaos_strlcat(msg, " lines into ", sizeof(msg));
    uaos_strlcat(msg, to_arg, sizeof(msg));
    put_line(msg);
    return 0;
}
