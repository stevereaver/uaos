/* guide.c — AmigaGuide help viewer for UAOS
 *
 * Native x86-64 userspace binary.  Opens SYS:Documentation/uaos.guide and
 * renders it in a scrollable Workbench-style window using the GUI syscalls.
 */

#include "uaos_syscall.h"
#include "uaos_libc.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define WIN_W         640
#define WIN_H         480
#define CHAR_W        8
#define CHAR_H        16
#define MAX_NODES     32
#define MAX_BODY      (64 * 1024)
#define MAX_LINKS     128
#define MAX_LINES     512
#define MAX_LINE_LEN  128

/* Special keys from the PS/2 keyboard driver / virtual keys */
#define VKEY_PGUP  0x01
#define VKEY_PGDN  0x02
#define VKEY_UP    0x03
#define VKEY_DOWN  0x04

/* -------------------------------------------------------------------------
 * Parsed guide data
 * ------------------------------------------------------------------------- */
typedef struct {
    char target[32];
    int  is_system;  /* 1 = SYSTEM link (execute command), 0 = LINK (navigate) */
    int x, y, w, h;
} Link;

typedef struct {
    char name[32];
    char title[64];
    char prev[32];
    char next[32];
    char body[MAX_BODY];
    int body_len;
} Node;

static Node  g_nodes[MAX_NODES];
static int   g_node_count = 0;
static int   g_current_node = 0;
static Link  g_links[MAX_LINKS];
static int   g_link_count = 0;

/* Layout */
static int g_client_w = 0;
static int g_client_h = 0;
static int g_lines_total = 0;
static int g_scroll_y = 0;
static int g_content_h = 0;

static char g_file_buf[MAX_BODY];
static int  g_file_len = 0;

/* -------------------------------------------------------------------------
 * Minimal libc helpers
 * ------------------------------------------------------------------------- */
static int isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static void skip_ws(const char **p)
{
    while (**p && isspace(**p))
        (*p)++;
}

static int read_line(const char **p, char *out, int max)
{
    int n = 0;
    while (**p && **p != '\n' && n < max - 1) {
        out[n++] = **p;
        (*p)++;
    }
    out[n] = '\0';
    if (**p == '\n')
        (*p)++;
    return n;
}

/* -------------------------------------------------------------------------
 * AmigaGuide parser
 * ------------------------------------------------------------------------- */
static void parse_guide(void)
{
    const char *p = g_file_buf;
    const char *end = g_file_buf + g_file_len;
    Node *node = NULL;

    while (p < end) {
        char line[256];
        int len = read_line(&p, line, sizeof(line));
        if (len == 0)
            continue;

        const char *lp = line;
        skip_ws(&lp);

        if (starts_with(lp, "@NODE ")) {
            if (g_node_count >= MAX_NODES)
                continue;
            node = &g_nodes[g_node_count++];
            uaos_memset(node, 0, sizeof(Node));
            lp += 6;
            /* Read name up to space or quote */
            int i = 0;
            while (*lp && *lp != ' ' && *lp != '\t' && *lp != '"' && i < 31)
                node->name[i++] = *lp++;
            node->name[i] = '\0';
            /* Skip to quoted title */
            while (*lp && *lp != '"')
                lp++;
            if (*lp == '"')
                lp++;
            i = 0;
            while (*lp && *lp != '"' && i < 63)
                node->title[i++] = *lp++;
            node->title[i] = '\0';
        } else if (starts_with(lp, "@ENDNODE")) {
            node = NULL;
        } else if (node && starts_with(lp, "@PREV ")) {
            lp += 6;
            int i = 0;
            while (*lp && !isspace(*lp) && i < 31)
                node->prev[i++] = *lp++;
            node->prev[i] = '\0';
        } else if (node && starts_with(lp, "@NEXT ")) {
            lp += 6;
            int i = 0;
            while (*lp && !isspace(*lp) && i < 31)
                node->next[i++] = *lp++;
            node->next[i] = '\0';
        } else if (node && starts_with(lp, "@TITLE ")) {
            /* Overwrite title */
            lp += 7;
            while (*lp && *lp != '"')
                lp++;
            if (*lp == '"')
                lp++;
            int i = 0;
            while (*lp && *lp != '"' && i < 63)
                node->title[i++] = *lp++;
            node->title[i] = '\0';
        } else if (node) {
            /* Append body line, keeping formatting */
            if (node->body_len + len + 1 < MAX_BODY) {
                if (node->body_len > 0 && node->body[node->body_len - 1] != '\n')
                    node->body[node->body_len++] = '\n';
                for (int i = 0; i < len; i++)
                    node->body[node->body_len++] = line[i];
            }
        }
    }

    if (g_node_count == 0) {
        /* Fallback single node */
        node = &g_nodes[g_node_count++];
        uaos_memset(node, 0, sizeof(Node));
        uaos_strcpy(node->name, "MAIN");
        uaos_strcpy(node->title, "Guide");
        if (g_file_len < MAX_BODY) {
            uaos_memcpy(node->body, g_file_buf, g_file_len);
            node->body_len = g_file_len;
        }
    }
}

static int find_node(const char *name)
{
    if (!name || !name[0])
        return -1;
    for (int i = 0; i < g_node_count; i++) {
        if (uaos_strcmp(g_nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */
static void render_text_line(int win, int y, const char *s, uint32_t color)
{
    uaos_gui_draw_text(win, 4, y, s, color);
}

static int parse_link(const char *p, char *label, char *target, int *is_system, const char **end)
{
    /* @{"label" LINK target}  or  @{"label" SYSTEM command} */
    *is_system = 0;
    if (p[0] != '@' || p[1] != '{')
        return 0;
    p += 2;
    if (*p != '"')
        return 0;
    p++;
    int li = 0;
    while (*p && *p != '"' && li < 63) {
        label[li++] = *p;
        p++;
    }
    label[li] = '\0';
    if (*p != '"')
        return 0;
    p++;
    skip_ws(&p);
    if (starts_with(p, "LINK ")) {
        p += 5;
        skip_ws(&p);
        int ti = 0;
        while (*p && *p != '}' && !isspace(*p) && ti < 31) {
            target[ti++] = *p;
            p++;
        }
        target[ti] = '\0';
    } else if (starts_with(p, "SYSTEM ")) {
        p += 7;
        skip_ws(&p);
        *is_system = 1;
        int ti = 0;
        while (*p && *p != '}' && ti < 31) {
            target[ti++] = *p;
            p++;
        }
        target[ti] = '\0';
    } else {
        return 0;
    }
    while (*p && *p != '}')
        p++;
    if (*p == '}')
        p++;
    *end = p;
    return 1;
}

static int strip_format(const char *src, char *dst, int max)
{
    int i = 0, j = 0;
    while (src[i] && j < max - 1) {
        if (src[i] == '@' && src[i + 1] == '{') {
            const char *p = src + i + 2;
            if (starts_with(p, "b}")) {
                i += 4;
                continue;
            } else if (starts_with(p, "ub}")) {
                i += 5;
                continue;
            } else if (starts_with(p, "i}")) {
                i += 4;
                continue;
            } else if (starts_with(p, "ui}")) {
                i += 5;
                continue;
            }
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
    return j;
}

static int wrap_line(const char *src, int max_chars, char *out, int out_max, const char **end)
{
    int len = 0;
    while (src[len] && src[len] != '\n')
        len++;

    if (len <= max_chars) {
        int j = 0;
        for (int i = 0; i < len && j < out_max - 1; i++)
            out[j++] = src[i];
        out[j] = '\0';
        *end = src[len] == '\n' ? src + len + 1 : src + len;
        return j;
    }

    /* Find break point */
    int br = max_chars;
    while (br > 0 && src[br] != ' ')
        br--;
    if (br == 0)
        br = max_chars;
    int j = 0;
    for (int i = 0; i < br && j < out_max - 1; i++)
        out[j++] = src[i];
    out[j] = '\0';
    *end = src + br;
    while (**end == ' ')
        (*end)++;
    if (**end == '\n')
        (*end)++;
    return j;
}

static void render_node(int win, int node_idx)
{
    const Node *n = &g_nodes[node_idx];

    /* Clear buffer */
    uaos_gui_draw_rect(win, 0, 0, g_client_w, g_client_h, UAOS_WB_WHITE);

    /* Title bar inside client area */
    char title_line[128];
    int ti = 0;
    while (n->title[ti] && ti < sizeof(title_line) - 1) {
        title_line[ti] = n->title[ti];
        ti++;
    }
    title_line[ti] = '\0';
    uaos_gui_draw_rect(win, 0, 0, g_client_w, CHAR_H + 4, UAOS_WB_LIGHT_BLUE);
    uaos_gui_draw_text(win, 4, 2, title_line, UAOS_WB_WHITE);

    g_link_count = 0;
    g_lines_total = 0;

    int y = CHAR_H + 12 - g_scroll_y;
    int max_chars = (g_client_w - 16) / CHAR_W;
    const char *p = n->body;

    while (*p && y < g_client_h + g_scroll_y + 200) {
        if (*p == '\n') {
            p++;
            continue;
        }

        char raw[256];
        int raw_len = 0;
        while (*p && *p != '\n' && raw_len < sizeof(raw) - 1)
            raw[raw_len++] = *p++;
        raw[raw_len] = '\0';
        if (*p == '\n')
            p++;

        /* Wrap line into display segments */
        const char *seg = raw;
        while (*seg) {
            char line[MAX_LINE_LEN];
            wrap_line(seg, max_chars, line, sizeof(line), &seg);

            /* Render line segment, scanning for links */
            int cx = 4;
            int col_off = 0;
            char txt[MAX_LINE_LEN];
            while (line[col_off]) {
                /* Check for link */
                char label[64], target[32];
                int is_sys = 0;
                const char *endp;
                if (line[col_off] == '@' && parse_link(&line[col_off], label, target, &is_sys, &endp)) {
                    /* Draw any pending text before link */
                    if (col_off > 0) {
                        int k = 0;
                        while (k < col_off && k < sizeof(txt) - 1) {
                            txt[k] = line[k];
                            k++;
                        }
                        txt[k] = '\0';
                        if (txt[0])
                            uaos_gui_draw_text(win, cx, y, txt, UAOS_WB_BLACK);
                        cx += uaos_strlen(txt) * CHAR_W;
                    }

                    /* Draw link label */
                    uaos_gui_draw_text(win, cx, y, label, UAOS_WB_BLUE);

                    /* Record link hitbox */
                    if (g_link_count < MAX_LINKS) {
                        Link *lk = &g_links[g_link_count++];
                        uaos_strcpy(lk->target, target);
                        lk->is_system = is_sys;
                        lk->x = cx;
                        lk->y = y;
                        lk->w = uaos_strlen(label) * CHAR_W;
                        lk->h = CHAR_H;
                    }

                    cx += uaos_strlen(label) * CHAR_W;
                    int consumed = (int)(endp - &line[col_off]);
                    /* Shift remaining line left */
                    int rest = 0;
                    while (line[col_off + consumed + rest])
                        rest++;
                    for (int m = 0; m <= rest; m++)
                        line[m] = line[col_off + consumed + m];
                    col_off = 0;
                } else {
                    col_off++;
                }
            }

            /* Draw remaining text */
            if (line[0]) {
                uaos_gui_draw_text(win, cx, y, line, UAOS_WB_BLACK);
            }

            y += CHAR_H;
            g_lines_total++;
        }
    }

    g_content_h = (CHAR_H + 12) + (g_lines_total + 1) * CHAR_H;
    if (g_content_h < g_client_h)
        g_content_h = g_client_h;
}

/* -------------------------------------------------------------------------
 * Event handling
 * ------------------------------------------------------------------------- */
static void goto_node(int win, const char *name)
{
    int idx = find_node(name);
    if (idx >= 0) {
        g_current_node = idx;
        g_scroll_y = 0;
        uaos_gui_set_scroll(win, 0, 0);
        uaos_gui_set_scroll_info(win, g_client_w, g_content_h);
    }
}

static void handle_click(int win, int mx, int my)
{
    for (int i = 0; i < g_link_count; i++) {
        Link *lk = &g_links[i];
        if (mx >= lk->x && mx < lk->x + lk->w &&
            my + g_scroll_y >= lk->y && my + g_scroll_y < lk->y + lk->h) {
            if (lk->is_system) {
                /* SYSTEM link: display command (can't execute from userspace) */
                char msg[80];
                uaos_strcpy(msg, "System: ");
                int ml = 8;
                int ti = 0;
                while (lk->target[ti] && ml < 79) { msg[ml++] = lk->target[ti++]; }
                msg[ml] = '\0';
                uaos_gui_draw_text(win, 4, 2, msg, UAOS_WB_BLUE);
            } else {
                goto_node(win, lk->target);
            }
            return;
        }
    }
}

static void clamp_scroll(int win)
{
    if (g_scroll_y < 0)
        g_scroll_y = 0;
    if (g_scroll_y > g_content_h - g_client_h)
        g_scroll_y = g_content_h - g_client_h;
    if (g_scroll_y < 0)
        g_scroll_y = 0;
    uaos_gui_set_scroll(win, 0, g_scroll_y);
}

static void scroll_by(int win, int dy)
{
    g_scroll_y += dy;
    clamp_scroll(win);
}

/* -------------------------------------------------------------------------
 * Main entry
 * ------------------------------------------------------------------------- */
int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    int fd = uaos_open("SYS:Documentation/uaos.guide", UAOS_O_RDONLY);
    if (fd < 0) {
        /* Try a relative fallback */
        fd = uaos_open("SYS:Tools/uaos.guide", UAOS_O_RDONLY);
    }
    if (fd < 0) {
        fd = uaos_open("uaos.guide", UAOS_O_RDONLY);
    }
    if (fd < 0) {
        uaos_write(1, "Guide: cannot open uaos.guide\n", 30);
        return 1;
    }

    g_file_len = 0;
    while (g_file_len < MAX_BODY - 1) {
        int n = uaos_read_file(fd, g_file_buf + g_file_len, MAX_BODY - 1 - g_file_len);
        if (n <= 0)
            break;
        g_file_len += n;
    }
    g_file_buf[g_file_len] = '\0';
    uaos_close(fd);

    parse_guide();

    g_client_w = WIN_W - 1 - 16;  /* left border + right scrollbar */
    g_client_h = WIN_H - 20 - 16; /* title bar + bottom scrollbar */

    const char *title = "UAOS Help";
    int win = (int)uaos_gui_create_window(title, 100, 60, WIN_W, WIN_H);
    if (win < 0) {
        uaos_write(1, "Guide: cannot create window\n", 28);
        return 1;
    }

    uaos_gui_set_scroll_info(win, g_client_w, g_client_h);
    g_current_node = 0;
    g_scroll_y = 0;

    for (;;) {
        render_node(win, g_current_node);
        uaos_gui_set_scroll_info(win, g_client_w, g_content_h);
        uaos_gui_present(win);

        struct uaos_gui_event ev;
        int rc = uaos_gui_get_event(win, &ev);
        if (rc == 0) {
            uaos_yield();
            continue;
        }

        if (ev.type == UAOS_GUI_EVENT_KEY) {
            unsigned char c = (unsigned char)ev.x;
            if (c == 'q' || c == 'Q' || c == 27)
                break;
            if (c == VKEY_UP || c == VKEY_PGUP)
                scroll_by(win, -CHAR_H * 4);
            if (c == VKEY_DOWN || c == VKEY_PGDN)
                scroll_by(win, CHAR_H * 4);
            if (c == 'h' || c == 'H')
                goto_node(win, "MAIN");
            if (c == 'b' || c == 'B') {
                const char *prev = g_nodes[g_current_node].prev;
                if (prev[0])
                    goto_node(win, prev);
            }
            if (c == 'n' || c == 'N') {
                const char *next = g_nodes[g_current_node].next;
                if (next[0])
                    goto_node(win, next);
            }
        } else if (ev.type == UAOS_GUI_EVENT_CLICK) {
            handle_click(win, ev.x, ev.y);
        } else if (ev.type == UAOS_GUI_EVENT_SCROLL) {
            g_scroll_y = ev.y;
            clamp_scroll(win);
        }
    }

    uaos_gui_destroy_window(win);
    return 0;
}
