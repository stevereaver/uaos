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

/* Workbench 3.x palette */
#define WB_GREY         FB_RGB(0xAA,0xAA,0xAA)   /* backdrop grey            */
#define WB_LIGHT_GREY   FB_RGB(0xCC,0xCC,0xCC)   /* scrollbar thumb          */
#define WB_DARK_GREY    FB_RGB(0x55,0x55,0x55)   /* shadow / borders         */
#define WB_BLACK        FB_RGB(0x00,0x00,0x00)
#define WB_WHITE        FB_RGB(0xFF,0xFF,0xFF)
#define WB_BLUE         FB_RGB(0x00,0x55,0xAA)   /* title bar / selection    */
#define WB_LIGHT_BLUE   FB_RGB(0x00,0x88,0xFF)   /* active title bar         */
#define WB_ORANGE       FB_RGB(0xFF,0x88,0x00)   /* disk icon colour         */
#define WB_CREAM        FB_RGB(0xFF,0xFF,0xCC)   /* text on dark             */
#define WB_RED          FB_RGB(0xCC,0x00,0x00)   /* error / alert            */
#define WB_GREEN        FB_RGB(0x00,0xAA,0x00)   /* success                  */

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

/* Double buffering — call BeginDraw before any drawing, Flip to push to screen */
void     FB_BeginDraw(void);
void     FB_Flip(void);
int      FB_IsDrawing(void);   /* returns 1 while back buffer is active */
uint32_t FB_GetPixel(int x, int y); /* reads from back buf if drawing, physical fb otherwise */

/* Primitive drawing --------------------------------------------------------- */
void FB_FillRect(int x, int y, int w, int h, uint32_t colour);
void FB_DrawRect(int x, int y, int w, int h, uint32_t colour);   /* outline   */
void FB_DrawHLine(int x, int y, int len, uint32_t colour);
void FB_DrawVLine(int x, int y, int len, uint32_t colour);
void FB_PutPixel(int x, int y, uint32_t colour);

/* Text rendering — 8×16 bitmap font ---------------------------------------- */
extern const uint8_t g_font8x16[95][16];
void FB_PutChar(int x, int y, char ch, uint32_t fg, uint32_t bg);
void FB_PutStr(int x, int y, const char *s, uint32_t fg, uint32_t bg);
int  FB_CharWidth(void);    /* always 8                                        */
int  FB_CharHeight(void);   /* always 16                                       */

/* Convenience: draw a string centred in a rectangle */
void FB_PutStrCentred(int rx, int ry, int rw, int rh,
                      const char *s, uint32_t fg, uint32_t bg);

#endif /* UAOS_FRAMEBUFFER_H */
