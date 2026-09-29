/* uibind.h — kernel backend: bind a UINode tree to a WM window
 *
 * uibind takes a declarative UI tree (uitree.h) and drives a WmWindow
 * with it: it arranges the tree into the client area on every draw,
 * renders leaf gadgets through the shared gadget set (gadgets.h), and
 * routes mouse/keyboard input to nodes, delivering UIEV_* events to the
 * application's callback.
 *
 * Gadget state lives in a Gad per interactive node inside the UIBind —
 * the UINode tree itself stays a pure description.  Rects are re-derived
 * from ui_arrange() on every draw, so window resize/zoom reflows the
 * layout for free.
 *
 * Storage model matches the kernel convention: a fixed pool of UIBind
 * structs in BSS; apps never allocate.
 */

#ifndef UAOS_UIBIND_H
#define UAOS_UIBIND_H

#include "uitree.h"
#include "gadgets.h"

#define UIBIND_MAX_WINDOWS  4    /* simultaneously bound windows       */
#define UIBIND_MAX_GADS     32   /* gadget nodes per window            */
#define UIBIND_STRBUF       64   /* backing store per string gadget    */
#define UIBIND_MARGIN       10   /* client-area margin around the tree */

/* -------------------------------------------------------------------------
 * Events delivered to the app callback.
 *   node   — the UINode that produced it (NULL for KEY/CLOSE)
 *   arg    — UIEV_KEY: the character; UIEV_RESIZE: unused (0)
 * Return value is used only for UIEV_CLOSE: non-zero allows the close
 * (the bind is then freed; the app must drop its pointer), 0 vetoes it.
 * ------------------------------------------------------------------------- */
enum {
    UIEV_CLICK = 1,   /* button activated (fires on mouse-down)     */
    UIEV_CHANGE,      /* gadget state changed (val/buf/sel/focus)   */
    UIEV_KEY,         /* keystroke no gadget consumed (arg = char)  */
    UIEV_CLOSE,       /* close-gadget request                       */
    UIEV_RESIZE,      /* window resized or zoomed (tree re-arranged)*/
};

typedef struct UIBind UIBind;
typedef int (*UIEventFn)(UIBind *b, UINode *node, int ev, int arg);

struct UIBind {
    int       used;
    int       wm;                        /* WM window handle           */
    UINode   *root;
    UIEventFn cb;
    void     *user;
    UINode   *drag;                      /* node captured while held   */
    int       ngads;
    UINode   *gnodes[UIBIND_MAX_GADS];   /* parallel to gads[]         */
    Gad       gads [UIBIND_MAX_GADS];
    char      strbuf[UIBIND_MAX_GADS][UIBIND_STRBUF];
};

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Open a WM window driven by root (root->u.text is the title).
 * x,y < 0 centres on screen; w,h <= 0 sizes the window to the tree's
 * measured natural size plus margins/titlebar.  Returns the bind or
 * NULL (pool full / >UIBIND_MAX_GADS gadget nodes / WM_AddWindow fail). */
UIBind *uibind_open(UINode *root, UIEventFn cb, void *user,
                    int x, int y, int w, int h);

/* Close the window and free the bind. */
void    uibind_close(UIBind *b);

/* 1 while the bind is live (0 after uibind_close or an allowed
 * UIEV_CLOSE). */
int     uibind_is_open(const UIBind *b);

/* WM handle, or -1. */
int     uibind_handle(const UIBind *b);

/* -------------------------------------------------------------------------
 * Live gadget state — look up by node id
 * ------------------------------------------------------------------------- */
Gad        *uibind_gad (UIBind *b, int id);          /* NULL if none    */
int         uibind_val (UIBind *b, int id);          /* g->val          */
const char *uibind_text(UIBind *b, int id);          /* string buf      */

/* Set a gadget's val and repaint (for radio nodes also clears its
 * group siblings). */
void        uibind_set_val(UIBind *b, int id, int v);

/* Damage-scope a repaint of the bound window. */
void        uibind_repaint(UIBind *b);

/* -------------------------------------------------------------------------
 * Tree helpers
 * ------------------------------------------------------------------------- */

/* Standard prefs bottom row: centred "Save / Use / Cancel" hgroup. */
UINode *ui_prefs_row(UIArena *a, int save_id, int use_id, int cancel_id);

/* Try to load a .gui description for <tool> (UAOS-127): checks
 * ENV:GUI/<tool>.gui first (runtime override), then
 * SYS:Prefs/GUI/<tool>.gui.  The file is read into a static buffer and
 * parsed into `arena`; on any failure the arena is rewound and
 * `fallback` is returned instead (may be NULL — a missing file never
 * bricks a tool).  Usage:
 *   UINode *root = uibind_load_gui(&arena, "pointer", NULL);
 *   if (!root) root = build_compiled_tree(&arena);                  */
UINode *uibind_load_gui(UIArena *arena, const char *tool, UINode *fallback);

#endif /* UAOS_UIBIND_H */
