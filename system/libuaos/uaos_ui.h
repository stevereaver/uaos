/* uaos_ui.h — UAOS userspace declarative-UI backend (UAOS-126)
 *
 * Userspace twin of the kernel's uibind: an application describes its
 * window as a UINode tree (uitree.h, shared with the kernel) and this
 * header renders/drives it over the uaos_gui_* syscall widget set —
 * no absolute coordinates in app code.
 *
 *   static uaos_ui_t ui;
 *   static UIArena arena;  static unsigned char abuf[8192];
 *   ui_arena_init(&arena, abuf, sizeof(abuf));
 *   UINode *root = ui_window(&arena, "Title", ui_vgroup(&arena, ..., NULL));
 *   uaos_ui_open(&ui, root, my_event_fn, NULL, -1, -1, 0, 0);
 *   while (uaos_ui_is_open(&ui)) uaos_ui_poll(&ui);
 *
 * Widgets come from uaos_gui.h (button/checkbox/radio/slider/string/
 * integer/label/listview).  UI_CYCLE, UI_LISTVIEW items, UI_PAGE tab
 * strips and UI_CUSTOM regions are drawn/handled by this layer on top,
 * mirroring the kernel backend's semantics.  Resize is detected by
 * polling uaos_gui_get_winsize and re-arranges the tree automatically.
 */

#ifndef UAOS_UI_H
#define UAOS_UI_H

#include "uitree.h"
#include "uiformat.h"
#include "uaos_gui.h"

#define UAOS_UI_MAX_GADS  32
#define UAOS_UI_MARGIN    10
#define UAOS_UI_CYCLE_AW  16    /* cycle arrow column width           */
#define UAOS_UI_ROW_H     18    /* listview row height                */
#define UAOS_UI_TAB_H     UI_PAGE_TAB_H

/* Events delivered to the app callback (mirror kernel UIEV_*).
 *   arg — UAOS_UIEV_KEY: the character. */
enum {
    UAOS_UIEV_CLICK = 1,   /* button activated                       */
    UAOS_UIEV_CHANGE,      /* gadget state changed (val/text/sel)    */
    UAOS_UIEV_KEY,         /* keystroke no gadget consumed           */
    UAOS_UIEV_RESIZE       /* client size changed; tree re-arranged  */
};

typedef struct uaos_ui uaos_ui_t;
typedef int (*uaos_ui_event_fn)(uaos_ui_t *ui, UINode *node, int ev, int arg);

struct uaos_ui {
    int             win;
    int             open;
    UINode         *root;
    uaos_ui_event_fn cb;
    void           *user;
    uaos_gui_t      gui;
    int             cw, ch;          /* last known client size         */
    int             ngads;
    UINode         *nodes[UAOS_UI_MAX_GADS];
    uaos_widget_t  *wids [UAOS_UI_MAX_GADS]; /* NULL for cycle/lv/custom */
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* 1 when n sits inside the visible subtree (every UI_PAGE ancestor shows
 * the child containing n). */
static inline int uaos_ui_visible(const UINode *n)
{
    for (const UINode *a = n->parent; a; a = a->parent) {
        if (a->type != UI_PAGE)
            continue;
        const UINode *d = n;
        while (d->parent != a)
            d = d->parent;
        int i = 0;
        for (const UINode *c = a->first_child; c; c = c->next_sibling, i++)
            if (c == d)
                break;
        if (i != a->u.page.active)
            return 0;
    }
    return 1;
}

static inline int uaos_ui_gad_index(uaos_ui_t *ui, const UINode *n)
{
    for (int i = 0; i < ui->ngads; i++)
        if (ui->nodes[i] == n)
            return i;
    return -1;
}

static inline int uaos_ui_gad_by_id(uaos_ui_t *ui, int id)
{
    for (int i = 0; i < ui->ngads; i++)
        if (ui->nodes[i]->id == id)
            return i;
    return -1;
}

/* Cycle gadget drawing (the uaos_gui widget set has no cycle kind). */
static inline void uaos_ui_draw_cycle(uaos_ui_t *ui, const UINode *n)
{
    int x = n->x, y = n->y, w = n->w, h = n->h;
    const char *v = "";
    if (n->u.items.items && n->u.items.active >= 0 &&
        n->u.items.active < n->u.items.count)
        v = n->u.items.items[n->u.items.active];
    uaos_gui_fill_rect(ui->win, x, y, w, h, UAOS_COL_LIGHT_GREY);
    uaos_gui_draw_bevel(ui->win, x, y, w, h, 1, UAOS_COL_GREY);
    uaos_gui_draw_text_bg(ui->win, x + 4, y + (h - 16) / 2, v,
                          UAOS_COL_BLACK, UAOS_COL_LIGHT_GREY);
    int ax = x + w - UAOS_UI_CYCLE_AW - 2;
    int ah = h / 2;
    uaos_gui_draw_bevel(ui->win, ax, y,      UAOS_UI_CYCLE_AW, ah,     1, UAOS_COL_GREY);
    uaos_gui_draw_text_bg(ui->win, ax + 4, y + (ah - 16) / 2, "^",
                          UAOS_COL_BLACK, UAOS_COL_LIGHT_GREY);
    uaos_gui_draw_bevel(ui->win, ax, y + ah, UAOS_UI_CYCLE_AW, h - ah, 1, UAOS_COL_GREY);
    uaos_gui_draw_text_bg(ui->win, ax + 4, y + ah + (h - ah - 16) / 2, "v",
                          UAOS_COL_BLACK, UAOS_COL_LIGHT_GREY);
}

/* Listview row overlay (the base widget draws only the frame). */
static inline void uaos_ui_draw_listview(uaos_ui_t *ui, const UINode *n)
{
    int rows = (n->h - 4) / UAOS_UI_ROW_H;
    for (int i = 0; i < rows && i < n->u.items.count; i++) {
        int ry = n->y + 2 + i * UAOS_UI_ROW_H;
        if (i == n->u.items.selected) {
            uaos_gui_fill_rect(ui->win, n->x + 1, ry, n->w - 2,
                               UAOS_UI_ROW_H, UAOS_COL_BLUE);
            uaos_gui_draw_text_bg(ui->win, n->x + 4, ry + 1,
                                  n->u.items.items[i],
                                  UAOS_COL_WHITE, UAOS_COL_BLUE);
        } else {
            uaos_gui_draw_text_bg(ui->win, n->x + 4, ry + 1,
                                  n->u.items.items[i],
                                  UAOS_COL_BLACK, UAOS_COL_WHITE);
        }
    }
}

/* UI_PAGE tab strips. */
static inline void uaos_ui_draw_tabs(uaos_ui_t *ui, UINode *n)
{
    for (; n; n = n->next_sibling) {
        if (n->type == UI_PAGE && uaos_ui_visible(n)) {
            int i = 0;
            for (UINode *c = n->first_child; c; c = c->next_sibling, i++) {
                int tx, ty, tw, th;
                if (!ui_page_tab_rect(n, i, &tx, &ty, &tw, &th))
                    continue;
                int act = (i == n->u.page.active);
                uaos_gui_fill_rect(ui->win, tx, ty, tw, th,
                                   act ? UAOS_COL_LIGHT_GREY : UAOS_COL_GREY);
                uaos_gui_draw_bevel(ui->win, tx, ty, tw, th, 1, UAOS_COL_GREY);
                if (c->tab)
                    uaos_gui_draw_text_bg(ui->win, tx + UI_TAB_PAD, ty + 3,
                                          c->tab, UAOS_COL_BLACK,
                                          act ? UAOS_COL_LIGHT_GREY
                                              : UAOS_COL_GREY);
            }
        }
        if (n->first_child)
            uaos_ui_draw_tabs(ui, n->first_child);
    }
}

static inline void uaos_ui_draw_customs(uaos_ui_t *ui, UINode *n)
{
    for (; n; n = n->next_sibling) {
        if (!uaos_ui_visible(n))
            continue;
        if (n->type == UI_CUSTOM && n->u.custom.draw)
            n->u.custom.draw(n, n->x, n->y, n->w, n->h,
                             n->u.custom.user ? n->u.custom.user : ui->user);
        if (n->first_child)
            uaos_ui_draw_customs(ui, n->first_child);
    }
}

/* Re-arrange the tree into the current client size and push node rects
 * into the widgets. */
static inline void uaos_ui_arrange(uaos_ui_t *ui)
{
    int w, h;
    if (uaos_gui_get_winsize(ui->win, &w, &h) != 0)
        return;
    ui->cw = w;
    ui->ch = h;
    ui_arrange(ui->root, UAOS_UI_MARGIN, UAOS_UI_MARGIN,
               w - UAOS_UI_MARGIN * 2, h - UAOS_UI_MARGIN * 2);
    for (int i = 0; i < ui->ngads; i++) {
        uaos_widget_t *wd = ui->wids[i];
        if (!wd) continue;
        wd->x = ui->nodes[i]->x;
        wd->y = ui->nodes[i]->y;
        wd->w = ui->nodes[i]->w;
        wd->h = ui->nodes[i]->h;
    }
}

static inline void uaos_ui_redraw(uaos_ui_t *ui)
{
    uaos_ui_arrange(ui);
    uaos_gui_fill_rect(ui->win, 0, 0, ui->cw, ui->ch, UAOS_COL_GREY);
    uaos_ui_draw_tabs(ui, ui->root);
    for (int i = 0; i < ui->ngads; i++) {
        UINode *n = ui->nodes[i];
        if (!uaos_ui_visible(n))
            continue;
        if (ui->wids[i])
            uaos_gui_draw_widget(&ui->gui, ui->wids[i]);
        else if (n->type == UI_CYCLE)
            uaos_ui_draw_cycle(ui, n);
    }
    /* listview item overlay after the frames */
    for (int i = 0; i < ui->ngads; i++) {
        UINode *n = ui->nodes[i];
        if (n->type == UI_LISTVIEW && uaos_ui_visible(n))
            uaos_ui_draw_listview(ui, n);
    }
    uaos_ui_draw_customs(ui, ui->root);
    uaos_gui_present(ui->win);
}

/* -------------------------------------------------------------------------
 * Tree → widget binding
 * ------------------------------------------------------------------------- */
static inline int uaos_ui_bind_walk(uaos_ui_t *ui, UINode *n)
{
    for (; n; n = n->next_sibling) {
        uaos_widget_t   *w  = NULL;
        uaos_newgadget_t ng;
        switch (n->type) {
        case UI_BUTTON:
        case UI_CHECKBOX:
        case UI_RADIO:
        case UI_SLIDER:
        case UI_STRING:
        case UI_INTEGER:
        case UI_LISTVIEW:
        case UI_LABEL:
        case UI_CYCLE: {
            if (ui->ngads >= UAOS_UI_MAX_GADS)
                return -1;
            int idx = ui->ngads++;
            ui->nodes[idx] = n;
            ui->wids[idx]  = NULL;
            if (n->type == UI_CYCLE)
                break;                    /* drawn by uaos_ui itself   */
            uaos_memset(&ng, 0, sizeof(ng));
            ng.id = n->id;
            ng.flags = 0;
            if (n->flags & UI_F_DISABLED) ng.flags |= UAOS_NG_DISABLED;
            if (n->flags & UI_F_READONLY) ng.flags |= UAOS_NG_READONLY;
            switch (n->type) {
            case UI_BUTTON:   ng.type = UAOS_GAD_BUTTON;   ng.text = n->u.text; break;
            case UI_CHECKBOX: ng.type = UAOS_GAD_CHECKBOX; ng.text = n->u.text; break;
            case UI_RADIO:    ng.type = UAOS_GAD_RADIO;    ng.text = n->u.text;
                              ng.group_id = n->group; break;
            case UI_SLIDER:
                ng.type = UAOS_GAD_SLIDER;
                ng.min_val = n->u.range.min;
                ng.max_val = n->u.range.max;
                ng.cur_val = n->u.range.cur;
                if (n->flags & UI_F_VERTICAL) ng.flags |= UAOS_NG_VSCROLL;
                break;
            case UI_STRING:   ng.type = UAOS_GAD_STRING;
                              ng.text = n->u.input.text;
                              ng.max_chars = n->u.input.max_chars; break;
            case UI_INTEGER:  ng.type = UAOS_GAD_INTEGER;
                              ng.min_val = n->u.input.min;
                              ng.max_val = n->u.input.max;
                              ng.cur_val = n->u.input.value; break;
            case UI_LISTVIEW: ng.type = UAOS_GAD_LISTVIEW; break;
            case UI_LABEL:    ng.type = UAOS_GAD_LABEL;    ng.text = n->u.text; break;
            }
            /* rect is filled in by uaos_ui_arrange before first draw */
            w = uaos_gui_create_gadget(&ui->gui, &ng);
            if (!w) return -1;
            if ((n->type == UI_CHECKBOX || n->type == UI_RADIO) &&
                (n->flags & UI_F_SELECTED))
                w->state = 1;
            if (n->type == UI_INTEGER) {
                /* seed text from the numeric value */
                char tmp[16];
                int pos = 0, v = n->u.input.value;
                char rev[16]; int r = 0;
                if (v < 0) { tmp[pos++] = '-'; v = -v; }
                if (v == 0) rev[r++] = '0';
                while (v > 0) { rev[r++] = '0' + (v % 10); v /= 10; }
                while (r > 0) tmp[pos++] = rev[--r];
                tmp[pos] = '\0';
                uaos_gui_set_str(&ui->gui, n->id, tmp);
            }
            ui->wids[idx] = w;
            break;
        }
        default:
            break;
        }
        if (n->first_child && uaos_ui_bind_walk(ui, n->first_child) < 0)
            return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Create the window and bind the tree.  x,y < 0 centre-ish (pass through
 * to the syscall); w,h <= 0 size the window to the tree's natural size
 * plus margins.  Returns 0 on success, -1 on failure. */
static inline int uaos_ui_open(uaos_ui_t *ui, UINode *root,
                               uaos_ui_event_fn cb, void *user,
                               int x, int y, int w, int h)
{
    if (!ui || !root)
        return -1;
    uaos_memset(ui, 0, sizeof(*ui));
    ui->root = root;
    ui->cb   = cb;
    ui->user = user;

    ui_measure(root);
    if (w <= 0) w = root->nat_w + UAOS_UI_MARGIN * 2;
    if (h <= 0) h = root->nat_h + UAOS_UI_MARGIN * 2;
    if (x < 0)  x = 60;
    if (y < 0)  y = 60;

    ui->win = (int)uaos_gui_create_window(
        root->u.text ? root->u.text : "UI", x, y, w, h);
    if (ui->win < 0)
        return -1;
    uaos_gui_init(&ui->gui, ui->win);

    if (uaos_ui_bind_walk(ui, root) < 0) {
        uaos_gui_destroy_window(ui->win);
        return -1;
    }
    ui->open = 1;
    uaos_ui_redraw(ui);
    return 0;
}

static inline void uaos_ui_close(uaos_ui_t *ui)
{
    if (!ui || !ui->open)
        return;
    ui->open = 0;
    uaos_gui_destroy_window(ui->win);
}

static inline int uaos_ui_is_open(const uaos_ui_t *ui)
{
    return ui && ui->open;
}

/* Live state access by node id. */
static inline int uaos_ui_val(uaos_ui_t *ui, int id)
{
    int i = uaos_ui_gad_by_id(ui, id);
    if (i < 0) return 0;
    UINode *n = ui->nodes[i];
    if (n->type == UI_CYCLE)    return n->u.items.active;
    if (n->type == UI_LISTVIEW) return n->u.items.selected;
    return uaos_gui_get_int(&ui->gui, id);
}

static inline const char *uaos_ui_text(uaos_ui_t *ui, int id)
{
    return uaos_gui_get_str(&ui->gui, id);
}

static inline void uaos_ui_set_val(uaos_ui_t *ui, int id, int v)
{
    int i = uaos_ui_gad_by_id(ui, id);
    if (i < 0) return;
    UINode *n = ui->nodes[i];
    if (n->type == UI_CYCLE) {
        if (v >= 0 && v < n->u.items.count)
            n->u.items.active = v;
    } else if (n->type == UI_LISTVIEW) {
        n->u.items.selected = v;
    } else {
        uaos_gui_set_int(&ui->gui, id, v);
    }
    uaos_ui_redraw(ui);
}

/* -------------------------------------------------------------------------
 * Event loop — call repeatedly while uaos_ui_is_open()
 * ------------------------------------------------------------------------- */

static inline int uaos_ui_poll(uaos_ui_t *ui)
{
    /* Resize check: re-arrange + redraw when the client size changed. */
    int w, h;
    if (uaos_gui_get_winsize(ui->win, &w, &h) == 0 &&
        (w != ui->cw || h != ui->ch)) {
        uaos_ui_redraw(ui);
        if (ui->cb)
            ui->cb(ui, NULL, UAOS_UIEV_RESIZE, 0);
    }

    struct uaos_gui_event ev;
    if (uaos_gui_get_event(ui->win, &ev) <= 0)
        return -1;

    if (ev.type == UAOS_GUI_EVENT_CLICK) {
        /* Nodes with no widget (cycle / listview / page tab strip) are
         * handled here; widget-bearing kinds go through
         * uaos_gui_handle_event (which also manages string defocus). */
        UINode *n = ui_node_at(ui->root, ev.x, ev.y);
        int gid = uaos_gui_handle_event(&ui->gui, &ev);

        if (n && n->type == UI_PAGE) {
            int i = 0;
            for (UINode *c = n->first_child; c; c = c->next_sibling, i++) {
                int tx, ty, tw, th;
                if (ui_page_tab_rect(n, i, &tx, &ty, &tw, &th) &&
                    ev.x >= tx && ev.x < tx + tw &&
                    ev.y >= ty && ev.y < ty + th &&
                    n->u.page.active != i) {
                    n->u.page.active = i;
                    uaos_ui_redraw(ui);
                    if (ui->cb) ui->cb(ui, n, UAOS_UIEV_CHANGE, 0);
                    return n->id;
                }
            }
        }
        if (n && n->type == UI_CYCLE) {
            int cnt = n->u.items.count;
            if (cnt > 0) {
                int ax = n->x + n->w - UAOS_UI_CYCLE_AW - 2;
                if (ev.x >= ax && ev.y >= n->y + n->h / 2)
                    n->u.items.active = (n->u.items.active - 1 + cnt) % cnt;
                else
                    n->u.items.active = (n->u.items.active + 1) % cnt;
                uaos_ui_redraw(ui);
                if (ui->cb) ui->cb(ui, n, UAOS_UIEV_CHANGE, 0);
                return n->id;
            }
        }
        if (n && n->type == UI_LISTVIEW) {
            int row = (ev.y - n->y - 2) / UAOS_UI_ROW_H;
            if (row >= 0 && row < n->u.items.count &&
                row != n->u.items.selected) {
                n->u.items.selected = row;
                uaos_ui_redraw(ui);
                if (ui->cb) ui->cb(ui, n, UAOS_UIEV_CHANGE, 0);
                return n->id;
            }
        }
        if (gid >= 0) {
            int i = uaos_ui_gad_by_id(ui, gid);
            UINode *gn = (i >= 0) ? ui->nodes[i] : NULL;
            uaos_ui_redraw(ui);
            if (ui->cb && gn)
                ui->cb(ui, gn,
                       (gn->type == UI_BUTTON) ? UAOS_UIEV_CLICK
                                               : UAOS_UIEV_CHANGE, 0);
            return gid;
        }
        if (n && n->type == UI_LABEL)
            return -1;             /* swallowed */
        return -1;
    }

    if (ev.type == UAOS_GUI_EVENT_KEY) {
        /* uaos_gui_handle_event routes every keystroke to the active
         * string gadget with no "consumed" feedback — ESC (and other
         * control keys the widget ignores) would be swallowed.  Match
         * the kernel backend: keys the field doesn't take (ESC)
         * go to the app as UAOS_UIEV_KEY instead. */
        if (ev.x == 27) {
            if (ui->cb)
                ui->cb(ui, NULL, UAOS_UIEV_KEY, ev.x);
            return -1;
        }
        int gid = uaos_gui_handle_event(&ui->gui, &ev);
        if (gid >= 0) {
            int i = uaos_ui_gad_by_id(ui, gid);
            uaos_ui_redraw(ui);
            if (ui->cb && i >= 0)
                ui->cb(ui, ui->nodes[i], UAOS_UIEV_CHANGE, 0);
            return gid;
        }
        if (ui->cb)
            ui->cb(ui, NULL, UAOS_UIEV_KEY, ev.x);
        return -1;
    }

    if (ev.type == UAOS_GUI_EVENT_MOVE || ev.type == UAOS_GUI_EVENT_RELEASE) {
        int gid = uaos_gui_handle_event(&ui->gui, &ev);
        if (gid >= 0) {
            int i = uaos_ui_gad_by_id(ui, gid);
            uaos_ui_redraw(ui);
            if (ui->cb && i >= 0)
                ui->cb(ui, ui->nodes[i], UAOS_UIEV_CHANGE, 0);
            return gid;
        }
        return -1;
    }

    return -1;
}

/* Standard prefs bottom row (identical to the kernel helper). */
static inline UINode *uaos_ui_prefs_row(UIArena *a,
                                        int save_id, int use_id,
                                        int cancel_id)
{
    return ui_hgroup(a,
        ui_spacer(a),
        ui_min(ui_button(a, "Save",   save_id),   80, 0),
        ui_min(ui_button(a, "Use",    use_id),    80, 0),
        ui_min(ui_button(a, "Cancel", cancel_id), 80, 0),
        ui_spacer(a),
        NULL);
}

/* -------------------------------------------------------------------------
 * .gui file loading (UAOS-127) — same convention as the kernel backend:
 * ENV:GUI/<tool>.gui first (runtime override), then
 * SYS:Prefs/GUI/<tool>.gui; returns `fallback` when neither parses.
 * ------------------------------------------------------------------------- */
#define UAOS_UI_GUI_MAX  16384

static inline UINode *uaos_ui_load(UIArena *arena, const char *tool,
                                   UINode *fallback)
{
    if (!arena || !tool)
        return fallback;

    static const char * const dirs[] = { "ENV:GUI/", "SYS:Prefs/GUI/" };
    uint32_t save = arena->used;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[96];
        int len = 0;
        for (const char *s = dirs[i]; *s && len < 90; s++) path[len++] = *s;
        for (const char *s = tool;   *s && len < 91; s++) path[len++] = *s;
        for (const char *s = ".gui"; *s && len < 95; s++) path[len++] = *s;
        path[len] = '\0';

        struct uaos_stat st;
        if (uaos_stat(path, &st) < 0 || !st.size || st.size >= UAOS_UI_GUI_MAX)
            continue;
        long fd = uaos_open(path, UAOS_O_RDONLY);
        if (fd < 0)
            continue;
        /* the parser carves strings in place — allocate the text buffer
         * from the arena so it lives as long as the tree */
        char *buf = (char *)ui_arena_alloc(arena, st.size + 1);
        if (!buf) { uaos_close((int)fd); continue; }
        long rd = uaos_read_file((int)fd, buf, st.size);
        uaos_close((int)fd);
        if (rd <= 0)
            continue;
        buf[rd] = '\0';

        UIParseErr err;
        UINode *root = ui_parse(arena, buf, &err);
        if (root)
            return root;
        arena->used = save;     /* rewind for the next candidate */
    }
    return fallback;
}

#endif /* UAOS_UI_H */
