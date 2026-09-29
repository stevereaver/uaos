/* uibind.c — kernel backend: bind a UINode tree to a WM window
 *
 * See uibind.h for the model.  Draw/click/move/release/key callbacks
 * registered with the WM walk the arranged tree; leaf gadget nodes are
 * rendered and driven through the shared Gad set.
 */

#include "uibind.h"
#include "uiformat.h"
#include "framebuffer.h"
#include "wm.h"
#include "../dos/vfs.h"
#include <stddef.h>

extern FbState g_fb;   /* for centre-on-screen in uibind_open */

static UIBind g_binds[UIBIND_MAX_WINDOWS];

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static UIBind *bind_for_handle(int h)
{
    for (int i = 0; i < UIBIND_MAX_WINDOWS; i++)
        if (g_binds[i].used && g_binds[i].wm == h)
            return &g_binds[i];
    return NULL;
}

static Gad *gad_of(UIBind *b, const UINode *n)
{
    if (!n) return NULL;
    for (int i = 0; i < b->ngads; i++)
        if (b->gnodes[i] == n)
            return &b->gads[i];
    return NULL;
}

/* 1 when n is inside the visible subtree (no UI_PAGE ancestor has a
 * different active child). */
static int node_visible(const UINode *n)
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

/* Re-arrange the tree into the window's client rect and push the node
 * rects into the matching Gads. */
static void bind_arrange(UIBind *b)
{
    int x, y, w, h;
    if (!WM_GetWindowRect(b->wm, &x, &y, &w, &h))
        return;
    int cx = x + UIBIND_MARGIN;
    int cy = y + WM_TITLEBAR_H + UIBIND_MARGIN;
    int cw = w - UIBIND_MARGIN * 2;
    int ch = h - WM_TITLEBAR_H - UIBIND_MARGIN * 2;
    ui_arrange(b->root, cx, cy, cw, ch);
    for (int i = 0; i < b->ngads; i++) {
        Gad *g = &b->gads[i];
        g->x = b->gnodes[i]->x;
        g->y = b->gnodes[i]->y;
        g->w = b->gnodes[i]->w;
        g->h = b->gnodes[i]->h;
    }
}

/* -------------------------------------------------------------------------
 * Tree → Gad binding (runs once at open; state then lives in the Gad)
 * ------------------------------------------------------------------------- */

static int bind_walk(UIBind *b, UINode *n)
{
    for (; n; n = n->next_sibling) {
        Gad *g = NULL;
        switch (n->type) {
        case UI_BUTTON:
        case UI_CHECKBOX:
        case UI_RADIO:
        case UI_CYCLE:
        case UI_SLIDER:
        case UI_STRING:
        case UI_INTEGER:
        case UI_LISTVIEW:
        case UI_LABEL:
            if (b->ngads >= UIBIND_MAX_GADS)
                return 0;
            g = &b->gads[b->ngads];
            b->gnodes[b->ngads] = n;
            b->ngads++;
            break;
        default:
            break;
        }
        if (g) {
            unsigned char *p = (unsigned char *)g;
            for (uint32_t i = 0; i < sizeof(Gad); i++)
                p[i] = 0;
            if (n->flags & UI_F_DISABLED)
                g->flags |= GADF_DISABLED;
            g->group = n->group;
            switch (n->type) {
            case UI_BUTTON:
                g->kind = GAD_BUTTON;
                g->text = n->u.text;
                break;
            case UI_CHECKBOX:
                g->kind = GAD_CHECKBOX;
                g->text = n->u.text;
                g->val  = (n->flags & UI_F_SELECTED) != 0;
                break;
            case UI_RADIO:
                g->kind = GAD_RADIO;
                g->text = n->u.text;
                g->val  = (n->flags & UI_F_SELECTED) != 0;
                break;
            case UI_CYCLE:
                g->kind     = GAD_CYCLE;
                g->choices  = (const char **)n->u.items.items;
                g->nchoices = n->u.items.count;
                g->val      = n->u.items.active;
                break;
            case UI_SLIDER:
                g->kind = GAD_SLIDER;
                g->min  = n->u.range.min;
                g->max  = n->u.range.max;
                g->val  = n->u.range.cur;
                if (n->flags & UI_F_VERTICAL)
                    g->flags |= GADF_VERTICAL;
                break;
            case UI_STRING:
            case UI_INTEGER: {
                int idx = (int)(g - b->gads);
                g->kind    = (n->type == UI_STRING) ? GAD_STRING : GAD_INTEGER;
                g->buf     = b->strbuf[idx];
                g->buf_max = n->u.input.max_chars > 0
                             ? n->u.input.max_chars + 1 : UIBIND_STRBUF;
                if (g->buf_max > UIBIND_STRBUF)
                    g->buf_max = UIBIND_STRBUF;
                if (n->type == UI_INTEGER) {
                    gad_itoa(g->buf, n->u.input.value);
                } else {
                    gad_str_cp(g->buf, n->u.input.text, g->buf_max);
                }
                g->buf_len = gad_slen(g->buf);
                g->cursor  = g->buf_len;
                break;
            }
            case UI_LISTVIEW:
                g->kind     = GAD_LISTVIEW;
                g->choices  = (const char **)n->u.items.items;
                g->nchoices = n->u.items.count;
                g->sel      = n->u.items.selected;
                break;
            case UI_LABEL:
            default:
                g->kind = GAD_LABEL;
                g->text = n->u.text;
                break;
            }
        }
        if (n->first_child && !bind_walk(b, n->first_child))
            return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

/* Draw the tab strip of every visible UI_PAGE in the tree. */
static void draw_page_tabs(UINode *n)
{
    for (; n; n = n->next_sibling) {
        if (n->type == UI_PAGE && node_visible(n)) {
            int i = 0;
            for (UINode *c = n->first_child; c; c = c->next_sibling, i++) {
                int tx, ty, tw, th;
                if (!ui_page_tab_rect(n, i, &tx, &ty, &tw, &th))
                    continue;
                int active = (i == n->u.page.active);
                FB_FillRect(tx, ty, tw, th,
                            active ? WB_LIGHT_GREY : WB_GREY);
                gad_bevel(tx, ty, tw, th, 1);
                if (c->tab)
                    FB_PutStr(tx + UI_TAB_PAD, ty + 3,
                              c->tab, WB_BLACK,
                              active ? WB_LIGHT_GREY : WB_GREY);
            }
        }
        if (n->first_child)
            draw_page_tabs(n->first_child);
    }
}

/* CUSTOM nodes render through their own callback. */
static void draw_custom(UIBind *b, UINode *n)
{
    for (; n; n = n->next_sibling) {
        if (!node_visible(n))
            continue;
        if (n->type == UI_CUSTOM && n->u.custom.draw)
            n->u.custom.draw(n, n->x, n->y, n->w, n->h,
                             n->u.custom.user ? n->u.custom.user : b->user);
        if (n->first_child)
            draw_custom(b, n->first_child);
    }
}

static void uibind_drawfn(int x, int y, int w, int h)
{
    UIBind *b = bind_for_handle(WM_CurrentDrawHandle);
    if (!b)
        return;

    gad_bg(x, y, w, h);
    bind_arrange(b);
    draw_page_tabs(b->root);

    for (int i = 0; i < b->ngads; i++) {
        UINode *n = b->gnodes[i];
        if (node_visible(n))
            gad_draw(&b->gads[i]);
    }

    draw_custom(b, b->root);
}

/* -------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

static void post_result(UIBind *b, UINode *n, int r)
{
    if (r == GADE_NONE || !n)
        return;
    if (r == GADE_CHANGE && n->type == UI_RADIO) {
        /* mutual exclusion: clear siblings in the same group */
        for (int i = 0; i < b->ngads; i++) {
            UINode *s = b->gnodes[i];
            if (s != n && s->type == UI_RADIO && s->group == n->group)
                b->gads[i].val = 0;
        }
    }
    uibind_repaint(b);
    if (b->cb)
        b->cb(b, n, r == GADE_CLICK ? UIEV_CLICK : UIEV_CHANGE, 0);
}

static void uibind_click(int handle, int mx, int my)
{
    UIBind *b = bind_for_handle(handle);
    if (!b) return;
    bind_arrange(b);

    /* String/integer fields always see the click for focus/defocus;
     * gad_event positions the cursor when inside. */
    int changed = 0;
    UINode *str_hit = NULL;
    for (int i = 0; i < b->ngads; i++) {
        Gad *g = &b->gads[i];
        if (g->kind != GAD_STRING && g->kind != GAD_INTEGER)
            continue;
        if (!node_visible(b->gnodes[i]))
            continue;
        int r = gad_event(g, GAD_DOWN, mx, my);
        if (r != GADE_NONE) {
            changed = 1;
            if (gad_hit(g, mx, my))
                str_hit = b->gnodes[i];
        }
    }

    UINode *n = ui_node_at(b->root, mx, my);

    if (n && n->type == UI_PAGE) {
        /* tab strip hit — switch active child */
        int i = 0;
        for (UINode *c = n->first_child; c; c = c->next_sibling, i++) {
            int tx, ty, tw, th;
            if (ui_page_tab_rect(n, i, &tx, &ty, &tw, &th) &&
                mx >= tx && mx < tx + tw && my >= ty && my < ty + th &&
                n->u.page.active != i) {
                n->u.page.active = i;
                changed = 1;
            }
        }
        if (changed) {
            uibind_repaint(b);
            if (b->cb)
                b->cb(b, n, UIEV_CHANGE, 0);
        }
        return;
    }

    Gad *g = gad_of(b, n);
    if (g && g->kind != GAD_STRING && g->kind != GAD_INTEGER) {
        if (g->kind == GAD_SLIDER)
            b->drag = n;             /* capture for GAD_MOVE tracking */
        post_result(b, n, gad_event(g, GAD_DOWN, mx, my));
    } else if (changed) {
        uibind_repaint(b);
        if (str_hit && b->cb)
            b->cb(b, str_hit, UIEV_CHANGE, 0);
    }
}

static void uibind_move(int handle, int mx, int my)
{
    UIBind *b = bind_for_handle(handle);
    if (!b || !b->drag) return;
    Gad *g = gad_of(b, b->drag);
    if (!g) return;
    if (g->kind == GAD_SLIDER) {
        int old = g->val;
        gad_slider_from_mouse(g, (g->flags & GADF_VERTICAL) ? my : mx);
        if (g->val != old) {
            uibind_repaint(b);
            if (b->cb)
                b->cb(b, b->drag, UIEV_CHANGE, 0);
        }
    }
}

static void uibind_release(int handle, int mx, int my)
{
    UIBind *b = bind_for_handle(handle);
    if (!b) return;
    if (b->drag) {
        Gad *g = gad_of(b, b->drag);
        b->drag = NULL;
        if (g)
            gad_event(g, GAD_UP, mx, my);
    }
}

static void uibind_key(char c)
{
    /* WM_KeyFn carries no handle — route to the focused window's bind. */
    UIBind *b = bind_for_handle(WM_GetFocus());
    if (!b) return;

    for (int i = 0; i < b->ngads; i++) {
        Gad *g = &b->gads[i];
        if ((g->kind == GAD_STRING || g->kind == GAD_INTEGER) &&
            g->focused && node_visible(b->gnodes[i])) {
            if (gad_event(g, GAD_KEY, (int)(unsigned char)c, 0)
                != GADE_NONE) {
                uibind_repaint(b);
                if (b->cb)
                    b->cb(b, b->gnodes[i], UIEV_CHANGE, 0);
                return;
            }
            break;   /* unconsumed (e.g. ESC/Enter) falls to the app */
        }
    }
    if (b->cb)
        b->cb(b, NULL, UIEV_KEY, (int)(unsigned char)c);
}

static int uibind_event(int handle, int ev, int p1, int p2, int p3)
{
    UIBind *b = bind_for_handle(handle);
    if (!b) return 0;
    switch (ev) {
    case WM_EVT_CLOSE_REQUEST: {
        int allow = b->cb ? b->cb(b, NULL, UIEV_CLOSE, 0) : 1;
        if (allow)
            b->used = 0;
        return allow;
    }
    case WM_EVT_RESIZE:
        bind_arrange(b);
        if (b->cb)
            b->cb(b, NULL, UIEV_RESIZE, p1);
        break;
    default:
        break;
    }
    (void)p2; (void)p3;
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

UIBind *uibind_open(UINode *root, UIEventFn cb, void *user,
                    int x, int y, int w, int h)
{
    if (!root)
        return NULL;

    UIBind *b = NULL;
    for (int i = 0; i < UIBIND_MAX_WINDOWS; i++)
        if (!g_binds[i].used) { b = &g_binds[i]; break; }
    if (!b)
        return NULL;

    unsigned char *p = (unsigned char *)b;
    for (uint32_t i = 0; i < sizeof(UIBind); i++)
        p[i] = 0;

    b->root = root;
    b->cb   = cb;
    b->user = user;
    b->wm   = -1;

    ui_measure(root);
    if (!bind_walk(b, root))
        return NULL;   /* too many gadget nodes */

    if (w <= 0) w = root->nat_w + UIBIND_MARGIN * 2;
    if (h <= 0) h = root->nat_h + WM_TITLEBAR_H + UIBIND_MARGIN * 2;
    if (x < 0)  x = ((int)g_fb.width  - w) / 2;
    if (y < 0)  y = ((int)g_fb.height - h) / 2;
    if (y < WM_TITLEBAR_H + 4) y = WM_TITLEBAR_H + 4;

    b->wm = WM_AddWindow(x, y, w, h,
                         root->u.text ? root->u.text : "UI",
                         uibind_drawfn, uibind_key);
    if (b->wm < 0)
        return NULL;

    b->used = 1;
    WM_SetClickHandler(b->wm, uibind_click);
    WM_SetMouseMoveHandler(b->wm, uibind_move);
    WM_SetMouseReleaseHandler(b->wm, uibind_release);
    WM_SetEventHandler(b->wm, uibind_event);
    WM_Redraw();
    return b;
}

void uibind_close(UIBind *b)
{
    if (!b || !b->used)
        return;
    int h = b->wm;
    b->used = 0;
    if (h >= 0)
        WM_CloseWindow(h);
}

int uibind_is_open(const UIBind *b)
{
    return b && b->used;
}

int uibind_handle(const UIBind *b)
{
    return (b && b->used) ? b->wm : -1;
}

Gad *uibind_gad(UIBind *b, int id)
{
    if (!b) return NULL;
    for (int i = 0; i < b->ngads; i++)
        if (b->gnodes[i]->id == id)
            return &b->gads[i];
    return NULL;
}

int uibind_val(UIBind *b, int id)
{
    Gad *g = uibind_gad(b, id);
    return g ? g->val : 0;
}

const char *uibind_text(UIBind *b, int id)
{
    Gad *g = uibind_gad(b, id);
    return (g && g->buf) ? g->buf : "";
}

void uibind_set_val(UIBind *b, int id, int v)
{
    Gad *g = uibind_gad(b, id);
    if (!g) return;
    UINode *n = NULL;
    for (int i = 0; i < b->ngads; i++)
        if (b->gnodes[i]->id == id) { n = b->gnodes[i]; break; }
    g->val = v;
    if (v && n && n->type == UI_RADIO) {
        for (int i = 0; i < b->ngads; i++) {
            UINode *s = b->gnodes[i];
            if (s != n && s->type == UI_RADIO && s->group == n->group)
                b->gads[i].val = 0;
        }
    }
    uibind_repaint(b);
}

void uibind_repaint(UIBind *b)
{
    int x, y, w, h;
    if (!b || !b->used || !WM_GetWindowRect(b->wm, &x, &y, &w, &h))
        return;
    WM_InvalidateRect(x, y, w, h);
}

UINode *ui_prefs_row(UIArena *a, int save_id, int use_id, int cancel_id)
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
 * .gui file loading (UAOS-127)
 * ------------------------------------------------------------------------- */

#define UIBIND_GUI_MAX  16384   /* largest .gui file accepted          */

/* Read `path` into a NUL-terminated buffer allocated from `arena` — the
 * parser carves strings out of it in place, so its lifetime must match
 * the resulting tree's.  Returns the buffer or NULL. */
static char *gui_read_file(UIArena *arena, const char *path)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ))
        return NULL;
    uint32_t size = VFS_Size(&fh);
    if (!size || size >= UIBIND_GUI_MAX) {
        VFS_Close(&fh);
        return NULL;
    }
    char *buf = (char *)ui_arena_alloc(arena, size + 1);
    if (!buf) {
        VFS_Close(&fh);
        return NULL;
    }
    uint32_t rd = VFS_Read(&fh, (uint8_t *)buf, size);
    VFS_Close(&fh);
    if (!rd)
        return NULL;
    buf[rd] = '\0';
    return buf;
}

UINode *uibind_load_gui(UIArena *arena, const char *tool, UINode *fallback)
{
    if (!arena || !tool)
        return fallback;

    static const char * const dirs[] = {
        "ENV:GUI/",          /* runtime override (RAM:)   */
        "SYS:Prefs/GUI/",    /* shipped description       */
    };
    uint32_t save = arena->used;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[96];
        int len = 0;
        for (const char *s = dirs[i]; *s && len < 90; s++) path[len++] = *s;
        for (const char *s = tool;   *s && len < 91; s++) path[len++] = *s;
        for (const char *s = ".gui"; *s && len < 95; s++) path[len++] = *s;
        path[len] = '\0';

        char *buf = gui_read_file(arena, path);
        if (!buf)
            continue;
        UIParseErr err;
        UINode *root = ui_parse(arena, buf, &err);
        if (root)
            return root;
        /* parse failed: rewind the arena so the fallback / next file
         * sees a clean pool */
        arena->used = save;
    }
    return fallback;
}
