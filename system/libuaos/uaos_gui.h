/* uaos_gui.h — UAOS userspace GUI widget toolkit
 *
 * Provides AmigaOS 3.1-style gadget classes for native x86-64 tasks:
 *   - Button (boolean gadget)
 *   - Checkbox (toggle gadget)
 *   - Radio button (mutually-exclusive group)
 *   - Slider (proportional gadget)
 *   - String gadget (text input)
 *   - Integer gadget (numeric input)
 *   - Label (static text)
 *   - Group border (3D recessed frame with title)
 *   - Listview (scrollable item list)
 *
 * All widgets draw into a userspace window created via uaos_gui_create_window().
 * The application polls events via uaos_gui_get_event() and dispatches them
 * through the widget set with uaos_gui_handle_event().
 *
 * Design follows AmigaOS GadTools conventions:
 *   - NewGadget struct describes gadget geometry and flags
 *   - CreateGadgetA returns a gadget pointer (opaque handle)
 *   - Gadgets are linked in a list per window
 *   - GT_GetGadgetAttrs / GT_SetGadgetAttrs query/set state
 */

#ifndef UAOS_GUI_H
#define UAOS_GUI_H

#include "uaos_syscall.h"
#include "uaos_libc.h"

/* -------------------------------------------------------------------------
 * Widget types (mirror AmigaOS GadTools gadget kinds)
 * ------------------------------------------------------------------------- */
enum {
    UAOS_GAD_BUTTON   = 0,
    UAOS_GAD_CHECKBOX = 1,
    UAOS_GAD_RADIO    = 2,
    UAOS_GAD_SLIDER   = 3,
    UAOS_GAD_STRING   = 4,
    UAOS_GAD_INTEGER  = 5,
    UAOS_GAD_LABEL    = 6,
    UAOS_GAD_LISTVIEW = 7,
};

/* NewGadget flags (bitmask) */
#define UAOS_NG_TOGGLE    0x0001  /* checkbox toggles on each click */
#define UAOS_NG_DISABLED  0x0002  /* gadget is greyed out */
#define UAOS_NG_HSCROLL   0x0004  /* slider is horizontal (default) */
#define UAOS_NG_VSCROLL   0x0008  /* slider is vertical */
#define UAOS_NG_READONLY  0x0010  /* string/integer gadget is read-only */

/* -------------------------------------------------------------------------
 * Widget structures
 * ------------------------------------------------------------------------- */
typedef struct uaos_widget {
    int      type;
    int      id;
    int      x, y, w, h;
    uint32_t flags;
    int      state;        /* selected/checked state (0 or 1) */
    int      min_val;      /* slider/integer minimum */
    int      max_val;      /* slider/integer maximum */
    int      cur_val;      /* slider/integer current value */
    char     text[64];     /* button label / string content */
    int      max_chars;    /* string/integer max characters */
    int      cursor_pos;   /* string cursor position */
    int      sel_start;    /* string selection start */
    int      sel_end;      /* string selection end */
    int      active;       /* string gadget is active (has focus) */
    int      group_id;     /* radio group identifier */
    int      win_handle;   /* owning window handle */
    int      dragging;     /* slider is being dragged */
    int      drag_start;   /* slider drag start position */
    int      drag_start_val; /* slider value at drag start */
    struct uaos_widget *next;
    struct uaos_widget *prev;
} uaos_widget_t;

typedef struct {
    int              win_handle;
    uaos_widget_t   *widgets;
    uaos_widget_t   *active_string;
} uaos_gui_t;

/* -------------------------------------------------------------------------
 * Color palette (Workbench 3.1 compatible)
 * ------------------------------------------------------------------------- */
#define UAOS_COL_BLUE       0x0055AA
#define UAOS_COL_LIGHT_BLUE 0x0088FF
#define UAOS_COL_GREY       0xAAAAAA
#define UAOS_COL_LIGHT_GREY 0xCCCCCC
#define UAOS_COL_DARK_GREY  0x555555
#define UAOS_COL_WHITE      0xFFFFFF
#define UAOS_COL_BLACK      0x000000
#define UAOS_COL_ORANGE     0xFF8800
#define UAOS_COL_CREAM      0xFFFFCC
#define UAOS_COL_RED        0xCC0000
#define UAOS_COL_GREEN      0x00AA00

/* -------------------------------------------------------------------------
 * NewGadget — describes a gadget before creation
 * ------------------------------------------------------------------------- */
typedef struct {
    int      type;
    int      id;
    int      left, top, width, height;
    uint32_t flags;
    const char *text;
    int      min_val, max_val, cur_val;
    int      max_chars;
    int      group_id;
} uaos_newgadget_t;

/* -------------------------------------------------------------------------
 * GUI context management
 * ------------------------------------------------------------------------- */
static inline void uaos_gui_init(uaos_gui_t *gui, int win_handle)
{
    gui->win_handle = win_handle;
    gui->widgets = NULL;
    gui->active_string = NULL;
}

static inline void uaos_gui_free(uaos_gui_t *gui)
{
    uaos_widget_t *w = gui->widgets;
    while (w) {
        uaos_widget_t *next = w->next;
        uaos_alloc(0); /* cannot free individually; pool is simple */
        w = next;
    }
    gui->widgets = NULL;
    gui->active_string = NULL;
}

/* -------------------------------------------------------------------------
 * Widget creation
 * ------------------------------------------------------------------------- */
static inline uaos_widget_t *uaos_gui_create_gadget(uaos_gui_t *gui,
                                                     const uaos_newgadget_t *ng)
{
    /* Use syscall alloc for widget storage */
    uaos_widget_t *w = (uaos_widget_t *)uaos_alloc(sizeof(uaos_widget_t));
    if (!w) return NULL;
    uaos_memset(w, 0, sizeof(*w));

    w->type      = ng->type;
    w->id        = ng->id;
    w->x         = ng->left;
    w->y         = ng->top;
    w->w         = ng->width;
    w->h         = ng->height;
    w->flags     = ng->flags;
    w->state     = 0;
    w->min_val   = ng->min_val;
    w->max_val   = ng->max_val;
    w->cur_val   = ng->cur_val;
    w->max_chars = ng->max_chars ? ng->max_chars : 32;
    w->cursor_pos = 0;
    w->sel_start  = 0;
    w->sel_end    = 0;
    w->active     = 0;
    w->group_id   = ng->group_id;
    w->win_handle = gui->win_handle;
    w->dragging   = 0;
    w->next       = NULL;

    if (ng->text) {
        uaos_strncpy(w->text, ng->text, sizeof(w->text) - 1);
        w->text[sizeof(w->text) - 1] = '\0';
        if (w->type == UAOS_GAD_STRING || w->type == UAOS_GAD_INTEGER) {
            w->cursor_pos = uaos_strlen(w->text);
            w->sel_start = w->sel_end = w->cursor_pos;
        }
    }

    /* Link into list */
    w->prev = NULL;
    w->next = gui->widgets;
    if (gui->widgets)
        gui->widgets->prev = w;
    gui->widgets = w;

    return w;
}

/* -------------------------------------------------------------------------
 * Widget lookup
 * ------------------------------------------------------------------------- */
static inline uaos_widget_t *uaos_gui_find_gadget(uaos_gui_t *gui, int id)
{
    uaos_widget_t *w = gui->widgets;
    while (w) {
        if (w->id == id) return w;
        w = w->next;
    }
    return NULL;
}

static inline uaos_widget_t *uaos_gui_gadget_at(uaos_gui_t *gui, int x, int y)
{
    uaos_widget_t *w = gui->widgets;
    while (w) {
        if (!(w->flags & UAOS_NG_DISABLED) &&
            x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h)
            return w;
        w = w->next;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Drawing helpers
 * ------------------------------------------------------------------------- */
static inline void uaos_gui_draw_bevel(int win, int x, int y, int w, int h,
                                       int raised, uint32_t base)
{
    uaos_gui_draw_3d_border(win, x, y, w, h, raised, base);
}

static inline int uaos_text_width(const char *s)
{
    return uaos_strlen(s) * 8;
}

/* -------------------------------------------------------------------------
 * Widget rendering
 * ------------------------------------------------------------------------- */
static inline void uaos_gui_draw_widget(uaos_gui_t *gui, uaos_widget_t *w)
{
    int win = gui->win_handle;
    int x = w->x, y = w->y, wd = w->w, h = w->h;

    switch (w->type) {
    case UAOS_GAD_BUTTON: {
        uint32_t bg = (w->flags & UAOS_NG_DISABLED) ? UAOS_COL_LIGHT_GREY : UAOS_COL_GREY;
        int raised = !(w->state);
        uaos_gui_fill_rect(win, x, y, wd, h, bg);
        uaos_gui_draw_bevel(win, x, y, wd, h, raised, bg);
        if (w->state) {
            /* Pressed: shift text by 1px */
            uaos_gui_draw_text_bg(win, x + 5, y + (h - 16) / 2 + 1,
                                  w->text, UAOS_COL_BLACK, bg);
        } else {
            uaos_gui_draw_text_bg(win, x + 4, y + (h - 16) / 2,
                                  w->text, UAOS_COL_BLACK, bg);
        }
        break;
    }

    case UAOS_GAD_CHECKBOX: {
        /* Draw label to the right of the checkbox */
        int box_sz = h < 16 ? h : 16;
        uint32_t bg = UAOS_COL_WHITE;
        uaos_gui_fill_rect(win, x, y, box_sz, box_sz, bg);
        uaos_gui_draw_bevel(win, x, y, box_sz, box_sz, 0, UAOS_COL_GREY);
        if (w->state) {
            /* Draw an X mark */
            uaos_gui_draw_line(win, x + 3, y + 3, x + box_sz - 4, y + box_sz - 4, UAOS_COL_BLACK);
            uaos_gui_draw_line(win, x + box_sz - 4, y + 3, x + 3, y + box_sz - 4, UAOS_COL_BLACK);
        }
        if (w->text[0]) {
            uaos_gui_draw_text_bg(win, x + box_sz + 4, y + (h - 16) / 2,
                                  w->text, UAOS_COL_BLACK, UAOS_COL_WHITE);
        }
        break;
    }

    case UAOS_GAD_RADIO: {
        int box_sz = h < 14 ? h : 14;
        int cx = x + box_sz / 2;
        int cy = y + h / 2;
        uint32_t bg = UAOS_COL_WHITE;
        uaos_gui_fill_rect(win, x, y, box_sz, box_sz, bg);
        uaos_gui_draw_ellipse(win, cx, cy, box_sz / 2 - 1, box_sz / 2 - 1, UAOS_COL_DARK_GREY);
        if (w->state) {
            uaos_gui_fill_rect(win, cx - 3, cy - 3, 6, 6, UAOS_COL_BLACK);
        }
        if (w->text[0]) {
            uaos_gui_draw_text_bg(win, x + box_sz + 4, y + (h - 16) / 2,
                                  w->text, UAOS_COL_BLACK, UAOS_COL_WHITE);
        }
        break;
    }

    case UAOS_GAD_SLIDER: {
        int is_vert = (w->flags & UAOS_NG_VSCROLL);
        uint32_t bg = UAOS_COL_LIGHT_GREY;
        uaos_gui_fill_rect(win, x, y, wd, h, bg);
        uaos_gui_draw_bevel(win, x, y, wd, h, 0, UAOS_COL_GREY);
        /* Compute knob position */
        int range = w->max_val - w->min_val;
        if (range <= 0) range = 1;
        int knob_pos;
        if (is_vert) {
            int track_h = h - 8;
            knob_pos = ((w->cur_val - w->min_val) * track_h) / range;
            int ky = y + 4 + knob_pos;
            uaos_gui_fill_rect(win, x + 2, ky, wd - 4, 8, UAOS_COL_GREY);
            uaos_gui_draw_bevel(win, x + 2, ky, wd - 4, 8, 1, UAOS_COL_GREY);
        } else {
            int track_w = wd - 8;
            knob_pos = ((w->cur_val - w->min_val) * track_w) / range;
            int kx = x + 4 + knob_pos;
            uaos_gui_fill_rect(win, kx, y + 2, 8, h - 4, UAOS_COL_GREY);
            uaos_gui_draw_bevel(win, kx, y + 2, 8, h - 4, 1, UAOS_COL_GREY);
        }
        break;
    }

    case UAOS_GAD_STRING:
    case UAOS_GAD_INTEGER: {
        uint32_t bg = (w->flags & UAOS_NG_READONLY) ? UAOS_COL_LIGHT_GREY : UAOS_COL_WHITE;
        uaos_gui_fill_rect(win, x, y, wd, h, bg);
        uaos_gui_draw_bevel(win, x, y, wd, h, 0, UAOS_COL_GREY);
        /* Draw text content */
        int tx = x + 4;
        int ty = y + (h - 16) / 2;
        if (w->text[0]) {
            uaos_gui_draw_text_bg(win, tx, ty, w->text, UAOS_COL_BLACK, bg);
        }
        /* Draw cursor if active */
        if (w->active) {
            int cursor_x = tx + w->cursor_pos * 8;
            uaos_gui_draw_line(win, cursor_x, ty, cursor_x, ty + 15, UAOS_COL_BLACK);
            /* Draw selection highlight */
            if (w->sel_start != w->sel_end) {
                int sel_lo = w->sel_start < w->sel_end ? w->sel_start : w->sel_end;
                int sel_hi = w->sel_start < w->sel_end ? w->sel_end : w->sel_start;
                int sx = tx + sel_lo * 8;
                int sw = (sel_hi - sel_lo) * 8;
                /* Invert selection area */
                uaos_gui_fill_rect(win, sx, ty, sw, 16, UAOS_COL_BLUE);
                uaos_gui_draw_text_bg(win, sx, ty,
                                      w->text + sel_lo, UAOS_COL_WHITE, UAOS_COL_BLUE);
            }
        }
        break;
    }

    case UAOS_GAD_LABEL: {
        uaos_gui_draw_text_bg(win, x, y + (h - 16) / 2, w->text,
                              UAOS_COL_BLACK, UAOS_COL_WHITE);
        break;
    }

    case UAOS_GAD_LISTVIEW: {
        /* Draw recessed border */
        uaos_gui_fill_rect(win, x, y, wd, h, UAOS_COL_WHITE);
        uaos_gui_draw_bevel(win, x, y, wd, h, 0, UAOS_COL_GREY);
        /* Items would be drawn here by the application via callbacks.
         * This is a minimal stub that shows the basic frame. */
        break;
    }
    }
}

static inline void uaos_gui_draw_all(uaos_gui_t *gui)
{
    uaos_widget_t *w = gui->widgets;
    /* Draw in reverse order so first-created is on top */
    while (w && w->next) w = w->next;
    while (w) {
        uaos_gui_draw_widget(gui, w);
        w = w->prev;
    }
    uaos_gui_present(gui->win_handle);
}

/* -------------------------------------------------------------------------
 * Group border (recessed frame with title text)
 * ------------------------------------------------------------------------- */
static inline void uaos_gui_draw_group(uaos_gui_t *gui, int x, int y, int w, int h,
                                       const char *title)
{
    int win = gui->win_handle;
    uaos_gui_fill_rect(win, x, y, w, h, UAOS_COL_WHITE);
    uaos_gui_draw_bevel(win, x, y, w, h, 0, UAOS_COL_GREY);
    if (title && title[0]) {
        int tw = uaos_text_width(title) + 8;
        /* Clear background behind title */
        uaos_gui_fill_rect(win, x + 8, y - 8, tw, 16, UAOS_COL_WHITE);
        uaos_gui_draw_text_bg(win, x + 12, y - 8, title, UAOS_COL_BLACK, UAOS_COL_WHITE);
    }
}

/* -------------------------------------------------------------------------
 * Event handling
 * ------------------------------------------------------------------------- */

/* Slider update from mouse drag */
static inline void slider_update_from_mouse(uaos_widget_t *w, int mx, int my)
{
    int range = w->max_val - w->min_val;
    if (range <= 0) range = 1;
    if (w->flags & UAOS_NG_VSCROLL) {
        int track_h = w->h - 8;
        if (track_h <= 0) track_h = 1;
        int rel = my - w->y - 4;
        if (rel < 0) rel = 0;
        if (rel > track_h) rel = track_h;
        w->cur_val = w->min_val + (rel * range) / track_h;
    } else {
        int track_w = w->w - 8;
        if (track_w <= 0) track_w = 1;
        int rel = mx - w->x - 4;
        if (rel < 0) rel = 0;
        if (rel > track_w) rel = track_w;
        w->cur_val = w->min_val + (rel * range) / track_w;
    }
    if (w->cur_val < w->min_val) w->cur_val = w->min_val;
    if (w->cur_val > w->max_val) w->cur_val = w->max_val;
}

/* String gadget key handling */
static inline void string_handle_key(uaos_gui_t *gui, uaos_widget_t *w, char c)
{
    int len = uaos_strlen(w->text);

    if (c == '\n' || c == '\r' || c == '\t') {
        /* Deactivate on Enter/Tab */
        w->active = 0;
        if (gui->active_string == w) gui->active_string = NULL;
        return;
    }

    if (c == '\b') {
        if (w->cursor_pos > 0) {
            for (int i = w->cursor_pos - 1; i < len; i++)
                w->text[i] = w->text[i + 1];
            w->cursor_pos--;
            w->sel_start = w->sel_end = w->cursor_pos;
        }
        return;
    }

    if (c == 0x7F) { /* Delete */
        if (w->cursor_pos < len) {
            for (int i = w->cursor_pos; i < len; i++)
                w->text[i] = w->text[i + 1];
        }
        return;
    }

    if (c == 0x01) { /* Ctrl+A = select all */
        w->cursor_pos = len;
        w->sel_start = 0;
        w->sel_end = len;
        return;
    }

    if (c == 0x05) { /* Left arrow */
        if (w->cursor_pos > 0) {
            w->cursor_pos--;
            w->sel_start = w->sel_end = w->cursor_pos;
        }
        return;
    }

    if (c == 0x06) { /* Right arrow */
        if (w->cursor_pos < len) {
            w->cursor_pos++;
            w->sel_start = w->sel_end = w->cursor_pos;
        }
        return;
    }

    if (c == 0x03) { /* Up/Home */
        w->cursor_pos = 0;
        w->sel_start = w->sel_end = 0;
        return;
    }

    if (c == 0x04) { /* Down/End */
        w->cursor_pos = len;
        w->sel_start = w->sel_end = len;
        return;
    }

    /* Printable character */
    if (c >= 32 && c < 127) {
        if (w->type == UAOS_GAD_INTEGER && (c < '0' || c > '9') && c != '-')
            return;
        if (len < w->max_chars - 1) {
            for (int i = len; i >= w->cursor_pos; i--)
                w->text[i + 1] = w->text[i];
            w->text[w->cursor_pos] = c;
            w->cursor_pos++;
            w->sel_start = w->sel_end = w->cursor_pos;
        }
    }
}

/* Main event handler — returns the gadget id if an event triggered a
 * gadget state change, or -1 if no gadget was affected. */
static inline int uaos_gui_handle_event(uaos_gui_t *gui,
                                        const struct uaos_gui_event *ev)
{
    if (!ev || ev->type == 0)
        return -1;

    /* Key events go to active string gadget */
    if (ev->type == UAOS_GUI_EVENT_KEY) {
        if (gui->active_string) {
            string_handle_key(gui, gui->active_string, (char)ev->x);
            return gui->active_string->id;
        }
        return -1;
    }

    /* Mouse events */
    if (ev->type == UAOS_GUI_EVENT_CLICK) {
        int mx = ev->x;
        int my = ev->y;

        /* Deactivate any active string gadget if clicking outside */
        if (gui->active_string) {
            uaos_widget_t *aw = gui->active_string;
            if (mx < aw->x || mx >= aw->x + aw->w ||
                my < aw->y || my >= aw->y + aw->h) {
                aw->active = 0;
                gui->active_string = NULL;
            }
        }

        uaos_widget_t *w = uaos_gui_gadget_at(gui, mx, my);
        if (!w) return -1;

        switch (w->type) {
        case UAOS_GAD_BUTTON:
            w->state = 1;
            return w->id;

        case UAOS_GAD_CHECKBOX:
            if (!(w->flags & UAOS_NG_DISABLED)) {
                w->state = !w->state;
                return w->id;
            }
            return -1;

        case UAOS_GAD_RADIO:
            if (!(w->flags & UAOS_NG_DISABLED) && !w->state) {
                /* Clear all radios in the same group */
                uaos_widget_t *p = gui->widgets;
                while (p) {
                    if (p->type == UAOS_GAD_RADIO && p->group_id == w->group_id)
                        p->state = 0;
                    p = p->next;
                }
                w->state = 1;
                return w->id;
            }
            return -1;

        case UAOS_GAD_SLIDER:
            w->dragging = 1;
            slider_update_from_mouse(w, mx, my);
            return w->id;

        case UAOS_GAD_STRING:
        case UAOS_GAD_INTEGER:
            if (!(w->flags & UAOS_NG_READONLY)) {
                /* Activate this string gadget */
                if (gui->active_string && gui->active_string != w) {
                    gui->active_string->active = 0;
                }
                w->active = 1;
                gui->active_string = w;
                /* Place cursor at click position */
                int rel_x = mx - w->x - 4;
                int pos = rel_x / 8;
                int len = uaos_strlen(w->text);
                if (pos < 0) pos = 0;
                if (pos > len) pos = len;
                w->cursor_pos = pos;
                w->sel_start = w->sel_end = pos;
            }
            return w->id;

        case UAOS_GAD_LISTVIEW:
            return w->id;
        }
        return -1;
    }

    if (ev->type == UAOS_GUI_EVENT_RELEASE) {
        /* Release any pressed button or slider */
        uaos_widget_t *w = gui->widgets;
        while (w) {
            if (w->type == UAOS_GAD_BUTTON && w->state) {
                w->state = 0;
                return w->id;
            }
            if (w->type == UAOS_GAD_SLIDER && w->dragging) {
                w->dragging = 0;
                return w->id;
            }
            w = w->next;
        }
        return -1;
    }

    if (ev->type == UAOS_GUI_EVENT_MOVE) {
        /* Update slider drag */
        uaos_widget_t *w = gui->widgets;
        while (w) {
            if (w->type == UAOS_GAD_SLIDER && w->dragging) {
                slider_update_from_mouse(w, ev->x, ev->y);
                return w->id;
            }
            w = w->next;
        }
        return -1;
    }

    return -1;
}

/* -------------------------------------------------------------------------
 * Get/Set gadget attributes
 * ------------------------------------------------------------------------- */
static inline int uaos_gui_get_int(uaos_gui_t *gui, int id)
{
    uaos_widget_t *w = uaos_gui_find_gadget(gui, id);
    if (!w) return 0;
    switch (w->type) {
    case UAOS_GAD_CHECKBOX:
    case UAOS_GAD_RADIO:
        return w->state;
    case UAOS_GAD_SLIDER:
    case UAOS_GAD_INTEGER:
        return w->cur_val;
    default:
        return 0;
    }
}

static inline const char *uaos_gui_get_str(uaos_gui_t *gui, int id)
{
    uaos_widget_t *w = uaos_gui_find_gadget(gui, id);
    if (!w) return NULL;
    if (w->type == UAOS_GAD_STRING || w->type == UAOS_GAD_INTEGER ||
        w->type == UAOS_GAD_LABEL || w->type == UAOS_GAD_BUTTON)
        return w->text;
    return NULL;
}

static inline void uaos_gui_set_int(uaos_gui_t *gui, int id, int val)
{
    uaos_widget_t *w = uaos_gui_find_gadget(gui, id);
    if (!w) return;
    switch (w->type) {
    case UAOS_GAD_CHECKBOX:
    case UAOS_GAD_RADIO:
        w->state = val ? 1 : 0;
        break;
    case UAOS_GAD_SLIDER:
        if (val < w->min_val) val = w->min_val;
        if (val > w->max_val) val = w->max_val;
        w->cur_val = val;
        break;
    case UAOS_GAD_INTEGER:
        if (val < w->min_val) val = w->min_val;
        if (val > w->max_val) val = w->max_val;
        w->cur_val = val;
        /* Update text representation */
        {
            char tmp[16];
            int n = 0;
            int v = val;
            if (v < 0) { tmp[n++] = '-'; v = -v; }
            char rev[16];
            int r = 0;
            if (v == 0) rev[r++] = '0';
            while (v > 0) { rev[r++] = '0' + (v % 10); v /= 10; }
            while (r > 0) tmp[n++] = rev[--r];
            tmp[n] = '\0';
            uaos_strcpy(w->text, tmp);
            w->cursor_pos = n;
            w->sel_start = w->sel_end = n;
        }
        break;
    default:
        break;
    }
}

static inline void uaos_gui_set_str(uaos_gui_t *gui, int id, const char *str)
{
    uaos_widget_t *w = uaos_gui_find_gadget(gui, id);
    if (!w || !str) return;
    if (w->type == UAOS_GAD_STRING || w->type == UAOS_GAD_INTEGER ||
        w->type == UAOS_GAD_LABEL || w->type == UAOS_GAD_BUTTON) {
        uaos_strncpy(w->text, str, sizeof(w->text) - 1);
        w->text[sizeof(w->text) - 1] = '\0';
        if (w->type == UAOS_GAD_STRING || w->type == UAOS_GAD_INTEGER) {
            w->cursor_pos = uaos_strlen(w->text);
            w->sel_start = w->sel_end = w->cursor_pos;
        }
    }
}

static inline void uaos_gui_set_disabled(uaos_gui_t *gui, int id, int disabled)
{
    uaos_widget_t *w = uaos_gui_find_gadget(gui, id);
    if (!w) return;
    if (disabled)
        w->flags |= UAOS_NG_DISABLED;
    else
        w->flags &= ~UAOS_NG_DISABLED;
}

/* -------------------------------------------------------------------------
 * Convenience: full window clear and redraw
 * ------------------------------------------------------------------------- */
static inline void uaos_gui_clear_window(uaos_gui_t *gui)
{
    uaos_gui_fill_rect(gui->win_handle, 0, 0, 4096, 4096, UAOS_COL_WHITE);
}

static inline void uaos_gui_refresh(uaos_gui_t *gui)
{
    uaos_gui_clear_window(gui);
    uaos_gui_draw_all(gui);
}

/* -------------------------------------------------------------------------
 * Event loop helper
 *
 * Polls for an event, handles it through the widget set, and redraws.
 * Returns the gadget id that was activated, or -1 if nothing happened.
 * If *ev_out is non-NULL, the raw event is also returned.
 * ------------------------------------------------------------------------- */
static inline int uaos_gui_poll(uaos_gui_t *gui, struct uaos_gui_event *ev_out)
{
    struct uaos_gui_event ev;
    long got = uaos_gui_get_event(gui->win_handle, &ev);
    if (got <= 0) {
        if (ev_out) ev_out->type = 0;
        return -1;
    }
    if (ev_out) *ev_out = ev;
    int gad_id = uaos_gui_handle_event(gui, &ev);
    if (gad_id >= 0)
        uaos_gui_refresh(gui);
    return gad_id;
}

#endif /* UAOS_GUI_H */
