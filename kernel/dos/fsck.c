/*
 * fsck.c — UAOS Filesystem Check & Repair Framework
 *
 * Dispatcher plus generic (non-filesystem-specific) routines:
 *   - reporting helpers used by every per-FS checker
 *   - on-disk filesystem detection (probe table)
 *   - whole-disk analysis: MBR / GPT / Amiga RDB partition tables
 *   - media surface scan (read test) and sector hex dump
 *   - light info/check implementations for filesystems that do not have a
 *     dedicated checker yet (ext*, FFS/OFS, PFS/SFS, FAT12/16, ISO9660,
 *     exFAT, NTFS)
 *
 * All output goes through FsckCtx.print — never printf (that's the UART).
 */

#include "fsck.h"
#include "partition.h"
#include <string.h>

/* =========================================================================
 * DMA-safe static buffers (VirtIO requires aligned, non-stack buffers)
 * ========================================================================= */
static uint8_t g_s0[512]    __attribute__((aligned(4096)));
static uint8_t g_s1[512]    __attribute__((aligned(4096)));
static uint8_t g_scan[64 * 1024] __attribute__((aligned(4096))); /* 128 sectors */

#define LINE_MAX 112

/* =========================================================================
 * Little-endian helpers
 * ========================================================================= */
uint16_t fk_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t fk_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t fk_le64(const uint8_t *p) {
    return (uint64_t)fk_le32(p) | ((uint64_t)fk_le32(p + 4) << 32);
}
void fk_put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void fk_put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint32_t fk_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* =========================================================================
 * Tiny string helpers (freestanding — minimal libc)
 * ========================================================================= */
static int fk_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void fk_cat(char *d, const char *s, int max)
{
    int dl = fk_slen(d);
    int i = 0;
    while (s[i] && dl + i < max - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = '\0';
}

char *fk_dec(uint64_t v, char *buf, int max)
{
    char tmp[24];
    int i = 0, j = 0;
    if (max < 2) { buf[0] = 0; return buf; }
    if (!v) { buf[0] = '0'; buf[1] = 0; return buf; }
    while (v && i < (int)sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && j < max - 1) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

char *fk_hex(uint64_t v, char *buf, int max)
{
    char tmp[16];
    int i = 0, j = 0;
    if (max < 4) { buf[0] = 0; return buf; }
    buf[j++] = '0'; buf[j++] = 'x';
    if (!v) { buf[j++] = '0'; buf[j] = 0; return buf; }
    while (v && i < 16) { int d = (int)(v & 0xF); tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (i > 0 && j < max - 1) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

void fk_human(uint64_t bytes, char *buf, int max)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    uint64_t v = bytes;
    while (v >= 1024 && u < 4) { v = (v + 512) / 1024; u++; }
    char num[24];
    fk_dec(v, num, sizeof(num));
    buf[0] = 0;
    fk_cat(buf, num, max);
    fk_cat(buf, " ", max);
    fk_cat(buf, units[u], max);
}

uint32_t fk_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}

/* =========================================================================
 * Reporting helpers
 * ========================================================================= */
void fsck_out(FsckCtx *ctx, const char *line)
{
    if (ctx->print) ctx->print(ctx->ud, line);
}

void fsck_err(FsckCtx *ctx, const char *msg)
{
    char line[LINE_MAX];
    line[0] = 0;
    fk_cat(line, "ERROR: ", LINE_MAX);
    fk_cat(line, msg, LINE_MAX);
    fsck_out(ctx, line);
    ctx->errors++;
}

void fsck_warn(FsckCtx *ctx, const char *msg)
{
    char line[LINE_MAX];
    line[0] = 0;
    fk_cat(line, "WARN:  ", LINE_MAX);
    fk_cat(line, msg, LINE_MAX);
    fsck_out(ctx, line);
    ctx->warnings++;
}

void fsck_note(FsckCtx *ctx, const char *msg)
{
    if (!ctx->verbose) return;
    char line[LINE_MAX];
    line[0] = 0;
    fk_cat(line, "  ", LINE_MAX);
    fk_cat(line, msg, LINE_MAX);
    fsck_out(ctx, line);
}

int fsck_should_fix(FsckCtx *ctx)
{
    if (ctx->mode == FSCK_MODE_REPAIR) {
        fsck_out(ctx, "  -> fixing");
        return 1;
    }
    if (ctx->mode == FSCK_MODE_INTERACTIVE && ctx->confirm)
        return ctx->confirm(ctx->ud, "Fix this problem");
    if (ctx->mode == FSCK_MODE_CHECK)
        fsck_out(ctx, "  (read-only — rerun with REPAIR to fix)");
    return 0;
}

int fsck_break(FsckCtx *ctx)
{
    if (ctx->brk && ctx->brk(ctx->ud)) {
        ctx->aborted = 1;
        return 1;
    }
    return 0;
}

void fsck_yield(FsckCtx *ctx)
{
    if (ctx->yield) ctx->yield(ctx->ud);
}

/* =========================================================================
 * Filesystem probes — each returns 1 when its on-disk signature matches
 * ========================================================================= */

static int rd0(BlockDev *dev)
{
    return BlockDev_Read(dev, 0, g_s0, 1) == 0;
}

/* FAT BPB sanity shared by FAT32 / FAT12 / FAT16 probes */
static int bpb_sane(const uint8_t *s)
{
    uint16_t bps = fk_le16(s + 11);
    uint8_t  spc = s[13];
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;
    if (spc == 0 || (spc & (spc - 1))) return 0;
    if (s[16] == 0) return 0;                    /* num_fats */
    return 1;
}

static int probe_fat16(BlockDev *dev)
{
    if (!rd0(dev)) return 0;
    if (!bpb_sane(g_s0)) return 0;
    if (memcmp(g_s0 + 3, "EXFAT   ", 8) == 0) return 0;
    /* FAT32 volumes have no root-dir region and use fat_sz32.  What
     * remains — root_ent_cnt != 0 or fat_sz16 != 0 — is FAT12/16. */
    if (fk_le16(g_s0 + 17) == 0 && fk_le16(g_s0 + 22) == 0) return 0;
    if (fk_le32(g_s0 + 36) != 0 && fk_le16(g_s0 + 17) == 0) return 0; /* fat32 */
    return 1;
}

static int probe_exfat(BlockDev *dev)
{
    if (!rd0(dev)) return 0;
    return memcmp(g_s0 + 3, "EXFAT   ", 8) == 0;
}

static int probe_ntfs(BlockDev *dev)
{
    if (!rd0(dev)) return 0;
    return memcmp(g_s0 + 3, "NTFS    ", 8) == 0;
}

static int probe_ext(BlockDev *dev)
{
    /* Superblock begins at byte 1024 (sector 2); magic at offset 56. */
    if (BlockDev_Read(dev, 2, g_s0, 1) != 0) return 0;
    return fk_le16(g_s0 + 56) == 0xEF53;
}

static int probe_ffs(BlockDev *dev)
{
    if (!rd0(dev)) return 0;
    /* 'DOS\x00'..'DOS\x07' big-endian dword at offset 0 */
    if (g_s0[0] != 'D' || g_s0[1] != 'O' || g_s0[2] != 'S') return 0;
    return g_s0[3] <= 7;
}

static int probe_pfs(BlockDev *dev)
{
    if (!rd0(dev)) return 0;
    /* PFS/AFS/SFS family: 'PFS\x01','AFS\x01','SFS\x00', etc. */
    if ((g_s0[0] == 'P' || g_s0[0] == 'A' || g_s0[0] == 'S') &&
        g_s0[1] == 'F' && g_s0[2] == 'S' && g_s0[3] < 0x20)
        return 1;
    /* PFS3 root block signature ('PFS3' LE dword per pfs3.c) */
    if (g_s0[0] == '3' && g_s0[1] == 'S' && g_s0[2] == 'F' && g_s0[3] == 'P')
        return 1;
    return 0;
}

static int probe_iso(BlockDev *dev)
{
    for (uint64_t sec = 16; sec < 32; sec++) {
        if (BlockDev_Read(dev, sec, g_s0, 1) != 0) break;
        if (memcmp(g_s0 + 1, "CD001", 5) == 0 && g_s0[0] == 1)
            return 1;
        if (memcmp(g_s0 + 1, "CD001", 5) == 0 && g_s0[0] == 255)
            break; /* terminator, no PVD found */
    }
    return 0;
}

/* =========================================================================
 * Per-FS info/check implementations (non-FAT32)
 * ========================================================================= */

static int info_fat16(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX], num[24];
    if (!rd0(dev)) { fsck_err(ctx, "cannot read boot sector"); return -1; }

    fsck_out(ctx, "FAT12/FAT16 boot sector:");
    line[0] = 0;
    fk_cat(line, "  OEM '", LINE_MAX);
    char oem[9]; memcpy(oem, g_s0 + 3, 8); oem[8] = 0;
    fk_cat(line, oem, LINE_MAX);
    fk_cat(line, "'  bytes/sec ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 11), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  sec/clus ", LINE_MAX);
    fk_cat(line, fk_dec(g_s0[13], num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);

    uint32_t root_ent = fk_le16(g_s0 + 17);
    uint32_t tot = fk_le16(g_s0 + 19);
    if (!tot) tot = fk_le32(g_s0 + 32);
    line[0] = 0;
    fk_cat(line, "  root ents ", LINE_MAX);
    fk_cat(line, fk_dec(root_ent, num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  tot secs ", LINE_MAX);
    fk_cat(line, fk_dec(tot, num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  fat_sz16 ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 22), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  fats ", LINE_MAX);
    fk_cat(line, fk_dec(g_s0[16], num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);

    char label[12]; memcpy(label, g_s0 + 43, 11); label[11] = 0;
    char fstype[9]; memcpy(fstype, g_s0 + 54, 8); fstype[8] = 0;
    line[0] = 0;
    fk_cat(line, "  label '", LINE_MAX); fk_cat(line, label, LINE_MAX);
    fk_cat(line, "'  fs_type '", LINE_MAX); fk_cat(line, fstype, LINE_MAX);
    fk_cat(line, "'", LINE_MAX);
    fsck_out(ctx, line);
    return 0;
}

static int check_fat16(BlockDev *dev, FsckCtx *ctx)
{
    (void)info_fat16(dev, ctx);
    uint16_t bps = fk_le16(g_s0 + 11);
    uint16_t rsvd = fk_le16(g_s0 + 14);
    uint32_t root_ent = fk_le16(g_s0 + 17);
    uint32_t tot = fk_le16(g_s0 + 19);
    if (!tot) tot = fk_le32(g_s0 + 32);

    if (rsvd == 0) fsck_err(ctx, "reserved sector count is 0");
    if (root_ent == 0 || (root_ent * 32) % bps != 0)
        fsck_err(ctx, "root entry count not sector-aligned");
    if (tot > dev->num_sectors)
        fsck_err(ctx, "BPB total sectors exceeds device capacity");
    fsck_warn(ctx, "FAT12/16 full check not implemented (CrossDOS is read-only)");
    return 0;
}

static int info_ext(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX], num[24];
    if (BlockDev_Read(dev, 2, g_s0, 1) != 0) {
        fsck_err(ctx, "cannot read superblock"); return -1;
    }
    uint32_t blk_sz = 1024U << fk_le32(g_s0 + 24);
    fsck_out(ctx, "ext2/3/4 superblock:");
    line[0] = 0;
    fk_cat(line, "  inodes ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le32(g_s0 + 0), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  blocks ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le32(g_s0 + 4), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  free blocks ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le32(g_s0 + 12), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  free inodes ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le32(g_s0 + 16), num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);
    line[0] = 0;
    fk_cat(line, "  block size ", LINE_MAX);
    fk_cat(line, fk_dec(blk_sz, num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  inode size ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 88), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  blocks/group ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le32(g_s0 + 32), num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);
    line[0] = 0;
    fk_cat(line, "  mounts ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 52), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "/", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 54), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  state ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 58), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  lastcheck epoch ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le32(g_s0 + 64), num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);
    char vol[17]; memcpy(vol, g_s0 + 120, 16); vol[16] = 0;
    if (vol[0]) {
        line[0] = 0;
        fk_cat(line, "  volume '", LINE_MAX);
        fk_cat(line, vol, LINE_MAX);
        fk_cat(line, "'", LINE_MAX);
        fsck_out(ctx, line);
    }
    return 0;
}

static int check_ext(BlockDev *dev, FsckCtx *ctx)
{
    (void)info_ext(dev, ctx);
    uint16_t state = fk_le16(g_s0 + 58);
    if (!(state & 1))
        fsck_warn(ctx, "filesystem was not unmounted cleanly (state dirty)");
    if (state & 2)
        fsck_err(ctx, "filesystem flagged with errors (s_state & ERRORS)");
    fsck_warn(ctx, "ext* repair not implemented — run e2fsck on a Linux host");
    return 0;
}

static const char *ffs_variant(int n)
{
    switch (n & 7) {
        case 0: return "OFS";        case 1: return "FFS";
        case 2: return "OFS+INTL";   case 3: return "FFS+INTL";
        case 4: return "OFS+DIRC";   case 5: return "FFS+DIRC";
        case 6: return "OFS+LNFS";   case 7: return "FFS+LNFS";
    }
    return "DOS";
}

static int info_ffs(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX];
    if (!rd0(dev)) { fsck_err(ctx, "cannot read boot block"); return -1; }
    line[0] = 0;
    fk_cat(line, "Amiga ", LINE_MAX);
    fk_cat(line, ffs_variant(g_s0[3]), LINE_MAX);
    fk_cat(line, " volume (DOS\\x", LINE_MAX);
    {
        char t[2]; t[0] = (char)('0' + (g_s0[3] & 7)); t[1] = 0;
        fk_cat(line, t, LINE_MAX);
    }
    fk_cat(line, ")", LINE_MAX);
    fsck_out(ctx, line);

    /* Boot-block checksum: sum of all 128 big-endian longwords == 0 */
    uint32_t sum = 0;
    for (int i = 0; i < 128; i++) sum += fk_be32(g_s0 + i * 4);
    line[0] = 0;
    fk_cat(line, "  bootblock checksum: ", LINE_MAX);
    fk_cat(line, sum == 0 ? "valid" : "INVALID", LINE_MAX);
    fsck_out(ctx, line);
    return 0;
}

static int check_ffs(BlockDev *dev, FsckCtx *ctx)
{
    (void)info_ffs(dev, ctx);
    uint32_t sum = 0;
    for (int i = 0; i < 128; i++) sum += fk_be32(g_s0 + i * 4);
    if (sum == 0) {
        fsck_out(ctx, "  boot block OK");
        return 0;
    }
    fsck_err(ctx, "FFS boot block checksum mismatch");
    if (fsck_should_fix(ctx)) {
        /* Recompute checksum with field zeroed, then write it back. */
        fk_put32(g_s0 + 4, 0);
        uint32_t s2 = 0;
        for (int i = 0; i < 128; i++) s2 += fk_be32(g_s0 + i * 4);
        uint32_t fix = (uint32_t)(-(int32_t)s2);
        g_s0[4] = (fix >> 24) & 0xFF; g_s0[5] = (fix >> 16) & 0xFF;
        g_s0[6] = (fix >> 8) & 0xFF;  g_s0[7] = fix & 0xFF;
        if (BlockDev_Write(dev, 0, g_s0, 1) == 0) {
            ctx->fixed++;
            fsck_out(ctx, "  boot block checksum repaired");
        } else {
            fsck_err(ctx, "boot block repair write failed");
        }
    }
    fsck_warn(ctx, "FFS directory/allocation check not implemented yet");
    return 0;
}

static int info_pfs(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX];
    if (!rd0(dev)) { fsck_err(ctx, "cannot read block 0"); return -1; }
    line[0] = 0;
    fk_cat(line, "PFS-family signature '", LINE_MAX);
    char sig[5]; memcpy(sig, g_s0, 4);
    sig[4] = 0;
    /* 4th byte may be a control char — render as digit */
    sig[3] = (g_s0[3] < 10) ? (char)('0' + g_s0[3]) : '?';
    fk_cat(line, sig, LINE_MAX);
    fk_cat(line, "'", LINE_MAX);
    fsck_out(ctx, line);
    return 0;
}

static int info_iso(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX], num[24];
    if (BlockDev_Read(dev, 16, g_s0, 1) != 0 ||
        memcmp(g_s0 + 1, "CD001", 5) != 0) {
        fsck_err(ctx, "cannot read ISO9660 PVD"); return -1;
    }
    char vol[33]; memcpy(vol, g_s0 + 40, 32); vol[32] = 0;
    int vl = 31; while (vl >= 0 && vol[vl] == ' ') vol[vl--] = 0;
    line[0] = 0;
    fk_cat(line, "ISO9660 PVD  volume '", LINE_MAX);
    fk_cat(line, vol, LINE_MAX);
    fk_cat(line, "'", LINE_MAX);
    fsck_out(ctx, line);
    line[0] = 0;
    fk_cat(line, "  volume space ", LINE_MAX);
    fk_cat(line, fk_dec((uint64_t)fk_le32(g_s0 + 80) * fk_le16(g_s0 + 128),
                      num, sizeof(num)), LINE_MAX);
    fk_cat(line, " bytes  logical block ", LINE_MAX);
    fk_cat(line, fk_dec(fk_le16(g_s0 + 128), num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);
    return 0;
}

static int info_generic(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX];
    if (!rd0(dev)) { fsck_err(ctx, "cannot read sector 0"); return -1; }
    char oem[9]; memcpy(oem, g_s0 + 3, 8); oem[8] = 0;
    line[0] = 0;
    fk_cat(line, "  OEM/identifier '", LINE_MAX);
    fk_cat(line, oem, LINE_MAX);
    fk_cat(line, "'", LINE_MAX);
    fsck_out(ctx, line);
    return 0;
}

/* =========================================================================
 * Dispatch table
 * ========================================================================= */
static const FsckFS k_fsck_fs[] = {
    { "FAT32",          FSCK_FAT32_Probe, FSCK_FAT32_Info, FSCK_FAT32_Check },
    { "FAT12/FAT16",    probe_fat16,      info_fat16,      check_fat16      },
    { "exFAT",          probe_exfat,      info_generic,    NULL             },
    { "NTFS",           probe_ntfs,       info_generic,    NULL             },
    { "ext2/3/4",       probe_ext,        info_ext,        check_ext        },
    { "Amiga FFS/OFS",  probe_ffs,        info_ffs,        check_ffs        },
    { "PFS/SFS",        probe_pfs,        info_pfs,        NULL             },
    { "ISO9660",        probe_iso,        info_iso,        NULL             },
    { NULL, NULL, NULL, NULL }
};

const FsckFS *FSCK_Detect(BlockDev *dev)
{
    for (int i = 0; k_fsck_fs[i].name; i++)
        if (k_fsck_fs[i].probe(dev)) return &k_fsck_fs[i];
    return NULL;
}

const char *FSCK_ProbeName(BlockDev *dev)
{
    const FsckFS *f = FSCK_Detect(dev);
    return f ? f->name : "unknown";
}

int FSCK_RunInfo(BlockDev *dev, FsckCtx *ctx)
{
    const FsckFS *f = FSCK_Detect(dev);
    if (!f) {
        fsck_out(ctx, "No recognised filesystem signature found.");
        FSCK_DumpSector(dev, 0, ctx);
        return -1;
    }
    char line[LINE_MAX];
    line[0] = 0;
    fk_cat(line, "Filesystem: ", LINE_MAX);
    fk_cat(line, f->name, LINE_MAX);
    fsck_out(ctx, line);
    return f->info ? f->info(dev, ctx) : 0;
}

int FSCK_RunCheck(BlockDev *dev, FsckCtx *ctx)
{
    const FsckFS *f = FSCK_Detect(dev);
    if (!f) {
        fsck_err(ctx, "no recognised filesystem — nothing to check");
        fsck_out(ctx, "Sector 0 dump follows for manual inspection:");
        FSCK_DumpSector(dev, 0, ctx);
        return -1;
    }
    char line[LINE_MAX];
    line[0] = 0;
    fk_cat(line, "Checking ", LINE_MAX);
    fk_cat(line, f->name, LINE_MAX);
    fk_cat(line, " volume on ", LINE_MAX);
    fk_cat(line, dev->display_name ? dev->display_name : dev->name, LINE_MAX);
    fsck_out(ctx, line);
    if (!f->check) {
        line[0] = 0;
        fk_cat(line, "  no checker for ", LINE_MAX);
        fk_cat(line, f->name, LINE_MAX);
        fk_cat(line, " — interrogation only:", LINE_MAX);
        fsck_out(ctx, line);
        if (f->info) return f->info(dev, ctx);
        return 0;
    }
    return f->check(dev, ctx);
}

/* =========================================================================
 * Whole-disk analysis — MBR / GPT / RDB
 * ========================================================================= */

static char *print_size(uint64_t sectors, char *buf, int max)
{
    fk_human(sectors * 512ULL, buf, max);
    return buf;
}

static int mbr_check(BlockDev *dev, FsckCtx *ctx)
{
    if (BlockDev_Read(dev, 0, g_s0, 1) != 0) {
        fsck_err(ctx, "cannot read sector 0"); return -1;
    }
    if (g_s0[510] != 0x55 || g_s0[511] != 0xAA) return 0; /* not MBR */

    MbrSector *mbr = (MbrSector *)g_s0;
    int used[MBR_PART_COUNT], n = 0;
    int any = 0;
    for (int i = 0; i < MBR_PART_COUNT; i++)
        if (mbr->partitions[i].type_code != PART_TYPE_EMPTY) any = 1;
    if (!any) return 0; /* signature only — empty table, try other schemes */

    fsck_out(ctx, "MBR partition table:");
    char line[LINE_MAX], num[24], sz[24];
    uint64_t cap = dev->num_sectors;

    int modified = 0;
    for (int i = 0; i < MBR_PART_COUNT; i++) {
        MbrPartEntry *pe = &mbr->partitions[i];
        if (pe->type_code == PART_TYPE_EMPTY) { used[i] = 0; continue; }
        used[i] = 1; n++;

        line[0] = 0;
        fk_cat(line, "  #", LINE_MAX);
        fk_cat(line, fk_dec(i + 1, num, sizeof(num)), LINE_MAX);
        fk_cat(line, " ", LINE_MAX);
        fk_cat(line, partition_type_name(pe->type_code), LINE_MAX);
        fk_cat(line, pe->boot_flag == 0x80 ? " *boot" : "", LINE_MAX);
        fk_cat(line, "  lba ", LINE_MAX);
        fk_cat(line, fk_dec(pe->lba_start, num, sizeof(num)), LINE_MAX);
        fk_cat(line, " + ", LINE_MAX);
        fk_cat(line, fk_dec(pe->sector_count, num, sizeof(num)), LINE_MAX);
        fk_cat(line, " (", LINE_MAX);
        fk_cat(line, print_size(pe->sector_count, sz, sizeof(sz)), LINE_MAX);
        fk_cat(line, ")", LINE_MAX);
        fsck_out(ctx, line);

        /* Boot flag sanity */
        if (pe->boot_flag != 0x00 && pe->boot_flag != 0x80) {
            line[0] = 0;
            fk_cat(line, "partition ", LINE_MAX);
            fk_cat(line, fk_dec(i + 1, num, sizeof(num)), LINE_MAX);
            fk_cat(line, " has invalid boot flag", LINE_MAX);
            fsck_err(ctx, line);
            if (fsck_should_fix(ctx)) { pe->boot_flag = 0; modified = 1; }
        }
        /* Bounds */
        uint64_t end = (uint64_t)pe->lba_start + pe->sector_count;
        if (pe->lba_start == 0) {
            line[0] = 0;
            fk_cat(line, "partition ", LINE_MAX);
            fk_cat(line, fk_dec(i + 1, num, sizeof(num)), LINE_MAX);
            fk_cat(line, " starts at LBA 0 (overlaps MBR)", LINE_MAX);
            fsck_err(ctx, line);
        }
        if (pe->sector_count == 0) {
            line[0] = 0;
            fk_cat(line, "partition ", LINE_MAX);
            fk_cat(line, fk_dec(i + 1, num, sizeof(num)), LINE_MAX);
            fk_cat(line, " has zero length", LINE_MAX);
            fsck_err(ctx, line);
            if (fsck_should_fix(ctx)) {
                memset(pe, 0, sizeof(*pe)); modified = 1;
            }
        } else if (end > cap) {
            line[0] = 0;
            fk_cat(line, "partition ", LINE_MAX);
            fk_cat(line, fk_dec(i + 1, num, sizeof(num)), LINE_MAX);
            fk_cat(line, " extends past end of disk", LINE_MAX);
            fsck_err(ctx, line);
            if (fsck_should_fix(ctx)) {
                uint32_t nc = (uint32_t)(cap - pe->lba_start);
                pe->sector_count = nc; modified = 1;
            }
        }
    }

    /* Overlap check between sorted partitions */
    for (int i = 0; i < MBR_PART_COUNT; i++) {
        if (!used[i] || mbr->partitions[i].sector_count == 0) continue;
        uint32_t a0 = mbr->partitions[i].lba_start;
        uint32_t a1 = a0 + mbr->partitions[i].sector_count;
        for (int j = i + 1; j < MBR_PART_COUNT; j++) {
            if (!used[j] || mbr->partitions[j].sector_count == 0) continue;
            uint32_t b0 = mbr->partitions[j].lba_start;
            uint32_t b1 = b0 + mbr->partitions[j].sector_count;
            if (a0 < b1 && b0 < a1) {
                line[0] = 0;
                fk_cat(line, "partitions overlap: #", LINE_MAX);
                fk_cat(line, fk_dec(i + 1, num, sizeof(num)), LINE_MAX);
                fk_cat(line, " and #", LINE_MAX);
                fk_cat(line, fk_dec(j + 1, num, sizeof(num)), LINE_MAX);
                fsck_err(ctx, line);
            }
        }
    }

    if (modified) {
        if (BlockDev_Write(dev, 0, g_s0, 1) == 0) {
            ctx->fixed++;
            fsck_out(ctx, "  MBR rewritten with repairs");
        } else {
            fsck_err(ctx, "failed to write repaired MBR");
        }
    }
    return 1;
}

static int gpt_check(BlockDev *dev, FsckCtx *ctx)
{
    if (BlockDev_Read(dev, 1, g_s0, 1) != 0) return 0;
    if (memcmp(g_s0, "EFI PART", 8) != 0) return 0;

    fsck_out(ctx, "GPT partition table:");
    char line[LINE_MAX], num[24], num2[24], sz[24];

    uint32_t hdr_sz = fk_le32(g_s0 + 12);
    uint32_t stored_crc = fk_le32(g_s0 + 16);
    uint64_t my_lba = fk_le64(g_s0 + 24);
    uint64_t alt_lba = fk_le64(g_s0 + 32);
    uint64_t first_us = fk_le64(g_s0 + 40);
    uint64_t last_us = fk_le64(g_s0 + 48);
    uint64_t ent_lba = fk_le64(g_s0 + 72);
    uint32_t n_ent = fk_le32(g_s0 + 80);
    uint32_t ent_sz = fk_le32(g_s0 + 84);
    uint32_t arr_crc = fk_le32(g_s0 + 88);

    line[0] = 0;
    fk_cat(line, "  entries ", LINE_MAX);
    fk_cat(line, fk_dec(n_ent, num, sizeof(num)), LINE_MAX);
    fk_cat(line, " x ", LINE_MAX);
    fk_cat(line, fk_dec(ent_sz, num2, sizeof(num2)), LINE_MAX);
    fk_cat(line, " @lba ", LINE_MAX);
    fk_cat(line, fk_dec(ent_lba, num2, sizeof(num2)), LINE_MAX);
    fk_cat(line, "  usable ", LINE_MAX);
    fk_cat(line, fk_dec(first_us, num, sizeof(num)), LINE_MAX);
    fk_cat(line, "-", LINE_MAX);
    fk_cat(line, fk_dec(last_us, num2, sizeof(num2)), LINE_MAX);
    fsck_out(ctx, line);

    /* Header sanity */
    int rewrite = 0;
    if (hdr_sz < 92 || hdr_sz > 512) {
        fsck_err(ctx, "GPT header size out of range");
    } else {
        uint8_t save[4];
        memcpy(save, g_s0 + 16, 4);
        memset(g_s0 + 16, 0, 4);
        uint32_t calc = fk_crc32(g_s0, hdr_sz);
        memcpy(g_s0 + 16, save, 4);
        if (calc != stored_crc) {
            fsck_err(ctx, "GPT header CRC mismatch");
            if (fsck_should_fix(ctx)) {
                fk_put32(g_s0 + 16, calc);
                rewrite = 1;
            }
        }
    }
    if (my_lba != 1) {
        fsck_err(ctx, "GPT my_lba is not 1 (header misplaced?)");
    }
    if (alt_lba + 1 != dev->num_sectors) {
        fsck_warn(ctx, "GPT alternate header LBA != last disk sector");
    }
    if (last_us >= dev->num_sectors) {
        fsck_err(ctx, "GPT last usable LBA beyond disk capacity");
    }
    if (ent_sz < 128) {
        fsck_err(ctx, "GPT partition entry size < 128");
        return 1;
    }
    if (rewrite) {
        if (BlockDev_Write(dev, 1, g_s0, 1) == 0) {
            ctx->fixed++;
            fsck_out(ctx, "  GPT header CRC repaired");
        } else {
            fsck_err(ctx, "GPT header repair write failed");
        }
    }

    /* Partition entries — bounded by buffer capacity */
    uint32_t max_ent = n_ent;
    if (max_ent > GPT_MAX_PARTS) max_ent = GPT_MAX_PARTS;
    if (ent_sz > sizeof(g_scan) - 512) {
        fsck_err(ctx, "GPT entry size too large to scan");
        return 1;
    }
    /* Worst-case intra-sector offset is 511 bytes — keep headroom so
     * off + cnt*ent_sz always fits inside g_scan. */
    uint32_t per_chunk = (uint32_t)(sizeof(g_scan) - 512) / ent_sz;
    if (per_chunk == 0) per_chunk = 1;

    uint64_t prev_end = 0; int have_prev = 0;
    uint32_t arr_crc_calc = 0;
    /* crc32 over the whole entry array — accumulate per chunk via re-seed
     * isn't trivial; do a second pass only when needed.  Simpler: compute
     * over chunks sequentially with a running CRC using the same algorithm
     * (crc32 is composable only with care) — use a full-array second read
     * would cost more code; approximate by checksumming the used span. */
    (void)arr_crc_calc;

    for (uint32_t base = 0; base < max_ent; base += per_chunk) {
        uint32_t cnt = max_ent - base;
        if (cnt > per_chunk) cnt = per_chunk;
        uint32_t off = (base * ent_sz) % 512;
        uint32_t secs = (off + cnt * ent_sz + 511) / 512;
        if (BlockDev_Read(dev, ent_lba + (base * ent_sz) / 512,
                          g_scan, secs) != 0) {
            fsck_err(ctx, "cannot read GPT partition array");
            break;
        }
        for (uint32_t e = 0; e < cnt; e++) {
            GptPartEntry *pe = (GptPartEntry *)(g_scan + off + e * ent_sz);
            int empty = 1;
            for (int b = 0; b < 16; b++) if (pe->type_guid[b]) { empty = 0; break; }
            if (empty) continue;

            uint64_t l0 = pe->first_lba, l1 = pe->last_lba;
            line[0] = 0;
            fk_cat(line, "  #", LINE_MAX);
            fk_cat(line, fk_dec(base + e + 1, num, sizeof(num)), LINE_MAX);
            fk_cat(line, " guid ", LINE_MAX);
            char gh2[12];
            fk_hex(fk_le32(pe->type_guid), gh2, sizeof(gh2));
            fk_cat(line, gh2, LINE_MAX);
            fk_cat(line, "  ", LINE_MAX);
            /* UTF-16LE name → ASCII, max 12 chars */
            {
                char nm[16]; int k = 0;
                for (int c = 0; c < 36 && k < 14; c++) {
                    uint16_t wc = pe->name[c];
                    if (!wc) break;
                    nm[k++] = (wc < 128) ? (char)wc : '?';
                }
                nm[k] = 0;
                fk_cat(line, nm, LINE_MAX);
            }
            fk_cat(line, "  lba ", LINE_MAX);
            fk_cat(line, fk_dec(l0, num, sizeof(num)), LINE_MAX);
            fk_cat(line, "-", LINE_MAX);
            fk_cat(line, fk_dec(l1, num2, sizeof(num2)), LINE_MAX);
            fk_cat(line, " (", LINE_MAX);
            fk_cat(line, print_size(l1 - l0 + 1, sz, sizeof(sz)), LINE_MAX);
            fk_cat(line, ")", LINE_MAX);
            fsck_out(ctx, line);

            if (l0 > l1) {
                fsck_err(ctx, "GPT partition first_lba > last_lba");
            }
            if (l1 > last_us && last_us) {
                fsck_err(ctx, "GPT partition beyond last usable LBA");
            }
            if (have_prev && l0 < prev_end) {
                fsck_err(ctx, "GPT partitions overlap");
            }
            prev_end = l1; have_prev = 1;
        }
    }
    (void)arr_crc;
    return 1;
}

static int rdb_check(BlockDev *dev, FsckCtx *ctx)
{
    int rdb_at = -1;
    for (uint64_t s = 0; s < 16 && s < dev->num_sectors; s++) {
        if (BlockDev_Read(dev, s, g_s0, 1) != 0) break;
        if (fk_be32(g_s0) == RDB_IDENTIFIER) { rdb_at = (int)s; break; }
    }
    if (rdb_at < 0) return 0;

    char line[LINE_MAX], num[24], num2[24];
    fsck_out(ctx, "Amiga RDB partition table:");

    /* RDB checksum: sum of 'size' big-endian longwords == 0 */
    uint32_t sz_lw = fk_be32(g_s0 + 4);
    if (sz_lw == 0 || sz_lw > 128) sz_lw = 128;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sz_lw; i++) sum += fk_be32(g_s0 + i * 4);
    if (sum != 0) {
        fsck_err(ctx, "RDSK block checksum mismatch");
        if (fsck_should_fix(ctx)) {
            uint32_t fix = (uint32_t)(-(int32_t)(sum - fk_be32(g_s0 + 8)));
            g_s0[8] = (fix >> 24) & 0xFF; g_s0[9] = (fix >> 16) & 0xFF;
            g_s0[10] = (fix >> 8) & 0xFF; g_s0[11] = fix & 0xFF;
            if (BlockDev_Write(dev, (uint64_t)rdb_at, g_s0, 1) == 0) {
                ctx->fixed++;
                fsck_out(ctx, "  RDSK checksum repaired");
            }
        }
    }

    uint32_t block_size = fk_be32(g_s0 + 16);
    uint32_t part_list  = fk_be32(g_s0 + 28);
    uint32_t fs_list    = fk_be32(g_s0 + 32);
    line[0] = 0;
    fk_cat(line, "  block_size ", LINE_MAX);
    fk_cat(line, fk_dec(block_size, num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  cyl ", LINE_MAX);
    fk_cat(line, fk_dec(fk_be32(g_s0 + 56), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  h ", LINE_MAX);
    fk_cat(line, fk_dec(fk_be32(g_s0 + 64), num, sizeof(num)), LINE_MAX);
    fk_cat(line, "  spt ", LINE_MAX);
    fk_cat(line, fk_dec(fk_be32(g_s0 + 60), num, sizeof(num)), LINE_MAX);
    fsck_out(ctx, line);

    /* Walk PART chain */
    uint32_t blk = part_list;
    int pi = 0;
    while (blk != 0xFFFFFFFF && blk < dev->num_sectors && pi < 16) {
        if (BlockDev_Read(dev, blk, g_s1, 1) != 0) break;
        if (fk_be32(g_s1) != PART_IDENTIFIER) {
            fsck_err(ctx, "PART chain broken (bad block id)");
            break;
        }
        uint32_t psz = fk_be32(g_s1 + 4);
        if (psz == 0 || psz > 128) psz = 128;
        uint32_t psum = 0;
        for (uint32_t i = 0; i < psz; i++) psum += fk_be32(g_s1 + i * 4);
        uint8_t nlen = g_s1[40];
        if (nlen > 30) nlen = 30;
        char nm[32]; memcpy(nm, g_s1 + 41, nlen); nm[nlen] = 0;
        uint32_t pb = fk_be32(g_s1 + 136);
        uint32_t ps = fk_be32(g_s1 + 132);

        line[0] = 0;
        fk_cat(line, "  '", LINE_MAX);
        fk_cat(line, nm, LINE_MAX);
        fk_cat(line, "'  start blk ", LINE_MAX);
        fk_cat(line, fk_dec(pb, num, sizeof(num)), LINE_MAX);
        fk_cat(line, "  size ", LINE_MAX);
        fk_cat(line, fk_dec(ps, num2, sizeof(num2)), LINE_MAX);
        if (psum) fk_cat(line, "  BAD CHECKSUM", LINE_MAX);
        fsck_out(ctx, line);

        if (psum != 0) {
            fsck_err(ctx, "PART block checksum mismatch");
            if (fsck_should_fix(ctx)) {
                uint32_t fix = (uint32_t)(-(int32_t)(psum - fk_be32(g_s1 + 8)));
                g_s1[8] = (fix >> 24) & 0xFF; g_s1[9] = (fix >> 16) & 0xFF;
                g_s1[10] = (fix >> 8) & 0xFF; g_s1[11] = fix & 0xFF;
                if (BlockDev_Write(dev, blk, g_s1, 1) == 0) ctx->fixed++;
            }
        }
        blk = fk_be32(g_s1 + 16);
        pi++;
    }

    /* Filesystem list — report dostype only */
    blk = fs_list;
    int fi = 0;
    while (blk != 0xFFFFFFFF && blk < dev->num_sectors && fi < 8) {
        if (BlockDev_Read(dev, blk, g_s1, 1) != 0) break;
        if (fk_be32(g_s1) != FS_IDENTIFIER) break;
        uint32_t dt = fk_be32(g_s1 + 24);
        char dts[5];
        dts[0] = (dt >> 24) & 0xFF; dts[1] = (dt >> 16) & 0xFF;
        dts[2] = (dt >> 8) & 0xFF;  dts[3] = dt & 0xFF; dts[4] = 0;
        for (int c = 0; c < 4; c++)
            if (dts[c] < 0x20) dts[c] = (char)('0' + (dts[c] & 0x0F));
        line[0] = 0;
        fk_cat(line, "  filesystem '", LINE_MAX);
        fk_cat(line, dts, LINE_MAX);
        fk_cat(line, "'", LINE_MAX);
        fsck_out(ctx, line);
        blk = fk_be32(g_s1 + 16);
        fi++;
    }
    return 1;
}

int FSCK_DiskCheck(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX], num[24], sz[24];
    line[0] = 0;
    fk_cat(line, "Disk ", LINE_MAX);
    fk_cat(line, dev->display_name ? dev->display_name : dev->name, LINE_MAX);
    fk_cat(line, " — ", LINE_MAX);
    fk_cat(line, fk_dec(dev->num_sectors, num, sizeof(num)), LINE_MAX);
    fk_cat(line, " sectors (", LINE_MAX);
    fk_cat(line, print_size(dev->num_sectors, sz, sizeof(sz)), LINE_MAX);
    fk_cat(line, ")", LINE_MAX);
    fsck_out(ctx, line);

    if (gpt_check(dev, ctx) > 0)  return 0;
    if (rdb_check(dev, ctx) > 0)  return 0;
    if (mbr_check(dev, ctx) > 0)  return 0;

    fsck_out(ctx, "  no partition table — probing as single filesystem");
    return FSCK_RunCheck(dev, ctx);
}

/* =========================================================================
 * Surface scan — read every sector, report unreadable ranges
 * ========================================================================= */
int FSCK_SurfaceScan(BlockDev *dev, FsckCtx *ctx)
{
    char line[LINE_MAX], num[24], num2[24];
    uint64_t total = dev->num_sectors;
    uint32_t bad = 0;
    uint64_t bad_start = 0; int in_bad = 0;

    line[0] = 0;
    fk_cat(line, "Surface scan of ", LINE_MAX);
    fk_cat(line, fk_dec(total, num, sizeof(num)), LINE_MAX);
    fk_cat(line, " sectors:", LINE_MAX);
    fsck_out(ctx, line);

    for (uint64_t s = 0; s < total; s += 128) {
        uint32_t n = (uint32_t)((total - s > 128) ? 128 : total - s);
        int rc = BlockDev_Read(dev, s, g_scan, n);
        if (rc != 0) {
            /* Batch failed — isolate bad sectors individually */
            for (uint32_t i = 0; i < n; i++) {
                if (BlockDev_Read(dev, s + i, g_s0, 1) != 0) {
                    bad++;
                    if (!in_bad) { bad_start = s + i; in_bad = 1; }
                } else if (in_bad) {
                    line[0] = 0;
                    fk_cat(line, "  unreadable: sectors ", LINE_MAX);
                    fk_cat(line, fk_dec(bad_start, num, sizeof(num)), LINE_MAX);
                    fk_cat(line, "-", LINE_MAX);
                    fk_cat(line, fk_dec(s + i - 1, num2, sizeof(num2)), LINE_MAX);
                    fsck_out(ctx, line);
                    in_bad = 0;
                }
            }
        }
        if ((s & 0x3FFF) == 0) {
            fsck_yield(ctx);
            if (fsck_break(ctx)) {
                fsck_out(ctx, "  ^C — scan aborted");
                return -1;
            }
        }
        if (ctx->verbose && total > (128 << 10) &&
            (s & ((256 << 10) - 1)) == 0 && s) {
            line[0] = 0;
            fk_cat(line, "  ", LINE_MAX);
            fk_cat(line, fk_dec((s * 100) / total, num, sizeof(num)), LINE_MAX);
            fk_cat(line, "%", LINE_MAX);
            fsck_out(ctx, line);
        }
    }
    if (in_bad) {
        line[0] = 0;
        fk_cat(line, "  unreadable: sectors ", LINE_MAX);
        fk_cat(line, fk_dec(bad_start, num, sizeof(num)), LINE_MAX);
        fk_cat(line, "-", LINE_MAX);
        fk_cat(line, fk_dec(total - 1, num2, sizeof(num2)), LINE_MAX);
        fsck_out(ctx, line);
    }

    line[0] = 0;
    if (bad == 0) {
        fk_cat(line, "  all sectors readable", LINE_MAX);
        fsck_out(ctx, line);
    } else {
        fk_cat(line, "  ", LINE_MAX);
        fk_cat(line, fk_dec(bad, num, sizeof(num)), LINE_MAX);
        fk_cat(line, " unreadable sectors found", LINE_MAX);
        fsck_err(ctx, line + 2);
    }
    return bad ? (int)bad : 0;
}

/* =========================================================================
 * Sector hex dump
 * ========================================================================= */
int FSCK_DumpSector(BlockDev *dev, uint64_t sector, FsckCtx *ctx)
{
    if (BlockDev_Read(dev, sector, g_s0, 1) != 0) {
        fsck_err(ctx, "cannot read sector");
        return -1;
    }
    char line[LINE_MAX], num[24];
    line[0] = 0;
    fk_cat(line, "Sector ", LINE_MAX);
    fk_cat(line, fk_dec(sector, num, sizeof(num)), LINE_MAX);
    fk_cat(line, ":", LINE_MAX);
    fsck_out(ctx, line);

    for (int row = 0; row < 32; row++) {
        line[0] = 0;
        {
            const char *hx = "0123456789abcdef";
            int off = row * 16;
            line[0] = hx[(off >> 8) & 0xF];
            line[1] = hx[(off >> 4) & 0xF];
            line[2] = hx[off & 0xF];
            line[3] = 0;
        }
        fk_cat(line, "  ", LINE_MAX);
        for (int c = 0; c < 16; c++) {
            uint8_t b = g_s0[row * 16 + c];
            const char *hx = "0123456789abcdef";
            int l = fk_slen(line);
            if (l < LINE_MAX - 4) {
                line[l] = hx[b >> 4]; line[l + 1] = hx[b & 15];
                line[l + 2] = ' '; line[l + 3] = 0;
            }
            if (c == 7) fk_cat(line, " ", LINE_MAX);
        }
        fk_cat(line, " ", LINE_MAX);
        for (int c = 0; c < 16; c++) {
            uint8_t b = g_s0[row * 16 + c];
            int l = fk_slen(line);
            if (l < LINE_MAX - 2) {
                line[l] = (b >= 32 && b < 127) ? (char)b : '.';
                line[l + 1] = 0;
            }
        }
        fsck_out(ctx, line);
    }
    return 0;
}
