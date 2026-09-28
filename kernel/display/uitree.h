/* uitree.h — UAOS declarative UI tree + layout engine
 *
 * A window's interface is described as a tree of UINodes: container groups
 * (UI_VGROUP / UI_HGROUP / UI_PAGE) arrange their children along a main
 * axis, leaf nodes are gadgets (button, checkbox, string, ...) or CUSTOM
 * regions drawn by the application.
 *
 * Layout is a two-pass process:
 *   ui_measure() — bottom-up: computes each node's natural size (nat_w/h)
 *   ui_arrange() — top-down:  assigns each node a pixel rect (x/y/w/h)
 * ui_layout() runs both. Re-run it whenever the window is resized.
 *
 * The engine is dependency-free (no FB_*, WM_*, syscall or libc headers —
 * only freestanding compiler headers) so the same source builds into the
 * kernel, into libuaos userspace code, and into host-side test/design
 * tools (tools/ui_layout_test.c).
 *
 * Nodes come from a caller-supplied arena (bump allocator over a static or
 * heap buffer) via the ui_*() constructors, or may be statically allocated
 * and linked by hand — the tree functions never allocate.
 */

#ifndef UAOS_UITREE_H
#define UAOS_UITREE_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Layout constants (measured WB 3.1 metrics — see kernel/display/wm.h)
 * ------------------------------------------------------------------------- */
#define UI_CHAR_W       8    /* 8x16 system font cell width            */
#define UI_LINE_H       16   /* 8x16 system font cell height           */
#define UI_TEXT_PAD     4    /* horizontal text inset inside a gadget  */
#define UI_BUTTON_H     22   /* standard gadget row height             */
#define UI_INPUT_H      22   /* string/integer field height            */
#define UI_CHECK_H      18   /* checkbox/radio row height (14px box)   */
#define UI_CHECK_BOX    14   /* checkbox/radio box edge                */
#define UI_SLIDER_H     20   /* slider track + knob height             */
#define UI_CYCLE_ARROW  20   /* cycle gadget arrow column width        */
#define UI_PAGE_TAB_H   22   /* page container tab strip height        */
#define UI_TAB_PAD      6    /* horizontal padding inside a tab        */
#define UI_DEF_SPACING  4    /* default gap between group children     */

/* -------------------------------------------------------------------------
 * Node types
 * ------------------------------------------------------------------------- */
typedef enum {
    UI_WINDOW = 0,   /* root: single child fills the window client area */
    UI_VGROUP,       /* children split the vertical axis                */
    UI_HGROUP,       /* children split the horizontal axis              */
    UI_PAGE,         /* tabbed container: only the active child shows   */
    UI_SPACER,       /* empty stretchy space (default weight 1)         */
    UI_LABEL,        /* static text                                     */
    UI_BUTTON,       /* push button                                     */
    UI_CHECKBOX,     /* toggle box + label                              */
    UI_RADIO,        /* mutually-exclusive option (group field)         */
    UI_CYCLE,        /* label + up/down arrows cycling an item list     */
    UI_SLIDER,       /* proportional slider (horizontal default)        */
    UI_STRING,       /* single-line text input                          */
    UI_INTEGER,      /* validated numeric input                         */
    UI_LISTVIEW,     /* scrollable item list                            */
    UI_CUSTOM        /* app-drawn region via callbacks                  */
} UIType;

/* Node flags */
#define UI_F_DISABLED   0x0001  /* gadget is ghosted / unresponsive        */
#define UI_F_READONLY   0x0002  /* string/integer not editable             */
#define UI_F_TOGGLE     0x0004  /* checkbox toggles on click               */
#define UI_F_SELECTED   0x0008  /* initial checked / selected state        */
#define UI_F_CENTER     0x0010  /* keep natural size, centre inside cell   */
#define UI_F_VERTICAL   0x0020  /* slider orientation (default horizontal) */

/* -------------------------------------------------------------------------
 * Node payload structs
 * ------------------------------------------------------------------------- */
struct UINode;

typedef void (*UICustomDrawFn)(struct UINode *node, int x, int y, int w, int h,
                               void *user);
typedef int  (*UICustomHitFn)(struct UINode *node, int mx, int my, void *user);

typedef struct {           /* SLIDER */
    int min, max, cur;
} UIRange;

typedef struct {           /* STRING (text/max_chars) / INTEGER (value/min/max) */
    const char *text;
    int         max_chars;
    int         value, min, max;
} UIInput;

typedef struct {           /* CYCLE / LISTVIEW */
    const char * const *items;
    int                 count;
    int                 active;   /* CYCLE: current item */
    int                 selected; /* LISTVIEW: selected row */
    int                 top;      /* LISTVIEW: first visible row */
} UIItems;

typedef struct {           /* CUSTOM */
    UICustomDrawFn draw;
    UICustomHitFn  hit;
    void          *user;
} UICustom;

typedef struct {           /* PAGE */
    int active;            /* index of the visible child */
} UIPage;

/* -------------------------------------------------------------------------
 * UINode
 * ------------------------------------------------------------------------- */
typedef struct UINode {
    UIType   type;
    int      id;            /* app-assigned; returned on interaction       */
    int      group;         /* RADIO mutual-exclusion group                */
    int      weight;        /* share of free space on the parent's main
                               axis (0 = keep natural size)               */
    int      min_w, min_h;  /* minimum total footprint incl. pad (0=none)  */
    int      max_w, max_h;  /* maximum box size (0 = unbounded)            */
    int      pad;           /* padding inside the node's own footprint     */
    int      spacing;       /* groups only: gap between children           */
    uint32_t flags;
    const char *tab;        /* tab label when a direct child of UI_PAGE    */
    union {
        const char *text;   /* WINDOW title / LABEL / BUTTON / CHECKBOX /
                               RADIO text                                 */
        UIRange     range;  /* SLIDER                                      */
        UIInput     input;  /* STRING / INTEGER                            */
        UIItems     items;  /* CYCLE / LISTVIEW                            */
        UICustom    custom; /* CUSTOM                                      */
        UIPage      page;   /* PAGE                                        */
    } u;
    struct UINode *first_child;
    struct UINode *next_sibling;
    struct UINode *parent;
    /* layout results — written by ui_measure()/ui_arrange() */
    int      nat_w, nat_h;  /* measured natural size, incl. pad            */
    int      x, y, w, h;    /* arranged box (pad already inset)            */
} UINode;

/* -------------------------------------------------------------------------
 * Arena — bump allocator over a caller-supplied buffer
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned char *base;
    uint32_t       cap;
    uint32_t       used;
} UIArena;

void  ui_arena_init(UIArena *a, void *buf, uint32_t cap);
void *ui_arena_alloc(UIArena *a, uint32_t size);   /* NULL when exhausted */
void  ui_arena_reset(UIArena *a);

/* -------------------------------------------------------------------------
 * Constructors — all return NULL when the arena is exhausted.
 * Container constructors take a NULL-terminated list of children.
 * ------------------------------------------------------------------------- */
UINode *ui_window  (UIArena *a, const char *title, UINode *content);
UINode *ui_vgroup  (UIArena *a, ...);
UINode *ui_hgroup  (UIArena *a, ...);
UINode *ui_page    (UIArena *a, ...);      /* children annotated via ui_tab() */
UINode *ui_spacer  (UIArena *a);
UINode *ui_label   (UIArena *a, const char *text);
UINode *ui_button  (UIArena *a, const char *text, int id);
UINode *ui_checkbox(UIArena *a, const char *text, int id);
UINode *ui_radio   (UIArena *a, const char *text, int group, int id);
UINode *ui_cycle   (UIArena *a, const char * const *items, int count, int id);
UINode *ui_slider  (UIArena *a, int min, int max, int cur, int id);
UINode *ui_string  (UIArena *a, const char *text, int max_chars, int id);
UINode *ui_integer (UIArena *a, int value, int min, int max, int id);
UINode *ui_listview(UIArena *a, const char * const *items, int count, int id);
UINode *ui_custom  (UIArena *a, int min_w, int min_h,
                    UICustomDrawFn draw, void *user, int id);

/* -------------------------------------------------------------------------
 * Annotation helpers — return the node so they can wrap constructor calls:
 *   ui_pad(ui_vgroup(a, ...), 8)
 * ------------------------------------------------------------------------- */
static inline UINode *ui_w      (UINode *n, int weight)    { n->weight = weight; return n; }
static inline UINode *ui_min    (UINode *n, int w, int h)  { n->min_w = w; n->min_h = h; return n; }
static inline UINode *ui_max    (UINode *n, int w, int h)  { n->max_w = w; n->max_h = h; return n; }
static inline UINode *ui_pad    (UINode *n, int pad)       { n->pad = pad; return n; }
static inline UINode *ui_spacing(UINode *n, int px)        { n->spacing = px; return n; }
static inline UINode *ui_flags  (UINode *n, uint32_t f)    { n->flags |= f; return n; }
static inline UINode *ui_tab    (UINode *n, const char *l) { n->tab = l; return n; }

/* -------------------------------------------------------------------------
 * Tree operations
 * ------------------------------------------------------------------------- */

/* Append a child to a container (programmatic alternative to varargs). */
void    ui_append(UINode *parent, UINode *child);

/* Bottom-up measure: fills nat_w/nat_h on every node. */
void    ui_measure(UINode *root);

/* Top-down arrange: assigns x/y/w/h to every node. Requires a prior
 * ui_measure() (ui_layout() calls both). */
void    ui_arrange(UINode *root, int x, int y, int w, int h);

/* Convenience: measure + arrange in one call. Re-run on resize. */
void    ui_layout(UINode *root, int x, int y, int w, int h);

/* Depth-first lookup by node id. Returns NULL when not found. */
UINode *ui_find(UINode *root, int id);

/* Deepest node whose arranged box contains (mx,my). For UI_PAGE the tab
 * strip returns the page node itself; only the active page's children are
 * descended into. Returns NULL when the point is outside the tree. */
UINode *ui_node_at(UINode *root, int mx, int my);

/* Rect of the i-th tab of a UI_PAGE node (i.e. where a backend draws the
 * tab and hit-tests clicks). Returns 0 when index is out of range. */
int     ui_page_tab_rect(const UINode *page, int index,
                         int *x, int *y, int *w, int *h);

/* Number of direct children. */
int     ui_child_count(const UINode *n);

#endif /* UAOS_UITREE_H */
