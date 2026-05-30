/*
 * console_device.c — UAOS console.device Implementation
 *
 * AmigaOS console.device provides console I/O operations for
 * character-based input/output. This is a native implementation for UAOS.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * console.device function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define CONSOLE_OPEN_DEVICE   1
#define CONSOLE_CLOSE_DEVICE  2
#define CONSOLE_BEGIN_IO      3
#define CONSOLE_ABORT_IO      4
#define CONSOLE_RAW_KEY       5
#define CONSOLE_READ          6
#define CONSOLE_WRITE         7
#define CONSOLE_RAW_WRITE     8

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void console_OpenDevice(void)
{
    /* OpenDevice - open console device */
    fprintf(stderr, "[CONSOLE] OpenDevice called\n");
}

static void console_CloseDevice(void)
{
    /* CloseDevice - close console device */
    fprintf(stderr, "[CONSOLE] CloseDevice called\n");
}

static void console_BeginIO(void)
{
    /* BeginIO - start I/O operation */
    fprintf(stderr, "[CONSOLE] BeginIO called\n");
}

static void console_AbortIO(void)
{
    /* AbortIO - abort I/O operation */
    fprintf(stderr, "[CONSOLE] AbortIO called\n");
}

static void console_RawKey(void)
{
    /* RawKey - read raw keyboard input */
    fprintf(stderr, "[CONSOLE] RawKey called\n");
}

static void console_Read(void)
{
    /* Read - read from console */
    fprintf(stderr, "[CONSOLE] Read called\n");
}

static void console_Write(void)
{
    /* Write - write to console */
    fprintf(stderr, "[CONSOLE] Write called\n");
}

static void console_RawWrite(void)
{
    /* RawWrite - write raw data to console */
    fprintf(stderr, "[CONSOLE] RawWrite called\n");
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *console_funcs[] = {
    console_OpenDevice,   /* index 1  */
    console_CloseDevice,  /* index 2  */
    console_BeginIO,      /* index 3  */
    console_AbortIO,      /* index 4  */
    console_RawKey,       /* index 5  */
    console_Read,          /* index 6  */
    console_Write,         /* index 7  */
    console_RawWrite,     /* index 8  */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_CONSOLE_Register(void)
{
    UAOS_ROM_Register("console.device", 40, 0x00000060,
                      (uint16_t)(sizeof(console_funcs) / sizeof(console_funcs[0])),
                      console_funcs);
}
