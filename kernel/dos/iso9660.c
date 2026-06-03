/* iso9660.c — UAOS ISO 9660 Level 2 Filesystem Reader */

#include "iso9660.h"
#include "ramfs.h"
#include "vfs.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

extern int g_virtio_irq_line;
extern unsigned int g_canary_before;
extern unsigned int g_canary_after;

#define ISO_SECTOR_SIZE 2048

/* ISO 9660 Primary Volume Descriptor offsets */
#define PVD_TYPE_CODE         0
#define PVD_STD_ID            1   /* "CD001" */
#define PVD_VOL_ID            40
#define PVD_SPACE_SIZE        80
#define PVD_SET_SIZE          120
#define PVD_SEQ_NUM           124
#define PVD_BLOCK_SIZE        128
#define PVD_PATH_TABLE_SIZE   132
#define PVD_L_PATH_TABLE      140
#define PVD_ROOT_DIR_RECORD   156

/* Directory record offsets */
#define DIRREC_LEN            0
#define DIRREC_EXT_ATTR_LEN   1
#define DIRREC_EXTENT_LBA     2
#define DIRREC_EXTENT_SIZE    10
#define DIRREC_RECORDING_DATE 18
#define DIRREC_FILE_FLAGS     25
#define DIRREC_FILE_UNIT_SIZE 26
#define DIRREC_INTERLEAVE     27
#define DIRREC_VOL_SEQ_NUM    28
#define DIRREC_NAME_LEN       32
#define DIRREC_NAME           33

/* File flags */
#define FLAG_HIDDEN    0x01
#define FLAG_DIRECTORY 0x02
#define FLAG_ASSOCIATED 0x04
#define FLAG_RECORD    0x08
#define FLAG_PROTECTION 0x10
#define FLAG_MULTI_EXTENT 0x80

/* SUSP signature */
#define SUSP_SP  "SP"
#define SUSP_CE  "CE"
#define SUSP_PD  "PD"
#define SUSP_ER  "ER"
#define RRIP_NM  "NM"
#define RRIP_PX  "PX"
#define RRIP_SL  "SL"
#define RRIP_CL  "CL"
#define RRIP_PL  "PL"
#define RRIP_RR  "RR"
#define RRIP_RE  "RE"

static uint8_t g_sector_buf[ISO_SECTOR_SIZE];

/* Read a 2048-byte logical sector from the block device */
static int iso_read_sector(BlockDev *bdev, uint32_t sector, uint8_t *buf) {
    uint32_t ratio = ISO_SECTOR_SIZE / bdev->sector_size;
    uint64_t dev_sector = (uint64_t)sector * ratio;
    return BlockDev_Read(bdev, dev_sector, buf, ratio);
}

/* Read multiple 2048-byte logical sectors */
static int iso_read_sectors(BlockDev *bdev, uint32_t start_sector, uint32_t count, uint8_t *buf) {
    uint32_t ratio = ISO_SECTOR_SIZE / bdev->sector_size;
    uint64_t dev_sec = (uint64_t)start_sector * ratio;
    uint32_t dev_cnt = count * ratio;
    return BlockDev_Read(bdev, dev_sec, buf, dev_cnt);
}

/* Read little-endian 16-bit */
static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Read little-endian 32-bit */
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read both-endian 32-bit (use little-endian copy) */
static uint32_t read_both32(const uint8_t *p) {
    return read_le32(p);
}

/* Parse Primary Volume Descriptor, return root dir record offset within buf */
static int parse_pvd(const uint8_t *buf, uint32_t *out_root_lba, uint32_t *out_root_size) {
    if (buf[PVD_TYPE_CODE] != 0x01) return -1;
    if (buf[PVD_STD_ID] != 'C' || buf[PVD_STD_ID+1] != 'D' ||
        buf[PVD_STD_ID+2] != '0' || buf[PVD_STD_ID+3] != '0' ||
        buf[PVD_STD_ID+4] != '1') return -1;

    const uint8_t *root = &buf[PVD_ROOT_DIR_RECORD];
    *out_root_lba = read_both32(&root[DIRREC_EXTENT_LBA]);
    *out_root_size = read_both32(&root[DIRREC_EXTENT_SIZE]);
    return 0;
}

/* Copy ISO name to dst[max], strip version suffix ";1" */
static int iso_copy_name(const uint8_t *name, int name_len, char *dst, int max) {
    int j = 0;
    for (int i = 0; i < name_len && j < max - 1; i++) {
        if (name[i] == ';') break; /* version suffix */
        dst[j++] = (char)name[i];
    }
    dst[j] = '\0';
    return j;
}

/* Get SUSP system use area start (skip initial pad if RISC OS / Amiga) */
static int susp_start(const uint8_t *dirrec, int rec_len, int name_len) {
    int su_start = DIRREC_NAME + name_len;
    if (su_start & 1) su_start++; /* pad to even */
    if (su_start >= rec_len) return -1;
    return su_start;
}

/* Parse SUSP entries, extract NM long name if present */
static int parse_susp_nm(const uint8_t *dirrec, int rec_len, int name_len, char *dst, int max) {
    int su_start = susp_start(dirrec, rec_len, name_len);
    if (su_start < 0 || su_start + 4 > rec_len) return 0;

    const uint8_t *su = &dirrec[su_start];
    int su_len = rec_len - su_start;
    int offset = 0;

    /* Check for SP (System Use Sharing Protocol) header */
    if (su[0] == 'S' && su[1] == 'P' && su[2] >= 7) {
        /* SP found; SUSP starts after SP entry */
        offset = su[2];
    }

    while (offset + 4 <= su_len) {
        if (su[offset] == 0 || su[offset] == 1) { offset++; continue; }
        int sig = (su[offset] << 8) | su[offset + 1];
        int len = su[offset + 2];
        if (len < 4 || offset + len > su_len) break;

        if (sig == ('N' << 8 | 'M')) {
            int flags = su[offset + 4];
            int nm_len = len - 5;
            int j = 0;
            for (int i = 0; i < nm_len && j < max - 1; i++)
                dst[j++] = (char)su[offset + 5 + i];
            dst[j] = '\0';
            if (!(flags & 1)) return j; /* no continuation */
        }
        /* CE = continuation area - skip for now (simplification) */
        if (sig == ('C' << 8 | 'E')) break;
        offset += len;
    }
    return 0;
}

/* =========================================================================
 * Recursive directory traversal: copy ISO files into RAMFS
 * ========================================================================= */

static void iso_traverse_dir(BlockDev *bdev, uint32_t dir_lba, uint32_t dir_size,
                               RamFsVol *vol, const char *ram_path, int proxy);

static void iso_process_entry(BlockDev *bdev, const uint8_t *rec, int rec_len,
                               RamFsVol *vol, const char *parent_ram_path,
                               int proxy)
{
    if (g_canary_before != 0xDEADBEEF) {
        kprint("[ISO9660] CANARY_BEFORE CORRUPTED!\n");
    }
    if (g_canary_after != 0xCAFEBABE) {
        kprint("[ISO9660] CANARY_AFTER CORRUPTED!\n");
    }
    if (rec_len < 34) return;
    uint8_t flags = rec[DIRREC_FILE_FLAGS];
    uint32_t extent_lba = read_both32(&rec[DIRREC_EXTENT_LBA]);
    uint32_t extent_size = read_both32(&rec[DIRREC_EXTENT_SIZE]);
    int name_len = rec[DIRREC_NAME_LEN];

    char iso_name[128];
    char long_name[256];

    iso_copy_name(&rec[DIRREC_NAME], name_len, iso_name, sizeof(iso_name));
    /* ISO 9660: '.' entry has name_len==1 and first byte==0 */
    /* '..' entry has name_len==1 and first byte==1 */
    if (name_len == 1 && (rec[DIRREC_NAME] == 0 || rec[DIRREC_NAME] == 1)) return;
    if (iso_name[0] == '\0') return; /* skip empty */

    /* Try Rock Ridge long name */
    long_name[0] = '\0';
    if (parse_susp_nm(rec, rec_len, name_len, long_name, sizeof(long_name)) > 0) {
        /* Use long name if parsed */
        int i = 0;
        while (long_name[i] && i < (int)sizeof(iso_name) - 1) { iso_name[i] = long_name[i]; i++; }
        iso_name[i] = '\0';
    }

    /* Build RAMFS path */
    char ram_path[256];
    int pl = 0;
    while (parent_ram_path[pl] && pl < 255) { ram_path[pl] = parent_ram_path[pl]; pl++; }
    if (pl > 0 && ram_path[pl - 1] != ':') { ram_path[pl++] = '/'; }
    int ni = 0;
    while (iso_name[ni] && pl < 255) { ram_path[pl++] = iso_name[ni++]; }
    ram_path[pl] = '\0';

    if (g_virtio_irq_line != 10) {
        kprint("[ISO9660] irq="); kprinthex(g_virtio_irq_line); kprint(" before mkdir/create for "); kprint(ram_path); kprint("\n");
    }

    if (flags & FLAG_DIRECTORY) {
        /* Create directory and recurse */
        RamFS_MkDir(vol, ram_path);
        iso_traverse_dir(bdev, extent_lba, extent_size, vol, ram_path, proxy);
    } else {
        /* Create file node */
        RamFsNode *node = RamFS_Create(vol, ram_path);
        if (!node) return;
        if (extent_size == 0) return;

        if (proxy) {
            /* Proxy file: data stays on block device, read on demand */
            node->ext_bdev = bdev;
            node->ext_lba = extent_lba;
            node->ext_blksz = ISO_SECTOR_SIZE;
            node->size = extent_size;
        } else {
            /* Read file data and copy into RAMFS */
            uint8_t *file_buf = RamFS_AllocPool(extent_size);
            if (!file_buf) {
                kprint("[ISO9660] Out of pool space for file "); kprint(ram_path); kprint("\n");
                return;
            }
            node->data = file_buf;
            node->alloc = extent_size;

            uint8_t file_sec[ISO_SECTOR_SIZE];
            uint32_t sectors = (extent_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
            uint32_t offset = 0;
            for (uint32_t s = 0; s < sectors && offset < extent_size; s++) {
                if (iso_read_sector(bdev, extent_lba + s, file_sec) != 0) break;
                uint32_t chunk = extent_size - offset;
                if (chunk > ISO_SECTOR_SIZE) chunk = ISO_SECTOR_SIZE;
                for (uint32_t i = 0; i < chunk; i++)
                    file_buf[offset + i] = file_sec[i];
                offset += chunk;
            }
            node->size = offset;
        }
    }
}

static void iso_traverse_dir(BlockDev *bdev, uint32_t dir_lba, uint32_t dir_size,
                               RamFsVol *vol, const char *ram_path, int proxy)
{
    uint8_t sector_buf[ISO_SECTOR_SIZE];
    uint32_t sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    for (uint32_t s = 0; s < sectors; s++) {
        if (iso_read_sector(bdev, dir_lba + s, sector_buf) != 0) return;
        int offset = 0;
        while (offset + 33 < ISO_SECTOR_SIZE) {
            uint8_t rec_len = sector_buf[offset + DIRREC_LEN];
            if (rec_len == 0) break;
            if (offset + rec_len > ISO_SECTOR_SIZE) break;
            uint8_t local_rec[256];
            for (int i = 0; i < rec_len && i < 256; i++)
                local_rec[i] = sector_buf[offset + i];
            if (g_virtio_irq_line != 10) {
                kprint("[ISO9660] irq="); kprinthex(g_virtio_irq_line); kprint(" after iso_read_sector\n");
            }
            iso_process_entry(bdev, local_rec, rec_len, vol, ram_path, proxy);
            offset += rec_len;
        }
    }
}

/* Standard AmigaDOS system directories that are assigns, not volumes.
 * These should not be auto-mounted as volumes from SYS_ROOT. */
static int is_assign_dir(const char *name)
{
    static const char *assigns[] = {
        "c", "devs", "l", "libs", "s", "fonts", "t", "env", "clips", NULL
    };
    for (int i = 0; assigns[i]; i++) {
        const char *a = assigns[i];
        const char *n = name;
        while (*a && *n) {
            char ca = *a, cn = *n;
            if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
            if (cn >= 'A' && cn <= 'Z') cn = cn - 'A' + 'a';
            if (ca != cn) break;
            a++; n++;
        }
        if (*a == '\0' && *n == '\0') return 1;
    }
    return 0;
}

/* Find a directory entry by name; returns 1 if found with *out_lba, *out_size set */
static int iso_find_dir_entry(BlockDev *bdev, uint32_t dir_lba, uint32_t dir_size,
                               const char *name, uint32_t *out_lba, uint32_t *out_size,
                               int *out_is_dir)
{
    uint8_t sector_buf[ISO_SECTOR_SIZE];
    uint32_t sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    for (uint32_t s = 0; s < sectors; s++) {
        if (iso_read_sector(bdev, dir_lba + s, sector_buf) != 0) return 0;
        int offset = 0;
        while (offset + 33 < ISO_SECTOR_SIZE) {
            uint8_t rec_len = sector_buf[offset + DIRREC_LEN];
            if (rec_len == 0) break;
            if (offset + rec_len > ISO_SECTOR_SIZE) break;
            uint8_t local_rec[256];
            for (int i = 0; i < rec_len && i < 256; i++)
                local_rec[i] = sector_buf[offset + i];
            int name_len = local_rec[DIRREC_NAME_LEN];
            char entry_name[128];
            iso_copy_name(&local_rec[DIRREC_NAME], name_len, entry_name, sizeof(entry_name));
            if (name_len == 1 && (local_rec[DIRREC_NAME] == 0 || local_rec[DIRREC_NAME] == 1)) {
                offset += rec_len;
                continue;
            }
            if (entry_name[0]) {
                int match = 1;
                int i = 0;
                while (name[i] && entry_name[i]) {
                    char a = name[i];
                    char b = entry_name[i];
                    if (a >= 'a' && a <= 'z') a = a - 'a' + 'A';
                    if (b >= 'a' && b <= 'z') b = b - 'a' + 'A';
                    if (a != b) { match = 0; break; }
                    i++;
                }
                if (match && name[i] == '\0' && entry_name[i] == '\0') {
                    *out_lba = read_both32(&local_rec[DIRREC_EXTENT_LBA]);
                    *out_size = read_both32(&local_rec[DIRREC_EXTENT_SIZE]);
                    *out_is_dir = (local_rec[DIRREC_FILE_FLAGS] & FLAG_DIRECTORY) ? 1 : 0;
                    return 1;
                }
            }
            offset += rec_len;
        }
    }
    return 0;
}

/* Mount an ISO 9660 subdirectory as a named RAMFS volume */
static int iso_mount_subvol(BlockDev *bdev, uint32_t dir_lba, uint32_t dir_size, const char *vol_name)
{
    RamFsVol *vol = RamFS_MountVol(vol_name);
    if (!vol) return -1;
    if (VFS_MountExistingVol(vol_name, vol) != 0) return -1;

    char root_path[32];
    int i = 0;
    while (vol_name[i] && i < 15) { root_path[i] = vol_name[i]; i++; }
    root_path[i] = ':'; root_path[i + 1] = '\0';

    iso_traverse_dir(bdev, dir_lba, dir_size, vol, root_path, 0);
    return 0;
}

/* =========================================================================
 * Public: ISO9660_MountCD
 * ========================================================================= */

int ISO9660_MountCD(BlockDev *bdev, const char *vol_name)
{
    extern int g_virtio_irq_line;
    #define CHECK_IRQ(label) do { int _irq = g_virtio_irq_line; if (_irq != 10) { kprint("[ISO9660] irq="); kprinthex(_irq); kprint(" at " label "\n"); } } while(0)

    if (!bdev || !vol_name || !*vol_name) return -1;

    uint8_t pvd_buf[ISO_SECTOR_SIZE];
    if (iso_read_sector(bdev, 16, pvd_buf) != 0) return -1;
    CHECK_IRQ("after_read_pvd");

    uint32_t root_lba, root_size;
    if (parse_pvd(pvd_buf, &root_lba, &root_size) != 0) return -1;
    CHECK_IRQ("after_parse_pvd");

    /* Check for sys-root directory (ISO names are uppercase) */
    uint32_t sys_lba, sys_size;
    int sys_is_dir;
    int is_workbench_mount = 0;
    /* Check if this is a Workbench mount request (CDROM/Workbench both accepted) */
    const char *p = vol_name;
    int vol_len = 0;
    while (p[vol_len]) vol_len++;
    if (vol_len == 5) {
        const char *cdrom = "CDROM";
        int match = 1;
        for (int i = 0; i < 5; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            if (c != cdrom[i]) { match = 0; break; }
        }
        if (match) is_workbench_mount = 1;
    } else if (vol_len == 9) {
        const char *workbench = "WORKBENCH";
        int match = 1;
        for (int i = 0; i < 9; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            if (c != workbench[i]) { match = 0; break; }
        }
        if (match) is_workbench_mount = 1;
    }

    if (is_workbench_mount && iso_find_dir_entry(bdev, root_lba, root_size, "SYS_ROOT", &sys_lba, &sys_size, &sys_is_dir)) {
        CHECK_IRQ("after_find_sysroot");
        if (sys_is_dir) {
            kprint("[ISO9660] Found SYS-ROOT, mounting Workbench: from SYS-ROOT contents...\n");
            /* Mount SYS-ROOT contents as Workbench: (Amiga-style live environment) */
            RamFsVol *wb_vol = RamFS_MountVol("Workbench");
            CHECK_IRQ("after_mountvol_workbench");
            if (wb_vol) {
                if (VFS_MountExistingVol("Workbench", wb_vol) == 0) {
                    /* Traverse SYS-ROOT to populate Workbench: with proxy files */
                    iso_traverse_dir(bdev, sys_lba, sys_size, wb_vol, "Workbench:", 1);
                    CHECK_IRQ("after_traverse_sysroot");
                    kprint("[ISO9660] Workbench: mounted from SYS-ROOT\n");
                } else {
                    kprint("[ISO9660] VFS_MountExistingVol Workbench failed\n");
                }
            } else {
                kprint("[ISO9660] RamFS_MountVol Workbench failed\n");
            }

            /* Also mount individual sub-volumes from SYS-ROOT */
            kprint("[ISO9660] Mounting sub-volumes...\n");
            uint8_t sys_buf[ISO_SECTOR_SIZE];
            uint32_t sectors = (sys_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
            for (uint32_t s = 0; s < sectors; s++) {
                if (iso_read_sector(bdev, sys_lba + s, sys_buf) != 0) break;
                int offset = 0;
                while (offset + 33 < ISO_SECTOR_SIZE) {
                    uint8_t rec_len = sys_buf[offset + DIRREC_LEN];
                    if (rec_len == 0) break;
                    if (offset + rec_len > ISO_SECTOR_SIZE) break;
                    uint8_t local_rec[256];
                    for (int i = 0; i < rec_len && i < 256; i++)
                        local_rec[i] = sys_buf[offset + i];
                    uint8_t flags = local_rec[DIRREC_FILE_FLAGS];
                    int name_len = local_rec[DIRREC_NAME_LEN];
                    char entry_name[128];
                    iso_copy_name(&local_rec[DIRREC_NAME], name_len, entry_name, sizeof(entry_name));
                    if (name_len == 1 && (local_rec[DIRREC_NAME] == 0 || local_rec[DIRREC_NAME] == 1)) {
                        offset += rec_len;
                        continue;
                    }
                    if (entry_name[0]) {
                        if (flags & FLAG_DIRECTORY) {
                            uint32_t sub_lba = read_both32(&local_rec[DIRREC_EXTENT_LBA]);
                            uint32_t sub_size = read_both32(&local_rec[DIRREC_EXTENT_SIZE]);
                            /* Lowercase the volume name for Amiga style */
                            char sub_vol[16];
                            int vi = 0;
                            while (entry_name[vi] && vi < 15) {
                                char c = entry_name[vi];
                                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                                sub_vol[vi] = c;
                                vi++;
                            }
                            sub_vol[vi] = '\0';
                            if (is_assign_dir(sub_vol)) {
                                kprint("[ISO9660]   Skipping assign dir "); kprint(sub_vol); kprint("/\n");
                            } else if (iso_mount_subvol(bdev, sub_lba, sub_size, sub_vol) == 0) {
                                kprint("[ISO9660]   Mounted "); kprint(sub_vol); kprint(":\n");
                                CHECK_IRQ("after_mount_subvol");
                            }
                        }
                    }
                    offset += rec_len;
                }
            }
            CHECK_IRQ("after_subvol_loop");
            return 0;
        }
    }
    CHECK_IRQ("after_sysroot_check");

    /* Fallback: mount entire CD as vol_name (or if not Workbench/CDROM) */
    RamFsVol *vol = RamFS_MountVol(vol_name);
    if (!vol) return -1;
    if (VFS_MountExistingVol(vol_name, vol) != 0) return -1;

    char root_path[32];
    int i = 0;
    while (vol_name[i] && i < 15) { root_path[i] = vol_name[i]; i++; }
    root_path[i] = ':'; root_path[i + 1] = '\0';

    iso_traverse_dir(bdev, root_lba, root_size, vol, root_path, is_workbench_mount ? 1 : 0);
    return 0;
}
