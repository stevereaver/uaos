/*
 * diskfont_lib.c — UAOS diskfont.library Implementation (UAOS-249)
 *
 * AmigaOS diskfont.library loads fonts from disk (FONTS:).  UAOS has no
 * disk fonts — the built-in font covers topaz — so this is a stub that
 * opens like the real ROM module (diskfont has been resident since
 * Kickstart 2.0) but reports "no fonts" on every call.  Guests that
 * enumerate fonts see an empty list; OpenDiskFont returns NULL so apps
 * fall back to the built-in font.
 */

#include "rom_modules.h"
#include "../../emulation/uaos_emu.h"
#include <stdint.h>
#include <stdio.h>

/* Guest RAM (big-endian!) for buffer writes */
extern uint8_t *g_ram;

static void df_w16(uint32_t addr, uint16_t val)
{
    g_ram[addr + 0] = (uint8_t)(val >> 8);
    g_ram[addr + 1] = (uint8_t)(val      );
}

/* =========================================================================
 * diskfont.library function indices -> diskfont_funcs[]
 * ========================================================================= */

#define DISKFONT_OPEN_LIBRARY          1
#define DISKFONT_CLOSE_LIBRARY         2
#define DISKFONT_OPEN_DISK_FONT        3
#define DISKFONT_AVAIL_FONTS           4
#define DISKFONT_NEW_FONT_CONTENTS     5
#define DISKFONT_DISPOSE_FONT_CONTENTS 6
#define DISKFONT_NEW_SCALED_DISK_FONT  7

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void diskfont_OpenLibrary(M68kCPUState *cpu)
{
    /* -6 Open: generated-base catch-all already returns the base, but if
     * we are mapped explicitly return A6 (the lib base) for correctness. */
    cpu->d[0] = cpu->a[6];
}

static void diskfont_CloseLibrary(M68kCPUState *cpu)
{
    (void)cpu;
}

static void diskfont_OpenDiskFont(M68kCPUState *cpu)
{
    /* OpenDiskFont(textAttr=a0) -> D0 = struct DiskFontHeader* or NULL.
     * No disk fonts exist — NULL tells the caller the font isn't found. */
    (void)cpu;
    cpu->d[0] = 0;
}

static void diskfont_AvailFonts(M68kCPUState *cpu)
{
    /* AvailFonts(buffer=a0, bufBytes=d0, flags=d1) -> D0 = error code.
     * Report an empty font list: afh_NumEntries (UWORD) = 0. */
    uint32_t buf      = cpu->a[0];
    uint32_t bufbytes = cpu->d[0];
    if (bufbytes < 2 || buf + 2 > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;      /* buffer too small / error */
        return;
    }
    df_w16(buf, 0);
    cpu->d[0] = 0;
}

static void diskfont_NewFontContents(M68kCPUState *cpu)
{
    /* NewFontContents(fontsHeader=a0, fontName=a1) -> D0 = FontContents* */
    (void)cpu;
    cpu->d[0] = 0;
}

static void diskfont_DisposeFontContents(M68kCPUState *cpu)
{
    /* DisposeFontContents(fontContents=a1) — nothing was ever allocated */
    (void)cpu;
}

static void diskfont_NewScaledDiskFont(M68kCPUState *cpu)
{
    /* NewScaledDiskFont(sourceFont=a0, destTextAttr=a1) -> D0 or NULL */
    (void)cpu;
    cpu->d[0] = 0;
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *diskfont_funcs[] = {
    diskfont_OpenLibrary,          /* index 1 */
    diskfont_CloseLibrary,         /* index 2 */
    diskfont_OpenDiskFont,         /* index 3 */
    diskfont_AvailFonts,           /* index 4 */
    diskfont_NewFontContents,      /* index 5 */
    diskfont_DisposeFontContents,  /* index 6 */
    diskfont_NewScaledDiskFont,    /* index 7 */
};

/* Canonical diskfont.library LVOs (AmigaOS V37) -> diskfont_funcs[] */
static const UaosRomLvo diskfont_lvo_map[] = {
    {  -6, DISKFONT_OPEN_LIBRARY          },  /* Open                */
    { -12, DISKFONT_CLOSE_LIBRARY         },  /* Close               */
    { -30, DISKFONT_OPEN_DISK_FONT        },  /* OpenDiskFont        */
    { -36, DISKFONT_AVAIL_FONTS           },  /* AvailFonts          */
    { -42, DISKFONT_NEW_FONT_CONTENTS     },  /* NewFontContents     */
    { -48, DISKFONT_DISPOSE_FONT_CONTENTS },  /* DisposeFontContents */
    { -54, DISKFONT_NEW_SCALED_DISK_FONT  },  /* NewScaledDiskFont   */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_DISKFONT_Register(void)
{
    UAOS_ROM_Register("diskfont.library", 37, 0x000000C0,
                      (uint16_t)(sizeof(diskfont_funcs) / sizeof(diskfont_funcs[0])),
                      diskfont_funcs);
    UAOS_ROM_BindLvoMap("diskfont.library", diskfont_lvo_map,
                        (uint16_t)(sizeof(diskfont_lvo_map) / sizeof(diskfont_lvo_map[0])));
}
