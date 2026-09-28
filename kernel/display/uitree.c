/* uitree.c — UAOS declarative UI tree + layout engine
 *
 * Dependency-free implementation: measure computes natural sizes
 * bottom-up, arrange distributes the window rect top-down by weight.
 * See uitree.h for the model. No libc, FB_*, WM_* or syscall usage — the
 * same file compiles into the kernel, libuaos, and host-side tools.
 */

#include "uitree.h"
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Local helpers (no libc)
 * ------------------------------------------------------------------------- */
static int ui_strlen(const char *s)
{
    int n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* Decimal digits needed for a signed value (incl. sign). */
static int ui_int_digits(int v)
{
    int d = 1;
    if (v < 0) { d++; v = -v; }
    while (v >= 10) { v /= 10; d++; }
    return d;
}

/* -------------------------------------------------------------------------
 * Arena
 * ------------------------------------------------------------------------- */
void ui_arena_init(UIArena *a, void *buf, uint32_t cap)
{
    a->base = (unsigned char *)buf;
    a->cap  = cap;
    a->used = 0;
}

void *ui_arena_alloc(UIArena *a, uint32_t size)
{
    uint32_t aligned = (a->used + 7u) & ~7u;
    if (aligned + size > a->cap)
        return NULL;
    unsigned char *p = a->base + aligned;
    a->used = aligned + size;
    return p;
}

void ui_arena_reset(UIArena *a)
{
    a->used = 0;
}

/* -------------------------------------------------------------------------
 * Node construction
 * ------------------------------------------------------------------------- */
static UINode *ui_new_node(UIArena *a, UIType type)
{
    UINode *n = (UINode *)ui_arena_alloc(a, sizeof(UINode));
    if (!n)
        return NULL;
    unsigned char *p = (unsigned char *)n;
    for (uint32_t i = 0; i < sizeof(UINode); i++)
        p[i] = 0;
    n->type    = type;
    n->spacing = UI_DEF_SPACING;
    return n;
}

void ui_append(UINode *parent, UINode *child)
{
    if (!parent || !child)
        return;
    child->parent = parent;
    if (!parent->first_child) {
        parent->first_child = child;
        return;
    }
    UINode *c = parent->first_child;
    while (c->next_sibling)
        c = c->next_sibling;
    c->next_sibling = child;
}

static UINode *ui_container(UIArena *a, UIType type, va_list ap)
{
    UINode *n = ui_new_node(a, type);
    if (!n)
        return NULL;
    for (;;) {
        UINode *child = va_arg(ap, UINode *);
        if (!child)
            break;
        ui_append(n, child);
    }
    return n;
}

UINode *ui_window(UIArena *a, const char *title, UINode *content)
{
    UINode *n = ui_new_node(a, UI_WINDOW);
    if (!n)
        return NULL;
    n->u.text = title;
    if (content)
        ui_append(n, content);
    return n;
}

UINode *ui_vgroup(UIArena *a, ...)
{
    va_list ap;
    va_start(ap, a);
    UINode *n = ui_container(a, UI_VGROUP, ap);
    va_end(ap);
    return n;
}

UINode *ui_hgroup(UIArena *a, ...)
{
    va_list ap;
    va_start(ap, a);
    UINode *n = ui_container(a, UI_HGROUP, ap);
    va_end(ap);
    return n;
}

UINode *ui_page(UIArena *a, ...)
{
    va_list ap;
    va_start(ap, a);
    UINode *n = ui_container(a, UI_PAGE, ap);
    va_end(ap);
    return n;
}

UINode *ui_spacer(UIArena *a)
{
    UINode *n = ui_new_node(a, UI_SPACER);
    if (n)
        n->weight = 1;   /* spacers absorb free space by default */
    return n;
}

UINode *ui_label(UIArena *a, const char *text)
{
    UINode *n = ui_new_node(a, UI_LABEL);
    if (n)
        n->u.text = text;
    return n;
}

UINode *ui_button(UIArena *a, const char *text, int id)
{
    UINode *n = ui_new_node(a, UI_BUTTON);
    if (n) {
        n->id     = id;
        n->u.text = text;
    }
    return n;
}

UINode *ui_checkbox(UIArena *a, const char *text, int id)
{
    UINode *n = ui_new_node(a, UI_CHECKBOX);
    if (n) {
        n->id     = id;
        n->flags |= UI_F_TOGGLE;
        n->u.text = text;
    }
    return n;
}

UINode *ui_radio(UIArena *a, const char *text, int group, int id)
{
    UINode *n = ui_new_node(a, UI_RADIO);
    if (n) {
        n->id     = id;
        n->group  = group;
        n->u.text = text;
    }
    return n;
}

UINode *ui_cycle(UIArena *a, const char * const *items, int count, int id)
{
    UINode *n = ui_new_node(a, UI_CYCLE);
    if (n) {
        n->id             = id;
        n->u.items.items  = items;
        n->u.items.count  = count;
        n->u.items.active = 0;
    }
    return n;
}

UINode *ui_slider(UIArena *a, int min, int max, int cur, int id)
{
    UINode *n = ui_new_node(a, UI_SLIDER);
    if (n) {
        n->id            = id;
        n->u.range.min   = min;
        n->u.range.max   = max;
        n->u.range.cur   = cur;
    }
    return n;
}

UINode *ui_string(UIArena *a, const char *text, int max_chars, int id)
{
    UINode *n = ui_new_node(a, UI_STRING);
    if (n) {
        n->id                = id;
        n->u.input.text      = text;
        n->u.input.max_chars = max_chars;
    }
    return n;
}

UINode *ui_integer(UIArena *a, int value, int min, int max, int id)
{
    UINode *n = ui_new_node(a, UI_INTEGER);
    if (n) {
        n->id            = id;
        n->u.input.value = value;
        n->u.input.min   = min;
        n->u.input.max   = max;
    }
    return n;
}

UINode *ui_listview(UIArena *a, const char * const *items, int count, int id)
{
    UINode *n = ui_new_node(a, UI_LISTVIEW);
    if (n) {
        n->id               = id;
        n->u.items.items    = items;
        n->u.items.count    = count;
        n->u.items.selected = -1;
    }
    return n;
}

UINode *ui_custom(UIArena *a, int min_w, int min_h,
                  UICustomDrawFn draw, void *user, int id)
{
    UINode *n = ui_new_node(a, UI_CUSTOM);
    if (n) {
        n->id             = id;
        n->min_w          = min_w;
        n->min_h          = min_h;
        n->u.custom.draw  = draw;
        n->u.custom.user  = user;
    }
    return n;
}

/* -------------------------------------------------------------------------
 * Measure — bottom-up natural sizes (written to nat_w/nat_h, incl. pad)
 * ------------------------------------------------------------------------- */
static void measure_leaf(UINode *n, int *w, int *h)
{
    switch (n->type) {
    case UI_LABEL:
        *w = ui_strlen(n->u.text) * UI_CHAR_W;
        *h = UI_LINE_H;
        break;
    case UI_BUTTON:
        *w = ui_strlen(n->u.text) * UI_CHAR_W + UI_TEXT_PAD * 4;
        *h = UI_BUTTON_H;
        break;
    case UI_CHECKBOX:
    case UI_RADIO:
        *w = UI_CHECK_BOX + UI_TEXT_PAD + ui_strlen(n->u.text) * UI_CHAR_W;
        *h = UI_CHECK_H;
        break;
    case UI_CYCLE: {
        int tw = 0;
        for (int i = 0; i < n->u.items.count; i++)
            tw = imax(tw, ui_strlen(n->u.items.items[i]) * UI_CHAR_W);
        *w = tw + UI_TEXT_PAD + UI_CYCLE_ARROW;
        *h = UI_BUTTON_H;
        break;
    }
    case UI_SLIDER:
        if (n->flags & UI_F_VERTICAL) {
            *w = UI_SLIDER_H;
            *h = 60;
        } else {
            *w = 60;
            *h = UI_SLIDER_H;
        }
        break;
    case UI_STRING: {
        int chars = n->u.input.max_chars > 0 ? n->u.input.max_chars : 10;
        int tw    = ui_strlen(n->u.input.text) * UI_CHAR_W;
        *w = imax(chars * UI_CHAR_W, tw) + UI_TEXT_PAD * 2;
        *h = UI_INPUT_H;
        break;
    }
    case UI_INTEGER: {
        int d1 = ui_int_digits(n->u.input.min);
        int d2 = ui_int_digits(n->u.input.max);
        *w = imax(d1, d2) * UI_CHAR_W + UI_TEXT_PAD * 2;
        *h = UI_INPUT_H;
        break;
    }
    case UI_LISTVIEW:
        *w = 120;
        *h = 80;
        break;
    case UI_SPACER:
    case UI_CUSTOM:
    default:
        *w = 0;
        *h = 0;
        break;
    }
}

void ui_measure(UINode *n)
{
    if (!n)
        return;

    int nw = 0, nh = 0;

    switch (n->type) {
    case UI_VGROUP: {
        int count = 0;
        for (UINode *c = n->first_child; c; c = c->next_sibling) {
            ui_measure(c);
            nw = imax(nw, c->nat_w);
            nh += c->nat_h;
            count++;
        }
        if (count > 1)
            nh += n->spacing * (count - 1);
        break;
    }
    case UI_HGROUP: {
        int count = 0;
        for (UINode *c = n->first_child; c; c = c->next_sibling) {
            ui_measure(c);
            nh = imax(nh, c->nat_h);
            nw += c->nat_w;
            count++;
        }
        if (count > 1)
            nw += n->spacing * (count - 1);
        break;
    }
    case UI_PAGE: {
        int tabs_w = 0;
        for (UINode *c = n->first_child; c; c = c->next_sibling) {
            ui_measure(c);
            nw = imax(nw, c->nat_w);
            nh = imax(nh, c->nat_h);
            tabs_w += ui_strlen(c->tab) * UI_CHAR_W + UI_TAB_PAD * 2;
        }
        nh += UI_PAGE_TAB_H;
        nw  = imax(nw, tabs_w);
        break;
    }
    case UI_WINDOW:
        if (n->first_child) {
            ui_measure(n->first_child);
            nw = n->first_child->nat_w;
            nh = n->first_child->nat_h;
        }
        break;
    default:
        measure_leaf(n, &nw, &nh);
        break;
    }

    nw += n->pad * 2;
    nh += n->pad * 2;
    if (n->min_w) nw = imax(nw, n->min_w);
    if (n->min_h) nh = imax(nh, n->min_h);
    if (n->max_w) nw = imin(nw, n->max_w);
    if (n->max_h) nh = imin(nh, n->max_h);
    n->nat_w = nw;
    n->nat_h = nh;
}

/* -------------------------------------------------------------------------
 * Arrange — top-down rect distribution
 * ------------------------------------------------------------------------- */

/* Distribute the main-axis extent of group n among its children and
 * recurse. horiz != 0 means an HGROUP (main axis = x/width). */
static void arrange_group(UINode *n, int horiz)
{
    int len   = horiz ? n->w : n->h;
    int cross = horiz ? n->h : n->w;
    int gx    = n->x;
    int gy    = n->y;
    if (len < 0)
        len = 0;

    /* pass 1: sum minimums and weights */
    int count = 0, tot_min = 0, tot_w = 0;
    for (UINode *c = n->first_child; c; c = c->next_sibling) {
        tot_min += horiz ? c->nat_w : c->nat_h;
        if (c->weight > 0)
            tot_w += c->weight;
        count++;
    }
    int gaps  = count > 1 ? (count - 1) * n->spacing : 0;
    int avail = len - gaps;
    if (avail < 0)
        avail = 0;
    int free_px = avail - tot_min;

    /* pass 2: assign cells */
    int pos = 0;
    for (UINode *c = n->first_child; c; c = c->next_sibling) {
        int m = horiz ? c->nat_w : c->nat_h;
        int cell;
        if (free_px <= 0) {
            /* oversubscribed: scale all minimums down proportionally */
            cell = tot_min > 0 ? (int)(((int64_t)m * avail) / tot_min) : 0;
        } else {
            int share = (tot_w > 0 && c->weight > 0)
                        ? (int)((int64_t)free_px * c->weight / tot_w)
                        : 0;
            cell = m + share;
        }
        /* last child mops up rounding so cells tile the group exactly —
         * except when nobody wanted the free space (keep start-packed).
         * pos already includes the consumed gaps, so use len - pos. */
        if (!c->next_sibling && (free_px <= 0 || tot_w > 0))
            cell = len - pos;
        if (cell < 0)
            cell = 0;

        if (horiz)
            ui_arrange(c, gx + pos, gy, cell, cross);
        else
            ui_arrange(c, gx, gy + pos, cross, cell);

        pos += cell + n->spacing;
    }
}

void ui_arrange(UINode *n, int x, int y, int w, int h)
{
    if (!n)
        return;

    /* node's box = cell inset by pad, clamped by explicit max and (when
     * UI_F_CENTER) by natural size */
    int cw = w - n->pad * 2;
    int ch = h - n->pad * 2;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;

    int bw = cw, bh = ch;
    if (n->max_w && bw > n->max_w) bw = n->max_w;
    if (n->max_h && bh > n->max_h) bh = n->max_h;
    if (n->flags & UI_F_CENTER) {
        bw = imin(bw, imax(n->nat_w - n->pad * 2, 0));
        bh = imin(bh, imax(n->nat_h - n->pad * 2, 0));
    }

    if (n->flags & UI_F_CENTER) {
        n->x = x + n->pad + (cw - bw) / 2;
        n->y = y + n->pad + (ch - bh) / 2;
    } else {
        n->x = x + n->pad;
        n->y = y + n->pad;
    }
    n->w = bw;
    n->h = bh;

    switch (n->type) {
    case UI_WINDOW:
        if (n->first_child)
            ui_arrange(n->first_child, n->x, n->y, n->w, n->h);
        break;
    case UI_VGROUP:
        arrange_group(n, 0);
        break;
    case UI_HGROUP:
        arrange_group(n, 1);
        break;
    case UI_PAGE: {
        int cy = n->y + UI_PAGE_TAB_H;
        int chh = n->h - UI_PAGE_TAB_H;
        if (chh < 0)
            chh = 0;
        for (UINode *c = n->first_child; c; c = c->next_sibling)
            ui_arrange(c, n->x, cy, n->w, chh);
        break;
    }
    default:
        break;
    }
}

void ui_layout(UINode *root, int x, int y, int w, int h)
{
    ui_measure(root);
    ui_arrange(root, x, y, w, h);
}

/* -------------------------------------------------------------------------
 * Tree operations
 * ------------------------------------------------------------------------- */
UINode *ui_find(UINode *n, int id)
{
    for (; n; n = n->next_sibling) {
        if (n->id == id)
            return n;
        UINode *hit = ui_find(n->first_child, id);
        if (hit)
            return hit;
    }
    return NULL;
}

UINode *ui_node_at(UINode *n, int mx, int my)
{
    for (; n; n = n->next_sibling) {
        if (mx < n->x || mx >= n->x + n->w ||
            my < n->y || my >= n->y + n->h)
            continue;
        if (n->type == UI_PAGE) {
            if (my < n->y + UI_PAGE_TAB_H)
                return n;              /* tab strip hit */
            int i = 0;
            for (UINode *c = n->first_child; c; c = c->next_sibling, i++) {
                if (i != n->u.page.active)
                    continue;
                UINode *hit = ui_node_at(c, mx, my);
                return hit ? hit : n;
            }
            return n;
        }
        UINode *hit = ui_node_at(n->first_child, mx, my);
        return hit ? hit : n;
    }
    return NULL;
}

int ui_page_tab_rect(const UINode *page, int index,
                     int *x, int *y, int *w, int *h)
{
    if (!page || page->type != UI_PAGE)
        return 0;
    int tx = page->x;
    int i  = 0;
    for (UINode *c = page->first_child; c; c = c->next_sibling, i++) {
        int tw = ui_strlen(c->tab) * UI_CHAR_W + UI_TAB_PAD * 2;
        if (i == index) {
            *x = tx;
            *y = page->y;
            *w = tw;
            *h = UI_PAGE_TAB_H;
            return 1;
        }
        tx += tw;
    }
    return 0;
}

int ui_child_count(const UINode *n)
{
    int count = 0;
    if (n)
        for (UINode *c = n->first_child; c; c = c->next_sibling)
            count++;
    return count;
}
