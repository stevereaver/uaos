/* crossdos_handler.c — CrossDOS FAT12/16 filesystem handler
 *
 * Implements read-only FAT12/FAT16 filesystem support for PC-format
 * floppy disks and small partitions.  This is the AmigaOS CrossDOS
 * equivalent: it allows AmigaDOS to access MS-DOS formatted media.
 *
 * The handler processes AmigaDOS packets (ACTION_FINDINPUT, ACTION_READ,
 * ACTION_END, ACTION_LOCATE_OBJECT, ACTION_EXAMINE_OBJECT, etc.) and
 * routes them through the FAT12/16 directory/file layer.
 */

#include "crossdos_handler.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "dos/handle_table.h"
#include "boot/kprint.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * FAT12/16 constants
 * ------------------------------------------------------------------------- */
#define FAT_MAX_FILES    16
#define FAT_DIR_BUF_SECS  4   /* 4 sectors = 2048 bytes dir cache */
#define FAT_NAME_LEN     13   /* 8.3 + null */

/* FAT type determination */
#define FAT_TYPE_FAT12   0
#define FAT_TYPE_FAT16   1

/* -------------------------------------------------------------------------
 * FAT12/16 filesystem state
 * ------------------------------------------------------------------------- */
typedef struct {
    BlockDev *bdev;
    uint16_t  bytes_per_sec;
    uint8_t   sec_per_clus;
    uint16_t  rsvd_sec_cnt;
    uint8_t   num_fats;
    uint16_t  root_ent_cnt;
    uint16_t  tot_sec16;
    uint32_t  tot_sec32;
    uint16_t  fat_sz16;       /* FAT size in sectors */
    uint16_t  sec_per_trk;
    uint16_t  num_heads;
    uint32_t  fat_start;      /* first FAT sector */
    uint32_t  root_start;     /* root dir start sector */
    uint32_t  root_sectors;   /* root dir sector count */
    uint32_t  data_start;     /* first data sector (cluster 2) */
    uint32_t  cluster_size;   /* bytes per cluster */
    int       fat_type;       /* FAT12 or FAT16 */
    uint32_t  vol_sectors;    /* total sectors */
} CrossDOSFS;

/* File handle */
typedef struct {
    CrossDOSFS *fs;
    uint32_t    start_cluster;  /* first cluster of file */
    uint32_t    cluster;        /* current cluster */
    uint32_t    offset_in_clus; /* offset within current cluster */
    uint32_t    pos;            /* absolute position */
    uint32_t    size;           /* file size in bytes */
    int         is_dir;
    int         in_use;
} CrossDOSFile;

/* Directory scan state */
typedef struct {
    CrossDOSFS *fs;
    uint32_t    sector;         /* current sector being scanned */
    uint32_t    offset;         /* offset within sector */
    uint32_t    entries_left;   /* entries remaining in root dir */
    int         in_root;        /* 1 = scanning root dir, 0 = cluster chain */
    uint32_t    cluster;        /* current cluster (for subdirs) */
    uint8_t     buf[512];       /* sector buffer */
    int         buf_valid;      /* buffer loaded flag */
} CrossDOSDir;

/* Handler private state */
typedef struct {
    Handler     base;
    CrossDOSFS  fs;
    CrossDOSFile files[FAT_MAX_FILES];
} CrossDOSHandler;

/* ------------------------------------------------------------------------- */
static CrossDOSFS *g_cx_fs;  /* single mount for now (static allocation) */
static CrossDOSFile g_cx_files[FAT_MAX_FILES];
static uint8_t g_sector_buf[512];

/* ------------------------------------------------------------------------- */
static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }

static int cx_slen(const char *s) { int n=0; while(s[n]) n++; return n; }

static int cx_str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ------------------------------------------------------------------------- */
static uint32_t cx_fat_entry(CrossDOSFS *fs, uint32_t cluster)
{
    uint8_t *buf = g_sector_buf;
    if (fs->fat_type == FAT_TYPE_FAT12) {
        uint32_t fat_off = cluster + (cluster / 2);
        uint32_t fat_sec = fs->fat_start + (fat_off / fs->bytes_per_sec);
        uint32_t fat_off_in_sec = fat_off % fs->bytes_per_sec;
        if (BlockDev_Read(fs->bdev, fat_sec, buf, 1) != 0) return 0xFFFFFFFF;
        uint16_t val = le16(&buf[fat_off_in_sec]);
        if (cluster & 1) val >>= 4;
        return val & 0x0FFF;
    } else {
        uint32_t fat_off = cluster * 2;
        uint32_t fat_sec = fs->fat_start + (fat_off / fs->bytes_per_sec);
        uint32_t fat_off_in_sec = fat_off % fs->bytes_per_sec;
        if (fat_off_in_sec + 1 >= fs->bytes_per_sec) {
            /* Entry spans two sectors — read both */
            if (BlockDev_Read(fs->bdev, fat_sec, buf, 1) != 0) return 0xFFFFFFFF;
            uint8_t buf2[512];
            if (BlockDev_Read(fs->bdev, fat_sec + 1, buf2, 1) != 0) return 0xFFFFFFFF;
            return le16(&buf[fs->bytes_per_sec - 1]) | ((uint16_t)buf2[0] << 8);
        }
        if (BlockDev_Read(fs->bdev, fat_sec, buf, 1) != 0) return 0xFFFFFFFF;
        return le16(&buf[fat_off_in_sec]);
    }
}

static int cx_is_eof(CrossDOSFS *fs, uint32_t cluster)
{
    if (fs->fat_type == FAT_TYPE_FAT12)
        return cluster >= 0x0FF8;
    else
        return cluster >= 0xFFF8;
}

static uint32_t cx_cluster_to_sector(CrossDOSFS *fs, uint32_t cluster)
{
    return fs->data_start + (cluster - 2) * fs->sec_per_clus;
}

/* ------------------------------------------------------------------------- */
static int cx_read_cluster(CrossDOSFS *fs, uint32_t cluster, uint8_t *buf)
{
    uint32_t sec = cx_cluster_to_sector(fs, cluster);
    for (uint32_t i = 0; i < fs->sec_per_clus; i++) {
        if (BlockDev_Read(fs->bdev, sec + i, buf + i * fs->bytes_per_sec, 1) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
static void cx_name_to_83(const char *name, char *out83)
{
    /* Convert "FILE.TXT" to FAT 8.3 format (11 chars, space-padded) */
    int i = 0;
    for (i = 0; i < 11; i++) out83[i] = ' ';
    out83[11] = '\0';

    i = 0;
    int pos = 0;
    while (name[i] && pos < 8) {
        if (name[i] == '.') break;
        out83[pos++] = name[i++];
    }
    /* Skip to dot or end */
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') i++;
    pos = 8;
    while (name[i] && pos < 11) {
        out83[pos++] = name[i++];
    }
}

static void cx_83_to_name(const char *name83, char *out)
{
    int i, j = 0;
    for (i = 0; i < 8 && name83[i] != ' '; i++)
        out[j++] = name83[i];
    if (name83[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++)
            out[j++] = name83[i];
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------------- */
static CrossDOSFile *cx_alloc_file(void)
{
    for (int i = 0; i < FAT_MAX_FILES; i++) {
        if (!g_cx_files[i].in_use) {
            g_cx_files[i].in_use = 1;
            return &g_cx_files[i];
        }
    }
    return NULL;
}

static void cx_free_file(CrossDOSFile *f)
{
    f->in_use = 0;
    f->fs = NULL;
}

/* ------------------------------------------------------------------------- */
/* Scan root directory for a file by 8.3 name.
 * Returns cluster number (file) or 0 if not found.
 * Sets *size and *is_dir if found. */
static uint32_t cx_find_in_root(CrossDOSFS *fs, const char *name83,
                                 uint32_t *size, int *is_dir)
{
    uint32_t entries = fs->root_ent_cnt;
    uint32_t sec = fs->root_start;
    uint8_t *buf = g_sector_buf;
    uint32_t ents_per_sec = fs->bytes_per_sec / 32;

    for (uint32_t e = 0; e < entries; e += ents_per_sec) {
        if (BlockDev_Read(fs->bdev, sec + e / ents_per_sec, buf, 1) != 0)
            return 0;
        uint32_t max = ents_per_sec;
        if (e + max > entries) max = entries - e;
        for (uint32_t j = 0; j < max; j++) {
            uint8_t *de = &buf[j * 32];
            if (de[0] == 0x00) return 0;  /* end of dir */
            if (de[0] == 0xE5) continue;   /* deleted */
            if ((de[11] & 0x18) == 0x08) continue; /* volume label */
            if (memcmp(de, name83, 11) == 0) {
                *is_dir = (de[11] & 0x10) ? 1 : 0;
                *size = le32(&de[28]);
                uint32_t cl = le16(&de[26]) | ((uint32_t)le16(&de[20]) << 16);
                return cl;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Packet actions
 * ------------------------------------------------------------------------- */

static void cx_action_findinput(Handler *h, DosPacket *pkt)
{
    (void)h;
    const char *path = (const char *)(uintptr_t)(uint32_t)pkt->dp_Arg1;
    if (!path) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    /* Skip volume prefix if present (e.g. "PC0:FILE.TXT") */
    const char *p = path;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    if (!*p) p = ".";

    /* Convert to 8.3 */
    char name83[12];
    cx_name_to_83(p, name83);

    uint32_t size = 0;
    int is_dir = 0;
    uint32_t cluster = cx_find_in_root(g_cx_fs, name83, &size, &is_dir);
    if (cluster == 0) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    CrossDOSFile *f = cx_alloc_file();
    if (!f) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return;
    }
    f->fs = g_cx_fs;
    f->start_cluster = cluster;
    f->cluster = cluster;
    f->offset_in_clus = 0;
    f->pos = 0;
    f->size = size;
    f->is_dir = is_dir;

    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
    /* Store handle in arg1 for later read/end */
    pkt->dp_Arg1 = (int32_t)(f - g_cx_files) + 1;
}

static void cx_action_read(Handler *h, DosPacket *pkt)
{
    (void)h;
    uint32_t handle = (uint32_t)pkt->dp_Arg1;
    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)pkt->dp_Arg2;
    uint32_t len = (uint32_t)pkt->dp_Arg3;

    if (handle == 0 || handle > FAT_MAX_FILES || !g_cx_files[handle-1].in_use) {
        pkt->dp_Res1 = 0;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    CrossDOSFile *f = &g_cx_files[handle - 1];
    if (f->pos >= f->size) {
        pkt->dp_Res1 = 0;
        pkt->dp_Res2 = 0;
        return;
    }

    uint32_t remaining = f->size - f->pos;
    if (len > remaining) len = remaining;

    uint32_t n = 0;
    uint8_t clus_buf[4096]; /* max cluster buffer */

    while (n < len) {
        if (f->cluster == 0 || cx_is_eof(f->fs, f->cluster)) break;
        if (f->offset_in_clus >= f->fs->cluster_size) {
            /* Next cluster */
            uint32_t next = cx_fat_entry(f->fs, f->cluster);
            if (cx_is_eof(f->fs, next)) break;
            f->cluster = next;
            f->offset_in_clus = 0;
        }
        if (f->offset_in_clus == 0) {
            if (cx_read_cluster(f->fs, f->cluster, clus_buf) != 0) break;
        }
        uint32_t chunk = f->fs->cluster_size - f->offset_in_clus;
        if (chunk > len - n) chunk = len - n;
        memcpy(buf + n, clus_buf + f->offset_in_clus, chunk);
        n += chunk;
        f->offset_in_clus += chunk;
        f->pos += chunk;
    }

    pkt->dp_Res1 = (int32_t)n;
    pkt->dp_Res2 = 0;
}

static void cx_action_end(Handler *h, DosPacket *pkt)
{
    (void)h;
    uint32_t handle = (uint32_t)pkt->dp_Arg1;
    if (handle > 0 && handle <= FAT_MAX_FILES)
        cx_free_file(&g_cx_files[handle - 1]);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

static void cx_action_disk_info(Handler *h, DosPacket *pkt)
{
    (void)h;
    InfoData *id = (InfoData *)(uintptr_t)(uint32_t)pkt->dp_Arg1;
    if (id && g_cx_fs) {
        id->id_NumBlocks = g_cx_fs->vol_sectors;
        id->id_NumBlocksUsed = g_cx_fs->vol_sectors; /* read-only approx */
        id->id_BytesPerBlock = g_cx_fs->bytes_per_sec;
        id->id_DiskState = ID_VALIDATED;
        id->id_NumSoftErrors = 0;
        id->id_UnitNumber = 0;
        id->id_DiskType = ID_DOS_DISK;
        id->id_VolumeNode = 0;
        id->id_InUse = 1;
    }
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

static void cx_action_examine(Handler *h, DosPacket *pkt)
{
    (void)h;
    /* Return minimal file info */
    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
}

static void cx_process_packet(Handler *h, DosPacket *pkt)
{
    if (!h || !pkt) return;
    switch (pkt->dp_Type) {
    case ACTION_FINDINPUT:
    case ACTION_FINDUPDATE:
        cx_action_findinput(h, pkt);
        break;
    case ACTION_READ:
        cx_action_read(h, pkt);
        break;
    case ACTION_END:
        cx_action_end(h, pkt);
        break;
    case ACTION_DISK_INFO:
        cx_action_disk_info(h, pkt);
        break;
    case ACTION_DIE:
        pkt->dp_Res1 = DOSTRUE;
        pkt->dp_Res2 = 0;
        break;
    case ACTION_INHIBIT:
        pkt->dp_Res1 = DOSTRUE;
        pkt->dp_Res2 = 0;
        break;
    default:
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}

/* ------------------------------------------------------------------------- */
int CrossDOS_Probe(BlockDev *bdev)
{
    if (!bdev) return 0;
    uint8_t buf[512];
    if (BlockDev_Read(bdev, 0, buf, 1) != 0) return 0;

    /* Check boot signature */
    if (buf[510] != 0x55 || buf[511] != 0xAA) return 0;

    /* Parse BPB */
    uint16_t bytes_per_sec = le16(&buf[11]);
    uint16_t root_ent_cnt  = le16(&buf[17]);
    uint16_t tot_sec16     = le16(&buf[19]);
    uint16_t fat_sz16      = le16(&buf[22]);
    uint32_t tot_sec32     = le32(&buf[32]);

    /* Must have non-zero bytes per sector and FAT size */
    if (bytes_per_sec == 0 || fat_sz16 == 0) return 0;
    if (root_ent_cnt == 0) return 0;  /* FAT32 has 0 root entries */

    uint32_t tot_sec = tot_sec16 ? tot_sec16 : tot_sec32;
    if (tot_sec == 0) return 0;

    /* Compute cluster count to determine FAT type */
    uint8_t sec_per_clus = buf[13];
    uint16_t rsvd = le16(&buf[14]);
    uint8_t num_fats = buf[16];

    uint32_t root_sectors = ((root_ent_cnt * 32) + bytes_per_sec - 1) / bytes_per_sec;
    uint32_t data_sectors = tot_sec - rsvd - (num_fats * fat_sz16) - root_sectors;
    uint32_t clusters = data_sectors / sec_per_clus;

    /* FAT12: < 4085 clusters, FAT16: < 65525 clusters */
    if (clusters < 4085) return 1;  /* FAT12 */
    if (clusters < 65525) return 1; /* FAT16 */
    return 0;  /* FAT32 — not CrossDOS */
}

/* ------------------------------------------------------------------------- */
Handler *CrossDOSHandler_Create(const char *name, BlockDev *bdev)
{
    if (!bdev) return NULL;

    uint8_t buf[512];
    if (BlockDev_Read(bdev, 0, buf, 1) != 0) {
        kprint("[CrossDOS] Failed to read boot sector\n");
        return NULL;
    }

    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        kprint("[CrossDOS] Invalid boot signature\n");
        return NULL;
    }

    CrossDOSFS *fs = g_cx_fs;
    if (!fs) {
        /* Allocate static state */
        static CrossDOSFS g_cx_fs_static;
        fs = g_cx_fs = &g_cx_fs_static;
    }

    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;
    fs->bytes_per_sec = le16(&buf[11]);
    fs->sec_per_clus  = buf[13];
    fs->rsvd_sec_cnt  = le16(&buf[14]);
    fs->num_fats      = buf[16];
    fs->root_ent_cnt  = le16(&buf[17]);
    fs->tot_sec16     = le16(&buf[19]);
    fs->fat_sz16      = le16(&buf[22]);
    fs->tot_sec32     = le32(&buf[32]);
    fs->vol_sectors   = fs->tot_sec16 ? fs->tot_sec16 : fs->tot_sec32;

    if (fs->bytes_per_sec == 0 || fs->fat_sz16 == 0 || fs->root_ent_cnt == 0) {
        kprint("[CrossDOS] Not a FAT12/16 volume\n");
        return NULL;
    }

    /* Calculate layout */
    fs->fat_start = fs->rsvd_sec_cnt;
    fs->root_start = fs->fat_start + (fs->num_fats * fs->fat_sz16);
    fs->root_sectors = ((fs->root_ent_cnt * 32) + fs->bytes_per_sec - 1) / fs->bytes_per_sec;
    fs->data_start = fs->root_start + fs->root_sectors;
    fs->cluster_size = fs->bytes_per_sec * fs->sec_per_clus;

    /* Determine FAT type */
    uint32_t data_sectors = fs->vol_sectors - fs->rsvd_sec_cnt -
                            (fs->num_fats * fs->fat_sz16) - fs->root_sectors;
    uint32_t clusters = data_sectors / fs->sec_per_clus;
    fs->fat_type = (clusters < 4085) ? FAT_TYPE_FAT12 : FAT_TYPE_FAT16;

    kprint("[CrossDOS] Mounted: ");
    kprint(fs->fat_type == FAT_TYPE_FAT12 ? "FAT12" : "FAT16");
    kprint(", bps=");
    {
        char num[12];
        extern void kprintdec(uint32_t);
        kprintdec(fs->bytes_per_sec);
    }
    kprint(", clusters=");
    {
        extern void kprintdec(uint32_t);
        kprintdec(clusters);
    }
    kprint("\n");

    Handler *h = Handler_Create(name ? name : "crossdos-handler",
                                NULL, cx_process_packet);
    if (!h) return NULL;

    return h;
}
