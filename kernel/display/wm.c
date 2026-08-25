/* wm.c — UAOS Window Manager */

#include "wm.h"
#include "framebuffer.h"
#include "cursor.h"
#include "desktop.h"
#include "filebrowser.h"
#include <stdint.h>
#include <stddef.h>

/* On-screen debug from filebrowser */
extern void dbg_add_line(const char *msg);
extern void UAOS_Intuition_NotifyDepthChange(int wm_handle);

/* Debug output */
#define WM_DEBUG 1
#if WM_DEBUG
    #define WM_LOG(msg) do { extern void kprint(const char *); kprint(msg); } while(0)
    #define WM_LOG_DEC(v) do { extern void kprintdec(uint32_t); kprintdec((uint32_t)(v)); } while(0)
#else
    #define WM_LOG(msg) do {} while(0)
    #define WM_LOG_DEC(v) do {} while(0)
#endif

/* =========================================================================
 * Window registry and z-order
 * ========================================================================= */

static WmWindow g_wins[WM_MAX_WINDOWS];
static int      g_zorder[WM_MAX_WINDOWS];  /* indices into g_wins, [0]=back */
static int      g_nwins = 0;

static int g_focus    = -1;   /* index into g_wins of focused window   */

/* Set on btn_pressed when hit_test returns -1 (desktop press).
 * Desktop_MouseRelease is only forwarded when this flag is set, so that a
 * release after a window click never accidentally triggers desktop actions. */
static int g_press_was_desktop = 0;

/* Drag/resize state */
static int g_drag_handle  = -1;
static int g_drag_off_x   = 0;
static int g_drag_off_y   = 0;
static int g_resize_handle = -1;
static int g_resize_base_w = 0;
static int g_resize_base_h = 0;
static int g_resize_orig_mx = 0;
static int g_resize_orig_my = 0;
static int g_btn_left_prev  = 0;
static int g_btn_right_prev = 0;

/* Handle of the window currently being painted by WM_Redraw/repaint_window. */
int WM_CurrentDrawHandle = -1;

/* Optional palette callback invoked before each window's chrome is drawn. */
static WM_PaletteFn g_palette_fn = NULL;

/* Forward declaration — focus notification helper used by mouse/raise/lower/close. */
static void wm_notify_focus_change(int old_focus, int new_focus);

static void wm_notify_gadget_event(int wh, int event_type, int gadget_id, int mx, int my)
{
    if (wh < 0 || wh >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[wh];
    if (w->active && w->on_event)
        w->on_event(wh, event_type, gadget_id, mx, my);
}

/* Scrollbar drag state */
static int g_scroll_drag_win  = -1;  /* window whose thumb is being dragged */
static int g_scroll_drag_axis = 0;   /* 0=vert, 1=horiz */
static int g_scroll_drag_base = 0;   /* scroll value at drag start */
static int g_scroll_drag_mbase = 0;  /* mouse coord at drag start  */

/* Active gadget press tracking for IDCMP_GADGETDOWN/UP */
static int g_gadget_win  = -1;
static int g_gadget_id   = 0;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Forward declaration — needed by scroll_by which is defined before repaint_window */
static void repaint_window(int wh);

/* =========================================================================
 * Chrome geometry helpers
 * ========================================================================= */

#define SB  WM_SCROLLBAR_W   /* scrollbar / border thickness shorthand */

/* Right scrollbar rect */
static void sb_right_rect(WmWindow *w, int *x, int *y, int *wd, int *ht)
{
    *x  = w->x + w->w - SB;
    *y  = w->y + WM_TITLEBAR_H;
    *wd = SB;
    *ht = w->h - WM_TITLEBAR_H - SB;  /* full height minus title and resize grip */
    if (*ht < 0) *ht = 0;
}

/* Bottom scrollbar rect */
static void sb_bottom_rect(WmWindow *w, int *x, int *y, int *wd, int *ht)
{
    *x  = w->x + 1;
    *y  = w->y + w->h - SB;
    *wd = w->w - 1 - SB;   /* stops left of resize grip */
    *ht = SB;
}

/* Client area: WM_BORDER left inset (white/blue/black bevel), right inset = SB (scrollbar) */
static void client_rect(WmWindow *w, int *cx, int *cy, int *cw, int *ch)
{
    *cx = w->x + WM_BORDER;
    *cy = w->y + WM_TITLEBAR_H;
    *cw = w->w - WM_BORDER - SB;   /* right edge is scrollbar */
    *ch = w->h - WM_TITLEBAR_H - SB; /* bottom edge is scrollbar */
}

/* =========================================================================
 * Scrollbar drawing helpers
 * ========================================================================= */

/* Hollow (outline-only) chevron glyph, 10x5, pixel-measured from a genuine
 * AmigaOS 3.x scrollbar (cross-checked against two independent scrollbars:
 * "In" [vertical] and "Buddy" [horizontal] in the same screenshot). Each
 * row is a pair of horizontal runs (left_off,left_w / right_off,right_w
 * relative to the glyph's left edge); width 0 means "no run". Row 0 is the
 * apex end; the base (row 4) shows two separated corner stubs, confirming
 * the glyph is hollow rather than a filled triangle. */
typedef struct { int8_t left_off, left_w, right_off, right_w; } ChevronRow;
/* Hollow, symmetric 2px-thick chevron. 10x5, with both arms present from
 * the tip downward so vertical flipping (down arrow) and horizontal
 * transposition (left/right arrows) stay perfectly symmetric. */
static const ChevronRow CHEVRON_ROWS[5] = {
    { 4, 2, -1, 0 },  /* apex: 2px tip where the arms meet */
    { 3, 2,  6, 2 },  /* arms 2 thick, 1px hollow gap      */
    { 2, 2,  7, 2 },  /* arms move outward, gap widens     */
    { 1, 2,  8, 2 },  /* hollow V continues                */
    { 0, 2,  8, 2 },  /* base: two separated 2px stubs     */
};
#define CHEVRON_W 10
#define CHEVRON_H 5

/* Draw one scrollbar arrow button. dir: 0=up 1=down 2=left 3=right.
 * Reproduces the measured AmigaOS 3.x construction: the button box has a
 * white bevel line on its leading edge and a black divider/border line on
 * its trailing edge (no side bevels beyond the shared well border); the
 * arrow glyph itself is a hollow 2px-thick chevron (NOT a filled
 * triangle), centred in the button. */
static void draw_arrow(int bx, int by, int bw, int bh,
                        int dir, uint32_t bg)
{
    FB_FillRect(bx, by, bw, bh, bg);
    if (dir == 0 || dir == 1) {
        FB_DrawHLine(bx, by, bw, WB_WHITE);
        FB_DrawHLine(bx, by + bh - 1, bw, WB_BLACK);
    } else {
        FB_DrawVLine(bx, by, bh, WB_WHITE);
        FB_DrawVLine(bx + bw - 1, by, bh, WB_BLACK);
    }

    int gx, gy;
    if (dir == 0 || dir == 1) {
        gx = bx + (bw - CHEVRON_W) / 2;
        gy = by + (bh - CHEVRON_H) / 2;
    } else {
        gx = bx + (bw - CHEVRON_H) / 2;   /* transposed bounding box */
        gy = by + (bh - CHEVRON_W) / 2;
    }

    for (int r = 0; r < CHEVRON_H; r++) {
        /* dir 0 (up) / 2 (left): apex-first row order (as measured).
         * dir 1 (down) / 3 (right): mirrored (apex-last). */
        int row = (dir == 0 || dir == 2) ? r : (CHEVRON_H - 1 - r);
        const ChevronRow *cr = &CHEVRON_ROWS[row];
        if (dir == 0 || dir == 1) {
            if (cr->left_w > 0)  FB_DrawHLine(gx + cr->left_off,  gy + r, cr->left_w,  WB_BLACK);
            if (cr->right_w > 0) FB_DrawHLine(gx + cr->right_off, gy + r, cr->right_w, WB_BLACK);
        } else {
            /* Transpose: the glyph's row axis runs vertically. */
            if (cr->left_w > 0)  FB_DrawVLine(gx + r, gy + cr->left_off,  cr->left_w,  WB_BLACK);
            if (cr->right_w > 0) FB_DrawVLine(gx + r, gy + cr->right_off, cr->right_w, WB_BLACK);
        }
    }
}

/* Draw the dithered track fill for a scrollbar's "live" column/row.
 * Reproduces the measured 1px ordered checkerboard of black/grey pixels
 * (never solid), phase alternating by 1px each line, confined to the
 * live width/height passed in. axis: 0=vertical (dither varies by row),
 * 1=horizontal (dither varies by column). */
static void draw_sb_track(int x, int y, int live_w, int len, int axis)
{
    for (int i = 0; i < len; i++) {
        int phase = i & 1;
        for (int c = 0; c < live_w; c++) {
            int on = ((c + phase) & 1) == 0;
            uint32_t col = on ? WB_BLACK : WB_GREY;
            if (axis == 0) FB_PutPixel(x + c, y + i, col);
            else           FB_PutPixel(x + i, y + c, col);
        }
    }
}

/* Draw the scrollbar thumb: a HOLLOW raised-bevel box (white top+left,
 * black bottom+right, unpainted grey interior) — verified against two
 * independent real scrollbars; the thumb is not solid-filled. */
static void draw_sb_thumb(int x, int y, int w, int h)
{
    FB_FillRect(x, y, w, h, WB_GREY);
    FB_DrawHLine(x, y, w, WB_WHITE);
    FB_DrawVLine(x, y, h, WB_WHITE);
    FB_DrawHLine(x, y + h - 1, w, WB_BLACK);
    FB_DrawVLine(x + w - 1, y, h, WB_BLACK);
}

/* Width of the scrollbar's "live" column (thumb/track/arrow glyphs),
 * centred within the full well width with a grey margin on each side —
 * measured pixel-exact from a genuine screenshot (10px live column
 * centred in an 18px well, i.e. 3px margin either side). */
#define SB_LIVE_MARGIN  ((WM_SCROLLBAR_W - 10) / 2)
#define SB_LIVE_W       (WM_SCROLLBAR_W - SB_LIVE_MARGIN * 2)

/* Draw a scrollbar (vertical or horizontal).
 * track_x/y/w/h: the full scrollbar rectangle
 * arrow_sz: length of each arrow button along the scrollbar's long axis
 * scroll: current offset, content_sz: total content size, view_sz: view size
 * axis: 0=vertical, 1=horizontal */
static void draw_scrollbar(int tx, int ty, int tw, int th,
                            int arrow_sz,
                            int scroll, int content_sz, int view_sz,
                            int axis, uint32_t bg)
{
    /* Clear entire scrollbar area including full interior to prevent ANY artifacts.
     * The well is filled with the window's active/inactive title-bar colour. */
    FB_FillRect(tx, ty, tw, th, bg);

    if (axis == 0) { /* vertical */
        int track_y = ty + arrow_sz;
        int track_h = th - arrow_sz * 2;
        int live_x  = tx + SB_LIVE_MARGIN;
        if (track_h > 0)
            draw_sb_track(live_x, track_y, SB_LIVE_W, track_h, 0);

        draw_arrow(tx, ty, tw, arrow_sz, 0, bg);
        if (th >= arrow_sz * 2)
            draw_arrow(tx, ty + th - arrow_sz, tw, arrow_sz, 1, bg);

        if (track_h > 4 && content_sz > view_sz && scroll >= 0) {
            int thumb_h = track_h * view_sz / content_sz;
            if (thumb_h < 8) thumb_h = 8;
            if (thumb_h > track_h) thumb_h = track_h;
            int range = content_sz - view_sz;
            int thumb_y = track_y + (range > 0 ? (track_h - thumb_h) * scroll / range : 0);
            if (thumb_y < track_y) thumb_y = track_y;
            if (thumb_y + thumb_h > track_y + track_h) thumb_y = track_y + track_h - thumb_h;
            /* Redraw track under the thumb footprint first isn't needed —
             * draw_sb_thumb fills its own footprint before framing it. */
            draw_sb_thumb(live_x, thumb_y, SB_LIVE_W, thumb_h);
        }

        /* 1px white inner highlight / black outer shadow, matching the
         * window's right-border sizing-gadget well construction. */
        FB_DrawVLine(tx, ty, th, WB_WHITE);
        FB_DrawVLine(tx + tw - 1, ty, th, WB_BLACK);
    } else { /* horizontal */
        int track_x = tx + arrow_sz;
        int track_w = tw - arrow_sz * 2;
        int live_y  = ty + SB_LIVE_MARGIN;
        if (track_w > 0)
            draw_sb_track(track_x, live_y, SB_LIVE_W, track_w, 1);

        draw_arrow(tx, ty, arrow_sz, th, 2, bg);
        draw_arrow(tx + tw - arrow_sz, ty, arrow_sz, th, 3, bg);

        if (track_w > 4 && content_sz > view_sz) {
            int thumb_w = track_w * view_sz / content_sz;
            if (thumb_w < 8) thumb_w = 8;
            if (thumb_w > track_w) thumb_w = track_w;
            int thumb_x = track_x + (track_w - thumb_w) * scroll
                          / (content_sz - view_sz);
            draw_sb_thumb(thumb_x, live_y, thumb_w, SB_LIVE_W);
        }

        FB_DrawHLine(tx, ty, tw, WB_WHITE);
        FB_DrawHLine(tx, ty + th - 1, tw, WB_BLACK);
    }
}

/* =========================================================================
 * System gadget imagery (close/zoom/depth)
 *
 * These bitmaps were extracted pixel-for-pixel from a genuine, unscaled
 * (non-interpolated, confirmed via colour-histogram to a flat 4-colour
 * palette) AmigaOS 3.1 screenshot, not guessed or freehand drawn. Each
 * gadget occupies a WM_GADGET_W (19) x (WM_TITLEBAR_H-2) (9) cell.
 * Coordinates below are relative to that cell's top-left corner.
 *
 * AmigaOS's 3D-look system gadgets render with a black outline in both
 * active and inactive states, but only ACTIVE windows get the white/grey
 * "shine" fill inside the gadget glyph — inactive windows show the outline
 * only, with the surrounding grey title-bar colour showing through the
 * interior (verified against real screenshots of both active and inactive
 * windows). draw_chrome() below reproduces that rule exactly.
 * ========================================================================= */

/* Close gadget: 5x5 box, black outline, white 3x3 interior (only when active).
 * Box top-left is at cell-relative (7, 2). */
#define CLOSE_BOX_X   7
#define CLOSE_BOX_Y   2
#define CLOSE_BOX_S   5

static void draw_close_gadget_image(int cell_x, int cell_y, int active)
{
    int bx = cell_x + CLOSE_BOX_X;
    int by = cell_y + CLOSE_BOX_Y;
    FB_DrawRect(bx, by, CLOSE_BOX_S, CLOSE_BOX_S, WB_BLACK);
    if (active)
        FB_FillRect(bx + 1, by + 1, CLOSE_BOX_S - 2, CLOSE_BOX_S - 2, WB_WHITE);
}

/* Zoom and depth gadget glyphs, captured pixel-for-pixel (not freehand) from
 * a genuine unscaled AmigaOS 3.1 screenshot: 'K'=black outline (always
 * drawn), 'W'/'G'=white/grey fill (drawn only for the active/focused
 * window; inactive windows show background instead, matching real
 * Intuition behaviour), '.'=always background. Each row is 16 characters
 * wide, 7 rows tall, anchored at cell-relative (3,1). */
static const char *const ZOOM_GLYPH[7] = {
    "KKKKKKKKKKKKK",
    "KKWWWKK.....K",
    "KKWWWKK.....K",
    "KKKKKKK.....K",
    "K...........K",
    "K...........K",
    "KKKKKKKKKKKKK",
};
static const char *const DEPTH_GLYPH[7] = {
    "KKKKKKKKKKK....",
    "KGGGGGGGGGK....",
    "KGGGKKKKKKKKKKK",
    "KGGGKWWWWWWWWWK",
    "KKKKKWWWWWWWWWK",
    "....KWWWWWWWWWK",
    "....KKKKKKKKKKK",
};

static void draw_glyph_rows(const char *const *glyph, int rows, int cell_x, int cell_y,
                            int active, uint32_t bg, char fill_char, uint32_t fill_col)
{
    int x0 = cell_x + 3, y0 = cell_y + 1;
    for (int r = 0; r < rows; r++) {
        const char *row = glyph[r];
        for (int c = 0; row[c]; c++) {
            char ch = row[c];
            uint32_t col;
            if (ch == 'K') col = WB_BLACK;
            else if (ch == fill_char) col = active ? fill_col : bg;
            else continue; /* '.' — leave background untouched */
            FB_PutPixel(x0 + c, y0 + r, col);
        }
    }
}

/* Zoom gadget: small box (white fill when active) overlapping the top-left
 * corner of a larger hollow outline box. */
static void draw_zoom_gadget_image(int cell_x, int cell_y, int active, uint32_t bg)
{
    draw_glyph_rows(ZOOM_GLYPH, 7, cell_x, cell_y, active, bg, 'W', WB_WHITE);
}

/* Depth gadget: back box (grey fill when active) offset up-left from the
 * front box (white fill when active), matching genuine AmigaOS imagery. */
static void draw_depth_gadget_image(int cell_x, int cell_y, int active, uint32_t bg)
{
    draw_glyph_rows(DEPTH_GLYPH, 7, cell_x, cell_y, active, bg, 'G', WB_GREY);
    draw_glyph_rows(DEPTH_GLYPH, 7, cell_x, cell_y, active, bg, 'W', WB_WHITE);
}

/* Draw a single window chrome (title bar + borders + scrollbars) */
static void draw_chrome(int wh)
{
    if (g_palette_fn) g_palette_fn(wh);
    /* Always use the default host Workbench chrome palette so window borders,
     * title bars, and gadget boxes are consistent regardless of any custom
     * screen palette that may have been installed. */
    WB_InitPalette();
    WmWindow *w = &g_wins[wh];
    int focused = (wh == g_focus);
    /* Active window: solid blue drag bar with white gadget highlights.
     * Inactive window: solid grey drag bar (same as backdrop), gadgets
     * show outline only — matches genuine AmigaOS 3.1 behaviour. */
    uint32_t tbar_col = focused ? WB_BLUE : WB_GREY;
    uint32_t text_col = WB_BLACK;

    /* Outer window frame: raised bevel — white top/left, black bottom/right. */
    FB_DrawHLine(w->x, w->y, w->w, WB_WHITE);
    FB_DrawVLine(w->x, w->y, w->h, WB_WHITE);
    FB_DrawHLine(w->x, w->y + w->h - 1, w->w, WB_BLACK);
    FB_DrawVLine(w->x + w->w - 1, w->y, w->h, WB_BLACK);

    /* Title bar fill — full width inside the outer outline */
    FB_FillRect(w->x + 1, w->y + 1, w->w - 2, WM_TITLEBAR_H - 2, tbar_col);
    /* Bottom divider of the title bar */
    FB_DrawHLine(w->x + 1, w->y + WM_TITLEBAR_H - 1, w->w - 2, WB_BLACK);

    /* Close gadget cell — flush to the window's left edge. The divider is
     * the LAST column of the cell (verified: it sits at the same position
     * as the cell's own right edge, not a separate column after it). */
    int cg_x = w->x + 1;
    int cg_y = w->y + 1;
    FB_FillRect(cg_x, cg_y, WM_GADGET_W, WM_TITLEBAR_H - 2, tbar_col);
    FB_DrawVLine(cg_x + WM_GADGET_W - 1, cg_y, WM_TITLEBAR_H - 2, WB_BLACK);
    draw_close_gadget_image(cg_x, cg_y, focused);

    /* Depth + zoom gadget cells — flush to the window's right edge. Each
     * cell's divider is its own rightmost column. */
    int dg_x = w->x + w->w - 1 - WM_GADGET_W;
    int dg_y = w->y + 1;
    int zg_x = dg_x - WM_GADGET_W;
    int zg_y = w->y + 1;
    FB_FillRect(zg_x, zg_y, WM_GADGET_W * 2, WM_TITLEBAR_H - 2, tbar_col);
    FB_DrawVLine(zg_x + WM_GADGET_W - 1, zg_y, WM_TITLEBAR_H - 2, WB_BLACK);
    FB_DrawVLine(dg_x + WM_GADGET_W - 1, dg_y, WM_TITLEBAR_H - 2, WB_BLACK);
    draw_zoom_gadget_image(zg_x, zg_y, focused, tbar_col);
    draw_depth_gadget_image(dg_x, dg_y, focused, tbar_col);

    /* Title text — vertically centred in the 8px-tall Topaz-scale interior,
     * horizontally centred between the close and zoom/depth gadget cells. */
    int title_x0 = cg_x + WM_GADGET_W;
    int title_x1 = zg_x;
    if (title_x1 > title_x0) {
        FB_PutStrSmallCentred(title_x0, w->y + 1, title_x1 - title_x0,
                              WM_TITLEBAR_H - 2, w->title, text_col, tbar_col);
    }

    /* Left content border: white outer edge (already drawn above) + 2px
     * blue accent stripe + 1px black inner line = WM_BORDER (4px total).
     * Measured pixel-for-pixel from a genuine AmigaOS 3.1 screenshot. */
    {
        int bl_y0 = w->y + WM_TITLEBAR_H;
        int bl_h  = w->h - WM_TITLEBAR_H - 1;
        if (bl_h > 0) {
            FB_FillRect(w->x + 1, bl_y0, WM_BORDER - 2, bl_h, tbar_col);
            FB_DrawVLine(w->x + WM_BORDER - 1, bl_y0, bl_h, WB_BLACK);
        }
    }

    /* Window body background */
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    FB_FillRect(cx, cy, cw, ch, WB_GREY);

    /* Right scrollbar */
    int rx, ry, rw, rh;
    sb_right_rect(w, &rx, &ry, &rw, &rh);
    int sv = w->content_h > ch ? w->content_h : ch + 1;
    if (w->scroll_y < 0) w->scroll_y = 0;
    draw_scrollbar(rx, ry, rw, rh, WM_ARROW_LEN,
                   w->scroll_y, sv, ch, 0, tbar_col);

    /* Bottom scrollbar */
    int bx, by, bw, bh;
    sb_bottom_rect(w, &bx, &by, &bw, &bh);
    int sh = (w->content_w > 0) ? w->content_w : (cw + 1);
    draw_scrollbar(bx, by, bw, bh, WM_ARROW_LEN,
                   w->scroll_x, sh, cw, 1, tbar_col);

    /* Sizing gadget — bottom-right corner. Pixel-measured from a genuine
     * AmigaOS 3.x window's default sizing-gadget corner: a 1px white
     * inner highlight (top+left) and 1px black outer shadow (bottom+
     * right) framing an 16x8 "grabber" well containing a stepped diagonal
     * line plus a horizontal base bar. AmigaOS's default 4-colour
     * Workbench palette has no separate "dark grey", so the well fill and
     * grabber lines use plain grey/black. The measured corner (from a
     * plain, non-scrollbar window) was 18x10; here it's anchored to the
     * bottom of the taller WM_SCROLLBAR_W-square well used by UAOS's
     * always-on scrollbars, since the exact proportions for a
     * scrollbar-equipped window's corner weren't available to measure. */
    {
        /* 16x8 sizing-gadget glyph: black (B) outline of a right-triangle
         * grabber with white (W) interior fill.  Anchored to the bottom of
         * the 16x16 sizing-gadget well so the horizontal/vertical scrollbar
         * tracks above remain untouched. */
        static const char *const GRABBER_ROWS[8] = {
            "............BBB.",
            "..........BBWWB.",
            "........BBWWWWB.",
            "......BBWWWWWWB.",
            "....BBWWWWWWWWB.",
            "....BBBBBBBBBBB.",
            "................",
            "................",
        };
        int gx = w->x + w->w - SB;
        int gy = w->y + w->h - SB;
        FB_FillRect(gx, gy, SB, SB, tbar_col);
        FB_DrawHLine(gx, gy, SB, WB_WHITE);
        FB_DrawVLine(gx, gy, SB, WB_WHITE);
        FB_DrawHLine(gx, gy + SB - 1, SB, WB_BLACK);
        FB_DrawVLine(gx + SB - 1, gy, SB, WB_BLACK);
        /* Draw the white fill first, then the black outline on top. */
        int wellw = SB - 2, wellh = SB - 2;
        int by0 = gy + 1 + (wellh - 8);
        for (int pass = 0; pass < 2; pass++) {
            uint32_t col = (pass == 0) ? WB_WHITE : WB_BLACK;
            char match = (pass == 0) ? 'W' : 'B';
            for (int row = 0; row < 8 && row < wellh; row++) {
                const char *line = GRABBER_ROWS[row];
                for (int c = 0; c < wellw && line[c]; c++) {
                    if (line[c] == match)
                        FB_PutPixel(gx + 1 + c, by0 + row, col);
                }
            }
        }
    }
}

/* Raise window to top of z-order */
static void raise_window(int wh)
{
    /* Find it in z-order */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++) {
        if (g_zorder[i] == wh) { pos = i; break; }
    }
    if (pos < 0 || pos == g_nwins - 1) return;  /* already on top */

    /* Shift everything above it down one slot */
    for (int i = pos; i < g_nwins - 1; i++)
        g_zorder[i] = g_zorder[i + 1];
    g_zorder[g_nwins - 1] = wh;
}

/* Move window 'src' directly in front of window 'behind' in the z-order.
 * If 'behind' is not active or invalid, raise 'src' to the front instead.
 *
 * g_zorder[0] is the backmost window and g_zorder[g_nwins-1] is the frontmost.
 * "in front of behind" therefore means one slot closer to the front than behind,
 * i.e. at index behind_pos + 1 in the final array. */
static void move_in_front_of(int src, int behind)
{
    if (src < 0 || src >= WM_MAX_WINDOWS) return;
    if (!g_wins[src].active) return;

    int src_pos = -1, behind_pos = -1;
    for (int i = 0; i < g_nwins; i++) {
        if (g_zorder[i] == src) src_pos = i;
        if (g_zorder[i] == behind) behind_pos = i;
    }
    if (src_pos < 0) return;

    if (behind_pos < 0 || !g_wins[behind].active) {
        raise_window(src);
        return;
    }

    /* Already directly in front of behind? */
    if (src_pos == behind_pos + 1) return;

    /* Desired position in the final array. */
    int target = behind_pos + 1;

    /* Removing src shifts everything after src_pos down by one. If src was
     * before the target position, the target shifts down by one too. */
    if (src_pos < target) target--;

    /* Clamp to the frontmost slot. */
    if (target >= g_nwins) target = g_nwins - 1;

    /* Remove src from its current position. */
    for (int i = src_pos; i < g_nwins - 1; i++)
        g_zorder[i] = g_zorder[i + 1];

    /* Insert src at target. */
    for (int i = g_nwins - 1; i > target; i--)
        g_zorder[i] = g_zorder[i - 1];
    g_zorder[target] = src;
}

/* Hit-test: returns window handle at (mx,my) in z-order (topmost first) */
static int hit_test(int mx, int my)
{
    for (int i = g_nwins - 1; i >= 0; i--) {
        int wh = g_zorder[i];
        WmWindow *w = &g_wins[wh];
        if (!w->active) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h)
            return wh;
    }
    return -1;
}

/* Hit-test close gadget */
static int hit_close_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int cg_x = w->x + 1;
    int cg_y = w->y + 1;
    return (mx >= cg_x && mx < cg_x + WM_GADGET_W &&
            my >= cg_y && my < cg_y + (WM_TITLEBAR_H - 2));
}

/* Hit-test title bar (whole row, used for drag) */
static int hit_titlebar(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + WM_TITLEBAR_H);
}

/* Hit-test zoom gadget (second from right in title bar) */
static int hit_zoom_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int dg_x = w->x + w->w - 1 - WM_GADGET_W;
    int zg_x = dg_x - WM_GADGET_W;
    int zg_y = w->y + 1;
    return (mx >= zg_x && mx < zg_x + WM_GADGET_W &&
            my >= zg_y && my < zg_y + (WM_TITLEBAR_H - 2));
}

/* Toggle zoom: maximise to full usable screen or restore saved geometry */
static void zoom_window(int wh)
{
    WmWindow *w = &g_wins[wh];
    if (w->zoomed) {
        /* Restore */
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->w = w->restore_w;
        w->h = w->restore_h;
        w->zoomed = 0;
    } else {
        /* Save current geometry and maximise */
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
        w->x = 0;
        w->y = 20;   /* below menu bar */
        w->w = (int)g_fb.width;
        w->h = (int)g_fb.height - 20;
        w->zoomed = 1;
    }
}

/* Hit-test depth gadget (top-right of title bar) */
static int hit_depth_gadget(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int dg_x = w->x + w->w - 1 - WM_GADGET_W;
    int dg_y = w->y + 1;
    return (mx >= dg_x && mx < dg_x + WM_GADGET_W &&
            my >= dg_y && my < dg_y + (WM_TITLEBAR_H - 2));
}

/* Cycle window to back of z-order (send behind all others) */
static void depth_window(int wh)
{
    /* Find position of wh in g_zorder */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++)
        if (g_zorder[i] == wh) { pos = i; break; }
    if (pos < 0) return;

    if (pos == 0) {
        /* Already at back — bring to front */
        for (int i = 0; i < g_nwins - 1; i++)
            g_zorder[i] = g_zorder[i + 1];
        g_zorder[g_nwins - 1] = wh;
    } else {
        /* Send to back */
        for (int i = pos; i > 0; i--)
            g_zorder[i] = g_zorder[i - 1];
        g_zorder[0] = wh;
    }

    /* Focus shifts to the new topmost window */
    g_focus = g_zorder[g_nwins - 1];
    UAOS_Intuition_NotifyDepthChange(wh);
}

/* Hit-test resize grip (bottom-right SB×SB square) */
static int hit_resize_grip(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    return (mx >= w->x + w->w - SB && mx < w->x + w->w &&
            my >= w->y + w->h - SB && my < w->y + w->h);
}

/* Scroll by one unit and redraw, clamped to [0, content-view] */
static void scroll_by(int wh, int axis, int delta)
{
    WmWindow *w = &g_wins[wh];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    if (axis == 0) { /* vertical */
        int vh = (w->view_h > 0) ? w->view_h : ch;
        int max_s = (w->content_h > vh) ? w->content_h - vh : 0;
        w->scroll_y += delta;
        if (w->scroll_y < 0) w->scroll_y = 0;
        if (w->scroll_y > max_s) w->scroll_y = max_s;
    } else { /* horizontal */
        int max_s = (w->content_w > cw) ? w->content_w - cw : 0;
        w->scroll_x += delta;
        if (w->scroll_x < 0) w->scroll_x = 0;
        if (w->scroll_x > max_s) w->scroll_x = max_s;
    }
    WM_Redraw();
}

/* Hit-test scrollbar arrows/thumb, returning scroll delta or 0 */
/* Returns 1 if the click was on a scrollbar element (consumed) */
static int hit_scrollbars(int wh, int mx, int my)
{
    WmWindow *w = &g_wins[wh];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);

    /* Right (vertical) scrollbar */
    int rx, ry, rw, rh;
    sb_right_rect(w, &rx, &ry, &rw, &rh);
    if (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) {
        if (my < ry + WM_ARROW_LEN) { scroll_by(wh, 0, -16); return 1; } /* up arrow: scroll 1 line up */
        if (my >= ry + rh - WM_ARROW_LEN) { scroll_by(wh, 0, 16); return 1; } /* down arrow: scroll 1 line down */
        /* Thumb track drag start */
        g_scroll_drag_win  = wh;
        g_scroll_drag_axis = 0;
        g_scroll_drag_base = w->scroll_y;
        g_scroll_drag_mbase = my;
        return 1;
    }

    /* Bottom (horizontal) scrollbar */
    int bx, by, bw, bh;
    sb_bottom_rect(w, &bx, &by, &bw, &bh);
    if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
        if (mx < bx + WM_ARROW_LEN) { scroll_by(wh, 1, -16); return 1; } /* left arrow: small scroll */
        if (mx >= bx + bw - WM_ARROW_LEN) { scroll_by(wh, 1, 16); return 1; } /* right arrow: small scroll */
        g_scroll_drag_win  = wh;
        g_scroll_drag_axis = 1;
        g_scroll_drag_base = w->scroll_x;
        g_scroll_drag_mbase = mx;
        return 1;
    }
    return 0;
}

/* Repaint a single window in-place without full desktop repaint */
static void repaint_window(int wh)
{
    draw_chrome(wh);
    WmWindow *w = &g_wins[wh];
    if (w->draw) {
        WM_CurrentDrawHandle = wh;
        w->draw(w->x, w->y, w->w, w->h);
        WM_CurrentDrawHandle = -1;
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int WM_AddWindow(int x, int y, int w, int h, const char *title,
                 WM_DrawFn draw, WM_KeyFn on_key)
{
    if (g_nwins >= WM_MAX_WINDOWS) return -1;

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_wins[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* WM_LOG disabled — causes hang when called at runtime from non-boot context */

    WmWindow *win = &g_wins[slot];
    win->x       = x;
    win->y       = y;
    win->w       = w;
    win->h       = h;
    win->draw     = draw;
    win->on_key   = on_key;
    win->on_click   = (WM_ClickFn)0;
    win->on_move    = (WM_MouseMoveFn)0;
    win->on_release = (WM_MouseReleaseFn)0;
    win->on_event   = (WM_EventFn)0;
    win->scroll_x   = 0;
    win->scroll_y   = 0;
    win->content_w  = 0;
    win->content_h  = 0;
    win->active     = 1;
    str_copy(win->title, title, 32);

    g_zorder[g_nwins++] = slot;

    /* New window gets focus */
    g_focus = slot;

    return slot;
}

void WM_SetMouseMoveHandler(int handle, WM_MouseMoveFn on_move)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_move = on_move;
}

void WM_SetMouseReleaseHandler(int handle, WM_MouseReleaseFn on_release)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_release = on_release;
}

void WM_SetEventHandler(int handle, WM_EventFn on_event)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_event = on_event;
}

void WM_SetPaletteFn(WM_PaletteFn fn)
{
    g_palette_fn = fn;
}

void WM_MouseEvent(int mx, int my, int btn_left, int btn_right)
{
    int btn_left_pressed  = (btn_left && !g_btn_left_prev);
    int btn_left_released = (!btn_left && g_btn_left_prev);
    int btn_right_pressed  = (btn_right && !g_btn_right_prev);
    int btn_right_released = (!btn_right && g_btn_right_prev);
    g_btn_left_prev = btn_left;
    g_btn_right_prev = btn_right;

    /* When a Workbench menu is open the desktop owns all mouse input, even
     * if the dropdown overlaps a window. Without this the window hit-test
     * swallows the events and menu items stop highlighting/activating. */
    if (Desktop_IsMenuOpen()) {
        if (btn_left_pressed) {
            g_press_was_desktop = 1;
            Desktop_MouseEvent(mx, my, 1, 0);
        }
        if (btn_right_pressed)
            Desktop_MouseEvent(mx, my, 0, 1);
        if (btn_left_released) {
            if (g_press_was_desktop)
                Desktop_MouseRelease(mx, my);
            g_press_was_desktop = 0;
        }
        if (btn_right_released)
            Desktop_RightButtonRelease(mx, my);
        if (!btn_left &&
            g_drag_handle < 0 && g_resize_handle < 0 && g_scroll_drag_win < 0)
            Desktop_MouseHover(mx, my);
        return;
    }

    if (btn_left_pressed) {
        WM_LOG("[WM] Mouse press at "); WM_LOG_DEC(mx); WM_LOG(","); WM_LOG_DEC(my); WM_LOG("\n");
        int wh = hit_test(mx, my);
        g_press_was_desktop = (wh < 0);
        if (wh < 0) {
            /* Missed all windows — pass to desktop (icon hit-test / menu) */
            WM_LOG("[WM] Missed windows, sending to desktop\n");
            Desktop_MouseEvent(mx, my, 1, 0);
            return;
        }
        if (wh >= WM_MAX_WINDOWS) return;
        WM_LOG("[WM] Hit window "); WM_LOG_DEC(wh); WM_LOG("\n");
        /* Cancel pending double-click state in all browsers except the
         * one being clicked.  A stale first-click in a background browser
         * could otherwise complete as a spurious double-click the next
         * time that browser's on_click fires (e.g. clicking Clock in
         * Tools opens DEVS in the Workbench: browser underneath). */
        FileBrowser_CancelClicks(wh);

        /* Close gadget takes priority. If the window has an event hook,
         * let it veto the close (e.g. Intuition windows need IDCMP_CLOSEWINDOW). */
        if (hit_close_gadget(wh, mx, my)) {
            g_gadget_win = wh;
            g_gadget_id  = WM_GADGET_CLOSE;
            wm_notify_gadget_event(wh, WM_EVT_GADGET_DOWN, WM_GADGET_CLOSE, mx, my);
            int allow_close = 1;
            if (g_wins[wh].on_event) {
                allow_close = g_wins[wh].on_event(wh, WM_EVT_CLOSE_REQUEST, 0, 0, 0);
            }
            if (allow_close) {
                WM_CloseWindow(wh);
            }
            return;
        }

        /* Zoom gadget (UAOS extension, no standard IDCMP class).
         * Notify the window's event handler so it can apply the stored WA_Zoom
         * geometry rather than a hardcoded maximise. */
        if (hit_zoom_gadget(wh, mx, my)) {
            g_gadget_win = wh;
            g_gadget_id  = WM_GADGET_ZOOM;
            wm_notify_gadget_event(wh, WM_EVT_GADGET_DOWN, WM_GADGET_ZOOM, mx, my);
            return;
        }

        /* Depth gadget — check before focus/raise */
        if (hit_depth_gadget(wh, mx, my)) {
            g_gadget_win = wh;
            g_gadget_id  = WM_GADGET_DEPTH;
            wm_notify_gadget_event(wh, WM_EVT_GADGET_DOWN, WM_GADGET_DEPTH, mx, my);
            depth_window(wh);
            WM_Redraw();
            return;
        }

        /* Focus and raise */
        int was_focused = (wh == g_focus);
        int old_focus = g_focus;
        g_focus = wh;
        raise_window(wh);
        if (!was_focused) {
            WM_Redraw();
            wm_notify_focus_change(old_focus, g_focus);
        }

        if (hit_resize_grip(wh, mx, my)) {
            g_gadget_win = wh;
            g_gadget_id  = WM_GADGET_SIZE;
            wm_notify_gadget_event(wh, WM_EVT_GADGET_DOWN, WM_GADGET_SIZE, mx, my);
            g_resize_handle  = wh;
            g_resize_base_w  = g_wins[wh].w;
            g_resize_base_h  = g_wins[wh].h;
            g_resize_orig_mx = mx;
            g_resize_orig_my = my;
        } else if (hit_titlebar(wh, mx, my)) {
            g_gadget_win = wh;
            g_gadget_id  = WM_GADGET_DRAG;
            wm_notify_gadget_event(wh, WM_EVT_GADGET_DOWN, WM_GADGET_DRAG, mx, my);
            g_drag_handle = wh;
            g_drag_off_x  = mx - g_wins[wh].x;
            g_drag_off_y  = my - g_wins[wh].y;
        } else if (hit_scrollbars(wh, mx, my)) {
            /* consumed by scrollbar */
        } else if (g_wins[wh].on_event) {
            g_wins[wh].on_event(wh, WM_EVT_MOUSE_DOWN, 0, mx, my);
        } else if (g_wins[wh].on_click) {
            /* Save focus before the click handler; the handler may open
             * a new window which changes g_focus.  We must return after
             * on_click so the new window layout isn't immediately hit-tested
             * against the same button-press event (which could spuriously
             * trigger the new window's close gadget or other gadgets). */
            g_wins[wh].on_click(wh, mx, my);
            return;
        }
    }

    /* Right mouse button press over a window */
    if (btn_right_pressed) {
        int wh = hit_test(mx, my);
        if (wh >= 0 && g_wins[wh].on_event)
            g_wins[wh].on_event(wh, WM_EVT_MOUSE_DOWN, 1, mx, my);
    }

    /* Right-click on the desktop (not over any window) — handled by desktop
     * for Amiga-style menu activation. */
    if (btn_right_pressed) {
        int wh = hit_test(mx, my);
        if (wh < 0) {
            Desktop_MouseEvent(mx, my, 0, 1);
            return;
        }
    }

    if (btn_left && g_drag_handle >= 0) {
        WmWindow *w = &g_wins[g_drag_handle];
        int new_x = mx - g_drag_off_x;
        int new_y = my - g_drag_off_y;

        /* Keep title bar reachable: at least 32px of it must stay on screen */
        int min_visible = 32;
        if (new_x > (int)g_fb.width  - min_visible) new_x = (int)g_fb.width  - min_visible;
        if (new_x < -(w->w - min_visible))               new_x = -(w->w - min_visible);
        if (new_y < 20) new_y = 20;  /* title bar must stay below menu bar */
        /* no bottom clamp — allow window to go off the bottom */

        if (new_x != w->x || new_y != w->y) {
            w->x = new_x;
            w->y = new_y;
            WM_Redraw();
        }
    }

    if (btn_left && g_resize_handle >= 0) {
        WmWindow *w = &g_wins[g_resize_handle];
        int new_w = g_resize_base_w + (mx - g_resize_orig_mx);
        int new_h = g_resize_base_h + (my - g_resize_orig_my);

        /* Enforce minimum size: shell needs at least titlebar + inputbar + some history */
        if (new_w < 200) new_w = 200;
        if (new_h < 120) new_h = 120;
        int max_w = (int)g_fb.width  - w->x;
        int max_h = (int)g_fb.height - w->y;
        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;

        if (new_w != w->w || new_h != w->h) {
            w->w = new_w;
            w->h = new_h;
            WM_Redraw();
            if (w->on_event)
                w->on_event(g_resize_handle, WM_EVT_RESIZE, new_w, new_h, 0);
        }
    }

    /* Scrollbar thumb drag */
    if (btn_left && g_scroll_drag_win >= 0) {
        int wh = g_scroll_drag_win;
        WmWindow *w = &g_wins[wh];
        int cx, cy, cw, ch;
        client_rect(w, &cx, &cy, &cw, &ch);

        if (g_scroll_drag_axis == 0) { /* vertical */
            int rx, ry, rw, rh;
            sb_right_rect(w, &rx, &ry, &rw, &rh);
            int track_h = rh - SB * 2;
            int vh = (w->view_h > 0) ? w->view_h : ch;
            int sv = (w->content_h > 0) ? w->content_h : vh;
            if (track_h > 0 && sv > vh) {
                int dm = my - g_scroll_drag_mbase;
                int max_s = sv - vh;
                int new_s = g_scroll_drag_base + dm * max_s / track_h;
                if (new_s < 0) new_s = 0;
                if (new_s > max_s) new_s = max_s;
                if (new_s != w->scroll_y) {
                    w->scroll_y = new_s;
                    WM_Redraw();
                }
            }
        } else { /* horizontal */
            int bx, by, bw, bh;
            sb_bottom_rect(w, &bx, &by, &bw, &bh);
            int track_w = bw - SB * 2;
            int sh = (w->content_w > 0) ? w->content_w : cw;
            if (track_w > 0 && sh > cw) {
                int dm = mx - g_scroll_drag_mbase;
                int max_s = sh - cw;
                int new_s = g_scroll_drag_base + dm * max_s / track_w;
                if (new_s < 0) new_s = 0;
                if (new_s > max_s) new_s = max_s;
                if (new_s != w->scroll_x) {
                    w->scroll_x = new_s;
                    WM_Redraw();
                }
            }
        }
    }

    /* Window client-area mouse move (drag) */
    if (btn_left && g_drag_handle < 0 && g_resize_handle < 0 && g_scroll_drag_win < 0) {
        if (g_focus >= 0) {
            WmWindow *w = &g_wins[g_focus];
            if (w->active && w->on_move)
                w->on_move(g_focus, mx, my);
        }
    }

    /* General window mouse move for IDCMP forwarding */
    if (g_focus >= 0) {
        WmWindow *w = &g_wins[g_focus];
        if (w->active && w->on_event)
            w->on_event(g_focus, WM_EVT_MOUSE_MOVE, mx, my, 0);
    }

    /* Desktop icon drag — only when this gesture started on the desktop */
    if (g_press_was_desktop && btn_left &&
        g_drag_handle < 0 && g_resize_handle < 0 && g_scroll_drag_win < 0) {
        Desktop_MouseMove(mx, my, 1);
    }

    if (btn_left_released) {
        if (g_focus >= 0) {
            WmWindow *w = &g_wins[g_focus];
            if (w->active && w->on_release)
                w->on_release(g_focus, mx, my);
            if (w->active && w->on_event)
                w->on_event(g_focus, WM_EVT_MOUSE_UP, 0, mx, my);
        }
        if (g_gadget_win >= 0) {
            wm_notify_gadget_event(g_gadget_win, WM_EVT_GADGET_UP, g_gadget_id, mx, my);
            g_gadget_win = -1;
            g_gadget_id  = 0;
        }
        g_drag_handle      = -1;
        g_resize_handle    = -1;
        g_scroll_drag_win  = -1;
        /* Only forward the release to the desktop if the press also landed on
         * the desktop.  If the press hit a window, Desktop_MouseRelease must
         * not fire — it could misfire a stale g_icon_drag_idx as a double-click
         * on a desktop icon (e.g. opening Workbench:DEVS after clicking Clock). */
        if (g_press_was_desktop)
            Desktop_MouseRelease(mx, my);
        g_press_was_desktop = 0;
    }

    /* Right-button release: notify window event hook first, then desktop menu. */
    if (btn_right_released) {
        if (g_focus >= 0) {
            WmWindow *w = &g_wins[g_focus];
            if (w->active && w->on_event)
                w->on_event(g_focus, WM_EVT_MOUSE_UP, 1, mx, my);
        }
        if (g_gadget_win >= 0) {
            wm_notify_gadget_event(g_gadget_win, WM_EVT_GADGET_UP, g_gadget_id, mx, my);
            g_gadget_win = -1;
            g_gadget_id  = 0;
        }
        Desktop_RightButtonRelease(mx, my);
    }

    /* Desktop hover (e.g. menu dropdown highlighting) — allowed while the
     * right mouse button is held so menu items highlight as the cursor moves. */
    if (!btn_left &&
        g_drag_handle < 0 && g_resize_handle < 0 && g_scroll_drag_win < 0) {
        if (hit_test(mx, my) < 0)
            Desktop_MouseHover(mx, my);
    }
}

void WM_KeyEvent(char c)
{
    if (g_focus < 0) return;
    WmWindow *w = &g_wins[g_focus];
    if (w->active && w->on_key)
        w->on_key(c);
    if (w->active && w->on_event)
        w->on_event(g_focus, WM_EVT_KEY, (int)(unsigned char)c, 0, 0);
}

void WM_Redraw(void)
{
    FB_BeginDraw();

    /* Repaint full desktop backdrop only if Workbench has been loaded */
    if (Desktop_IsWorkbenchLoaded())
        Desktop_Draw();

    /* The desktop may have applied a custom Intuition screen palette. Reset
     * to the default Workbench chrome palette before drawing window borders,
     * title bars and gadgets so every window (native or Intuition) matches. */
    WB_InitPalette();

    /* Paint windows back-to-front */
    for (int i = 0; i < g_nwins; i++) {
        int wh = g_zorder[i];
        WmWindow *w = &g_wins[wh];
        if (!w->active) continue;
        draw_chrome(wh);
        if (w->draw) {
            WM_CurrentDrawHandle = wh;
            w->draw(w->x, w->y, w->w, w->h);
            WM_CurrentDrawHandle = -1;
        }
    }

    /* Workbench menu dropdown must float above all windows. */
    Desktop_DrawMenuDropdown();

    /* Cursor on top, then flip entire frame to screen in one blit */
    Cursor_Redraw();
    FB_Flip();
}

int WM_GetFocus(void)
{
    return g_focus;
}

static void wm_notify_focus_change(int old_focus, int new_focus)
{
    if (old_focus == new_focus) return;
    if (old_focus >= 0 && old_focus < WM_MAX_WINDOWS &&
        g_wins[old_focus].active && g_wins[old_focus].on_event) {
        g_wins[old_focus].on_event(old_focus, WM_EVT_FOCUS, 0, 0, 0);
    }
    if (new_focus >= 0 && new_focus < WM_MAX_WINDOWS &&
        g_wins[new_focus].active && g_wins[new_focus].on_event) {
        g_wins[new_focus].on_event(new_focus, WM_EVT_FOCUS, 1, 0, 0);
    }
}

void WM_RequestFocus(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;

    int was_focused = (handle == g_focus);
    int old_focus = g_focus;
    g_focus = handle;
    raise_window(handle);
    if (!was_focused) {
        WM_Redraw();
        wm_notify_focus_change(old_focus, g_focus);
    }
}

void WM_CloseWindow(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[handle];
    if (!w->active) return;

    /* Save footprint before deactivating */
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;

    int old_focus = g_focus;

    /* Remove from z-order array */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++) {
        if (g_zorder[i] == handle) { pos = i; break; }
    }
    if (pos >= 0) {
        for (int i = pos; i < g_nwins - 1; i++)
            g_zorder[i] = g_zorder[i + 1];
        g_nwins--;
    }

    /* Update focus to the new top window before deactivating so that
     * any focus notification hook can still read the closing window. */
    if (g_focus == handle)
        g_focus = (g_nwins > 0) ? g_zorder[g_nwins - 1] : -1;

    /* Free the slot */
    w->active = 0;

    /* Erase window footprint and repaint everything below it */
    Cursor_Hide();
    Desktop_RedrawRect(ox, oy, ow + 2, oh + 2);
    for (int i = 0; i < g_nwins; i++) {
        int wh = g_zorder[i];
        if (g_wins[wh].active) repaint_window(wh);
    }
    Cursor_Redraw();

    wm_notify_focus_change(old_focus, g_focus);
}

void WM_SetClickHandler(int handle, WM_ClickFn on_click)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].on_click = on_click;
}

void WM_SetScrollInfo(int handle, int content_w, int content_h)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].content_w = content_w;
    g_wins[handle].content_h = content_h;
}

void WM_SetScrollInfoEx(int handle, int content_w, int content_h, int view_h)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    g_wins[handle].content_w = content_w;
    g_wins[handle].content_h = content_h;
    g_wins[handle].view_h    = view_h;
}

int WM_GetScrollX(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].scroll_x;
}

int WM_GetScrollY(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].scroll_y;
}

void WM_SetScrollX(int handle, int x)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[handle];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    int sv = (w->content_w > cw) ? w->content_w : cw;
    int max_s = sv - cw;
    if (x < 0) x = 0;
    if (x > max_s) x = max_s;
    w->scroll_x = x;
}

void WM_SetScrollY(int handle, int y)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[handle];
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    /* Use content-supplied view_h if set, otherwise fall back to client ch */
    int vh = (w->view_h > 0) ? w->view_h : ch;
    int sv = (w->content_h > vh) ? w->content_h : vh;
    int max_s = sv - vh;  /* always >= 0 */
    if (y < 0) y = 0;
    if (y > max_s) y = max_s;
    w->scroll_y = y;
}

int WM_IsWindowActive(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    return g_wins[handle].active;
}

int WM_GetWindowRect(int handle, int *x, int *y, int *w, int *h)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS || !g_wins[handle].active)
        return 0;
    WmWindow *win = &g_wins[handle];
    if (x) *x = win->x;
    if (y) *y = win->y;
    if (w) *w = win->w;
    if (h) *h = win->h;
    return 1;
}

WM_DrawFn WM_GetDrawFn(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return 0;
    if (!g_wins[handle].active) return 0;
    return g_wins[handle].draw;
}

void WM_RaiseWindow(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    int old_focus = g_focus;
    raise_window(handle);
    g_focus = handle;
    wm_notify_focus_change(old_focus, g_focus);
    UAOS_Intuition_NotifyDepthChange(handle);
}

void WM_MoveWindowInFrontOf(int src, int behind)
{
    if (src < 0 || src >= WM_MAX_WINDOWS) return;
    if (!g_wins[src].active) return;
    move_in_front_of(src, behind);
    WM_Redraw();
    UAOS_Intuition_NotifyDepthChange(src);
}

void WM_LowerWindow(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;

    /* Find the window in z-order and move it to the back */
    int pos = -1;
    for (int i = 0; i < g_nwins; i++) {
        if (g_zorder[i] == handle) { pos = i; break; }
    }
    if (pos < 0) return;

    if (pos > 0) {
        for (int i = pos; i > 0; i--)
            g_zorder[i] = g_zorder[i - 1];
        g_zorder[0] = handle;
    }

    int old_focus = g_focus;
    /* Focus stays with the now-topmost window */
    g_focus = (g_nwins > 0) ? g_zorder[g_nwins - 1] : -1;
    WM_Redraw();
    wm_notify_focus_change(old_focus, g_focus);
    UAOS_Intuition_NotifyDepthChange(handle);
}

void WM_RepaintWindow(int handle)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;

    /* Repaint the affected window chrome and any overlapping windows.
     * A full redraw is simplest and guarantees correct overlap. */
    WM_Redraw();
}

void WM_SetWindowTitle(int handle, const char *title)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    if (!title) return;

    str_copy(g_wins[handle].title, title, 32);
    WM_Redraw();
}

void WM_MoveWindow(int handle, int new_x, int new_y)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    g_wins[handle].x = new_x;
    g_wins[handle].y = new_y;
    WM_Redraw();
}

void WM_SetWindowGeometry(int handle, int x, int y, int width, int height)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    WmWindow *w = &g_wins[handle];
    if (!w->active) return;
    if (width < 200) width = 200;
    if (height < 100) height = 100;
    w->x = x;
    w->y = y;
    w->w = width;
    w->h = height;
    WM_Redraw();
    if (w->on_event)
        w->on_event(handle, WM_EVT_RESIZE, w->w, w->h, 0);
}

void WM_SetWindowZoomed(int handle, int zoomed)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS) return;
    if (!g_wins[handle].active) return;
    g_wins[handle].zoomed = zoomed ? 1 : 0;
}

int WM_GetWindowTitle(int handle, char *out, int max)
{
    if (handle < 0 || handle >= WM_MAX_WINDOWS || !g_wins[handle].active)
        return 0;
    int i = 0;
    while (i < max - 1 && g_wins[handle].title[i]) {
        out[i] = g_wins[handle].title[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}
