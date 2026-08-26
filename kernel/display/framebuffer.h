/* framebuffer.h — UAOS Linear Framebuffer Abstraction
 *
 * Populated from the Multiboot2 framebuffer tag (type 8) which is filled by
 * GRUB2 from the GOP (UEFI) or VESA BIOS framebuffer.
 */

#ifndef UAOS_FRAMEBUFFER_H
#define UAOS_FRAMEBUFFER_H

#include <stdint.h>

/* RGB colour helpers -------------------------------------------------------- */
#define FB_RGB(r,g,b)   (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

/* Workbench 3.x palette — runtime variables so SetPrefs can change them */
extern uint32_t WB_GREY;         /* backdrop grey            */
extern uint32_t WB_LIGHT_GREY;   /* scrollbar thumb          */
extern uint32_t WB_DARK_GREY;    /* shadow / borders         */
extern uint32_t WB_BLACK;
extern uint32_t WB_WHITE;
extern uint32_t WB_BLUE;         /* title bar / selection    */
extern uint32_t WB_LIGHT_BLUE;   /* active title bar         */
extern uint32_t WB_ORANGE;       /* disk icon colour         */
extern uint32_t WB_CREAM;        /* text on dark             */
extern uint32_t WB_RED;          /* error / alert            */
extern uint32_t WB_GREEN;        /* success                  */

/* (Re-)initialise the Workbench palette to the default values. */
void WB_InitPalette(void);

/* Framebuffer state --------------------------------------------------------- */
typedef struct {
    uint64_t  phys_addr;   /* physical base address of framebuffer             */
    uint32_t  width;       /* width in pixels                                  */
    uint32_t  height;      /* height in pixels                                 */
    uint32_t  pitch;       /* bytes per row                                    */
    uint8_t   bpp;         /* bits per pixel (we support 24 and 32)            */
    uint8_t   valid;       /* non-zero if successfully initialised             */
} FbState;

extern FbState g_fb;

/* Initialise from Multiboot2 info struct */
void FB_Init(uint32_t mb2_info_phys);

/* Double buffering — call BeginDraw before any drawing, Flip to push to screen.
 * While drawing is active, all primitives paint into a back buffer and track
 * a dirty bounding rectangle; FB_Flip memcpy's only that rectangle to VRAM. */
void     FB_BeginDraw(void);
void     FB_Flip(void);
int      FB_IsDrawing(void);   /* returns 1 while back buffer is active */
uint32_t FB_GetPixel(int x, int y); /* reads from back buf if drawing, physical fb otherwise */

/* Extend the dirty rectangle (no-op when not drawing). Used by callers that
 * touch VRAM directly while a back-buffer frame is in progress (e.g. the
 * cursor sprite drawn at frame end) so FB_Flip will copy those pixels too. */
void     FB_DirtyInclude(int x, int y, int w, int h);

/* Primitive drawing --------------------------------------------------------- */
void FB_FillRect(int x, int y, int w, int h, uint32_t colour);
void FB_DrawRect(int x, int y, int w, int h, uint32_t colour);   /* outline   */
void FB_DrawHLine(int x, int y, int len, uint32_t colour);
void FB_DrawVLine(int x, int y, int len, uint32_t colour);
void FB_PutPixel(int x, int y, uint32_t colour);

/* Blit a horizontal run of ARGB pixels (alpha in high byte; alpha==0 skips
 * the pixel).  Clipped to the framebuffer.  If `invert` is non-zero the RGB
 * channels are XORed with 0xFFFFFF before writing.  Hoisted branch version
 * of the per-pixel loops in icon_render.c. */
void FB_BlitARGB(int x, int y, int w, const uint32_t *argb, int invert);

/* Text rendering — 8×16 bitmap font ---------------------------------------- */
extern const uint8_t g_font8x16[95][16];
void FB_PutChar(int x, int y, char ch, uint32_t fg, uint32_t bg);
void FB_PutStr(int x, int y, const char *s, uint32_t fg, uint32_t bg);
int  FB_CharWidth(void);    /* always 8                                        */
int  FB_CharHeight(void);   /* always 16                                       */

/* Convenience: draw a string centred in a rectangle */
void FB_PutStrCentred(int rx, int ry, int rw, int rh,
                      const char *s, uint32_t fg, uint32_t bg);

/* Compact 8x8 font — used for authentic-scale window chrome (title bars).
 * See framebuffer.c for details on how glyphs are derived. */
void FB_PutCharSmall(int x, int y, char ch, uint32_t fg, uint32_t bg);
void FB_PutStrSmall(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void FB_PutStrSmallCentred(int rx, int ry, int rw, int rh,
                           const char *s, uint32_t fg, uint32_t bg);
int  FB_CharWidthSmall(void);   /* always 8 */
int  FB_CharHeightSmall(void);  /* always 8 */

#endif /* UAOS_FRAMEBUFFER_H */
