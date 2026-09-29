/* gadgets.h — UAOS shared kernel gadget set
 *
 * One descriptor + draw/hit/event functions covering the standard
 * Workbench-style controls.  All kernel tools render through this
 * instead of private per-window widget code.  The UINode kernel
 * backend (uibind.c) renders UI_* gadget nodes through Gad too.
 *
 * Geometry is caller-assigned: set x,y,w,h (from the layout engine or
 * by hand) then call gad_draw().  gad_event() dispatches mouse/key
 * input and mutates gadget state; it returns a GADE_* code so callers
 * know whether to redraw and/or act.
 */

#ifndef GADGETS_H
#define GADGETS_H

#include <stdint.h>

/* =========================================================================
 * Kinds
 * ========================================================================= */

enum {
    GAD_BUTTON,     /* bevelled button, centred label                 */
    GAD_CHECKBOX,   /* 14x14 box + label, toggles val on click        */
    GAD_RADIO,      /* round indicator + label, sets val=1 on click   */
    GAD_CYCLE,      /* value field + up/down arrows                   */
    GAD_SLIDER,     /* min..val..max, horizontal (or GADF_VERTICAL)   */
    GAD_STRING,     /* editable text field, cursor + focus            */
    GAD_INTEGER,    /* string field restricted to digits              */
    GAD_LABEL,      /* plain text (non-interactive)                   */
    GAD_GBOX,       /* recessed group frame + title (non-interactive) */
    GAD_LISTVIEW,   /* recessed list, click selects row -> sel        */
};

/* Flags */
#define GADF_DISABLED   0x01    /* drawn ghosted, ignores input        */
#define GADF_VERTICAL   0x02    /* slider: vertical instead of horiz   */

/* Event phases for gad_event() */
enum { GAD_DOWN, GAD_MOVE, GAD_UP, GAD_KEY };

/* Return codes from gad_event() */
enum {
    GADE_NONE,      /* not consumed / nothing changed                */
    GADE_CLICK,     /* activation click (buttons: on mouse-down)     */
    GADE_CHANGE,    /* gadget state changed (val/sel/buf/focus)      */
};

/* =========================================================================
 * Gadget descriptor
 * ========================================================================= */

typedef struct Gad {
    int  kind;
    int  x, y, w, h;           /* rect — assigned by caller/layout    */
    const char *text;          /* label / button text                 */
    int  val, min, max;        /* check/radio: 0/1; slider & int:     */
                               /* value range; cycle: choice index    */
    int  flags;                /* GADF_*                              */
    int  group;                /* radio mutual-exclusion group id     */
    const char **choices;      /* cycle / listview item strings       */
    int  nchoices;
    /* string / integer editing state — buf is caller-owned storage   */
    char *buf;
    int   buf_max;             /* capacity of buf incl. NUL           */
    int   buf_len;             /* current length                      */
    int   cursor;              /* cursor char position                */
    int   focused;
    int   sel;                 /* listview selected row               */
    int   pressed;             /* caller-managed pressed visual flag  */
} Gad;

/* =========================================================================
 * Metrics (WB 3.1 conventions)
 * ========================================================================= */

#define GAD_BTN_W     80
#define GAD_BTN_H     22
#define GAD_BTN_GAP   12
#define GAD_CHECK_SZ  14
#define GAD_LABEL_DX  20       /* check/radio label x offset          */
#define GAD_ROW_H     18       /* listview row height                 */
#define GAD_FIELD_H   22       /* string/integer field height         */
#define GAD_ARROW_W   16       /* cycle arrow column width            */

/* =========================================================================
 * API
 * ========================================================================= */

void gad_draw(const Gad *g);
int  gad_hit(const Gad *g, int mx, int my);
int  gad_event(Gad *g, int phase, int a, int b);

/* Slider helper: set val from a pointer position (mx or my). */
void gad_slider_from_mouse(Gad *g, int m);

/* Standard prefs bottom row: positions Apply/Save/Close centred. */
void gad_btn_row(Gad *apply, Gad *save, Gad *close,
                 int wx, int wy, int ww, int wh);

/* Drawing / text helpers shared by window renderers. */
void gad_bevel(int x, int y, int w, int h, int raised);
void gad_label(int x, int y, const char *s);
void gad_gbox(int x, int y, int w, int h, const char *title);
void gad_bg(int wx, int wy, int w, int h);   /* grey client fill       */

int  gad_slen(const char *s);
void gad_str_cp(char *dst, const char *src, int max);
int  gad_str_eq(const char *a, const char *b);
void gad_itoa(char *buf, int val);

#endif /* GADGETS_H */
