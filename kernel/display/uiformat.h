/* uiformat.h — .gui text format: parse a UINode tree from text (UAOS-127)
 *
 * A .gui file describes the same tree the ui_*() constructors build, so a
 * tool can ship its interface as data (SYS:Prefs/GUI/<tool>.gui, overridable
 * via ENV:GUI/<tool>.gui) and iterate without recompiling.  This is also
 * the output format of the in-OS GUI designer.
 *
 * The parser is dependency-free like uitree.c: no libc, no allocation —
 * nodes and item arrays come from the caller's UIArena, strings are
 * carved out of the input buffer IN PLACE (ui_parse() writes NULs into
 * `text`; pass a mutable copy).
 *
 * ---------------------------------------------------------------------
 * Grammar (// = line comment):
 *
 *   file      := window
 *   window    := "window" STRING attrs "{" node* "}"
 *   node      := leaf ";" | group attrs "{" node* "}" | page
 *   group     := "vgroup" | "hgroup"
 *   page      := "page" attrs "{" tab* "}"
 *   tab       := "tab" STRING attrs "{" node* "}"    (implicit vgroup)
 *   leaf      := "spacer"
 *              | "label"    STRING
 *              | "button"   STRING
 *              | "checkbox" STRING
 *              | "radio"    STRING  group=N
 *              | "cycle"    STRING*          (items)
 *              | "listview" STRING*          (items, sel=N)
 *              | "slider"   RANGE            (min..max)
 *              | "integer"  RANGE
 *              | "string"   [STRING] maxchars=N
 *              | "custom"   min=W,H
 *   attrs     := ( attr | FLAG )*
 *   attr      := "id" "=" (N|NAME)      NAME -> ui_sym() hash
 *              | "weight" "=" N | "w" "=" N
 *              | "min" "=" W "," H | "max" "=" W "," H
 *              | "pad" "=" N | "spacing" "=" N
 *              | "group" "=" N | "tab" "=" STRING
 *              | "active" "=" N        (CYCLE/PAGE initial item)
 *              | "sel" "=" N           (LISTVIEW selected row)
 *              | "value" "=" N         (SLIDER/INTEGER initial)
 *              | "maxchars" "=" N      (STRING field capacity)
 *              | "text" "=" STRING     (alternate text payload)
 *   FLAG      := "disabled" | "readonly" | "toggle" | "selected"
 *              | "center" | "vertical"
 *   RANGE     := N ".." N
 *   STRING    := '"' ... '"' | '\'' ... '\''   (' accepted for shell use)
 *
 * Example:
 *
 *   window "Pointer Prefs" {
 *     vgroup spacing=6 {
 *       label "Cursor size" ;
 *       hgroup weight=1 {
 *         button "16x16" id=size16 selected ;
 *         button "32x32" id=size32 ;
 *         button "48x48" id=size48 ;
 *       }
 *       slider id=accel 0..100 value=50 ;
 *       hgroup {
 *         spacer ; button "Apply" id=apply min=80,0 ;
 *         spacer ; button "Close" id=close min=80,0 ; spacer ;
 *       }
 *     }
 *   }
 *
 * CUSTOM nodes carry only rect hints — the app attaches draw/hit callbacks
 * after parsing via ui_find(root, ui_sym("name")).
 * ---------------------------------------------------------------------
 */

#ifndef UAOS_UIFORMAT_H
#define UAOS_UIFORMAT_H

#include "uitree.h"

typedef struct {
    int  line, col;          /* 1-based error position              */
    char msg[64];            /* short diagnostic                    */
} UIParseErr;

/* Parse `text` into a UINode tree allocated from `a`.
 * MUTATES text (strings are terminated in place).  Returns the UI_WINDOW
 * root on success, NULL on error with `err` filled in (err may be NULL). */
UINode *ui_parse(UIArena *a, char *text, UIParseErr *err);

#endif /* UAOS_UIFORMAT_H */
