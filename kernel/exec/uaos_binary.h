/* uaos_binary.h — UAOS Executable Binary Format
 *
 * Every executable file on the UAOS filesystem begins with this header.
 * The shell and any other launcher read the magic bytes to decide how to
 * run the file:
 *
 *   UAOS_BIN_NATIVE — native x86-64 kernel command
 *       The 'name' field contains the command name (NUL-terminated, up to
 *       15 chars).  The shell looks it up in the NativeCmd registry and
 *       calls the corresponding Cmd_* function directly.
 *
 *   UAOS_BIN_M68K   — AmigaOS Amiga Hunk binary (M68k)
 *       The bytes immediately following the header are a standard Amiga
 *       Hunk binary (beginning with HUNK_HEADER = 0x000003F3).  The shell
 *       passes the payload to UAOS_Emu_LoadAndRun.
 *
 * Header layout (all fields big-endian for Amiga compatibility):
 *   Offset  Size  Field
 *   0       4     magic   "UAOS" = 0x55 0x41 0x4F 0x53
 *   4       2     type    UAOS_BIN_NATIVE (0x0001) or UAOS_BIN_M68K (0x0002)
 *   6       2     flags   reserved, must be 0
 *   8       4     payload_size   bytes of payload following the header
 *  12       16    name    NUL-padded command/program name (e.g. "dir\0")
 *  28       4     reserved  must be 0
 *  ---- total header: 32 bytes ----
 *  32+      payload_size  bytes of payload
 *
 * For NATIVE binaries the payload is empty (payload_size = 0).
 * For M68K   binaries the payload is the raw Amiga Hunk data.
 */

#ifndef UAOS_BINARY_H
#define UAOS_BINARY_H

#include <stdint.h>

#define UAOS_BIN_MAGIC      0x55414F53UL   /* "UAOS" */
#define UAOS_BIN_TYPE_NATIVE 0x0001U       /* native x86 kernel command  */
#define UAOS_BIN_TYPE_M68K   0x0002U       /* AmigaOS Amiga Hunk binary  */
#define UAOS_BIN_TYPE_X64    0x0003U       /* x86-64 ELF64 binary        */

/* Amiga Hunk header magic (raw binaries without UAOS wrapper) */
#define UAOS_BIN_HUNK_MAGIC 0x000003F3UL

#define UAOS_BIN_HEADER_SIZE 32            /* fixed header size in bytes */

/* In-memory representation (packed, big-endian on disk) */
typedef struct {
    uint8_t  magic[4];        /* "UAOS"                                */
    uint16_t type;            /* UAOS_BIN_TYPE_*  (big-endian)         */
    uint16_t flags;           /* reserved, 0                           */
    uint32_t payload_size;    /* byte count of payload (big-endian)    */
    char     name[16];        /* NUL-padded name                       */
    uint8_t  reserved[4];    /* must be zero                          */
} __attribute__((packed)) UaosBinHeader;

/* -------------------------------------------------------------------------
 * Read helpers — convert big-endian fields to native uint16/uint32
 * (The kernel is x86-64 little-endian; disk is big-endian.)
 * ------------------------------------------------------------------------- */

static inline uint16_t uaos_bin_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t uaos_bin_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* Validate magic in a raw 32-byte header buffer.
 * Returns 1 if valid UAOS binary header, 0 otherwise. */
static inline int uaos_bin_check_magic(const uint8_t *hdr)
{
    return hdr[0] == 'U' && hdr[1] == 'A' && hdr[2] == 'O' && hdr[3] == 'S';
}

#endif /* UAOS_BINARY_H */
