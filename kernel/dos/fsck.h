/*
 * fsck.h — UAOS Filesystem Check & Repair Framework
 *
 * Provides a pluggable disk/filesystem interrogation and repair layer used
 * by the `fsck` shell command.  Each supported filesystem registers a probe
 * plus optional info/check entry points; the dispatcher picks the first
 * probe that matches the on-disk structures.
 *
 * The checker operates on the raw BlockDev (sector level), independent of
 * the mounted filesystem drivers — a check must not disturb a live
 * Fat32FS/Ext4FS instance.
 *
 * Adding a new filesystem checker:
 *   1. Implement probe/info/check in kernel/dos/fsck_<fs>.c
 *   2. Add an entry to k_fsck_fs[] in fsck.c
 */

#ifndef UAOS_FSCK_H
#define UAOS_FSCK_H

#include <stdint.h>
#include "blockdev.h"

/* -------------------------------------------------------------------------
 * Check modes
 * ------------------------------------------------------------------------- */
#define FSCK_MODE_CHECK        0   /* read-only report */
#define FSCK_MODE_REPAIR       1   /* apply fixes automatically */
#define FSCK_MODE_INTERACTIVE  2   /* prompt before each fix */

/* -------------------------------------------------------------------------
 * Check context — passed to every checker entry point.
 *
 * print    : required line output callback
 * ud       : opaque user pointer handed to all callbacks
 * mode     : FSCK_MODE_*
 * verbose  : 1 = print detailed findings, 0 = summary only
 * confirm  : interactive "Fix? (y/n)" prompt — returns 1 for yes.
 *            May be NULL (interactive falls back to check-only).
 * brk      : returns non-zero once when the user requested a break (Ctrl-C).
 *            Checkers poll it in long loops and abort early.  May be NULL.
 * yield    : cooperative yield so the desktop stays responsive during long
 *            scans.  May be NULL.
 *
 * errors/warnings/fixed are accumulated by the helpers below.
 * ------------------------------------------------------------------------- */
typedef struct FsckCtx {
    BlockDev *dev;
    void    (*print)(void *ud, const char *line);
    void     *ud;
    int       mode;
    int       verbose;
    int     (*confirm)(void *ud, const char *msg);
    int     (*brk)(void *ud);
    void    (*yield)(void *ud);

    uint32_t  errors;
    uint32_t  warnings;
    uint32_t  fixed;
    int       aborted;      /* set when a break request stops a scan */
} FsckCtx;

/* -------------------------------------------------------------------------
 * Per-filesystem operations
 * ------------------------------------------------------------------------- */
typedef struct FsckFS {
    const char *name;                             /* "FAT32", "ext4", ... */
    int       (*probe)(BlockDev *dev);            /* 1 = filesystem detected */
    int       (*info)(BlockDev *dev, FsckCtx *);  /* dump geometry; may be NULL */
    int       (*check)(BlockDev *dev, FsckCtx *); /* full check; NULL = unsupported */
} FsckFS;

/* -------------------------------------------------------------------------
 * Dispatcher
 * ------------------------------------------------------------------------- */

/* Detect the filesystem on a device.  Returns the FsckFS entry or NULL. */
const FsckFS *FSCK_Detect(BlockDev *dev);

/* Detect and return a human-readable filesystem name ("FAT32", "ext4",
 * "Amiga FFS", ...).  Returns "unknown" when nothing matches. */
const char *FSCK_ProbeName(BlockDev *dev);

/* Dump filesystem geometry/metadata for a device (the INFO operation).
 * Falls back to a sector-0 hexdump when nothing is recognised. */
int FSCK_RunInfo(BlockDev *dev, FsckCtx *ctx);

/* Run a full check on a device.  Detects the filesystem and dispatches to
 * its check entry point. */
int FSCK_RunCheck(BlockDev *dev, FsckCtx *ctx);

/* Analyse a whole-disk device: partition tables (MBR/GPT/RDB) plus a
 * fallback filesystem probe for partitionless ("superfloppy") media. */
int FSCK_DiskCheck(BlockDev *dev, FsckCtx *ctx);

/* Read-only media surface scan — reads every sector in batches and reports
 * unreadable ranges.  Polls brk/yield so it stays interruptible. */
int FSCK_SurfaceScan(BlockDev *dev, FsckCtx *ctx);

/* Hex-dump a single sector (the DUMP operation). */
int FSCK_DumpSector(BlockDev *dev, uint64_t sector, FsckCtx *ctx);

/* -------------------------------------------------------------------------
 * Shared reporting helpers (implemented in fsck.c)
 * ------------------------------------------------------------------------- */

/* Emit a line verbatim. */
void fsck_out(FsckCtx *ctx, const char *line);

/* "ERROR: <msg>" — increments ctx->errors. */
void fsck_err(FsckCtx *ctx, const char *msg);

/* "WARN: <msg>" — increments ctx->warnings. */
void fsck_warn(FsckCtx *ctx, const char *msg);

/* Verbose-only note. */
void fsck_note(FsckCtx *ctx, const char *msg);

/* Decide whether a fix should be applied for the issue just reported:
 *   REPAIR       -> always yes (prints "  fixing")
 *   INTERACTIVE  -> asks via ctx->confirm
 *   CHECK        -> always no (prints "  (use REPAIR to fix)")
 * Returns 1 when the caller should apply the fix.  When it returns 1 and
 * the fix succeeds the caller does ctx->fixed++. */
int fsck_should_fix(FsckCtx *ctx);

/* Poll the break callback; returns 1 (and sets ctx->aborted) on request. */
int fsck_break(FsckCtx *ctx);

/* Cooperative yield if the callback is set. */
void fsck_yield(FsckCtx *ctx);

/* Little-endian readers (byte pointer form — safe on packed buffers). */
uint16_t fk_le16(const uint8_t *p);
uint32_t fk_le32(const uint8_t *p);
uint64_t fk_le64(const uint8_t *p);
void     fk_put16(uint8_t *p, uint16_t v);
void     fk_put32(uint8_t *p, uint32_t v);

/* Number formatting — writes into buf[max], returns buf. */
char    *fk_dec(uint64_t v, char *buf, int max);
char    *fk_hex(uint64_t v, char *buf, int max);     /* "0x..." lowercase */

/* Format "<bytes>" as a human-readable "<n> <unit>" (KiB/MiB/GiB). */
void     fk_human(uint64_t bytes, char *buf, int max);

/* Checksum helpers. */
uint32_t fk_crc32(const uint8_t *data, uint32_t len); /* IEEE 802.3 */

/* -------------------------------------------------------------------------
 * FAT32 checker (fsck_fat32.c) — registered in the fsck.c dispatch table.
 * ------------------------------------------------------------------------- */
int FSCK_FAT32_Probe(BlockDev *dev);
int FSCK_FAT32_Info(BlockDev *dev, FsckCtx *ctx);
int FSCK_FAT32_Check(BlockDev *dev, FsckCtx *ctx);

#endif /* UAOS_FSCK_H */
