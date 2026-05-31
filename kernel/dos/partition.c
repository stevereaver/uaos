/*
 * partition.c — UAOS Partition Table Editor Implementation
 *
 * Supports MBR, GPT, and Amiga RDB partition schemes.
 */

#include "partition.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations for internal helpers */
static void scpy(char *dst, const char *src, int max);
static void scat(char *dst, const char *src, int max);

/* =========================================================================
 * Helper Functions
 * ========================================================================= */

static void uint_to_str(uint32_t v, char *buf, int max)
{
    int i = 0;
    char tmp[16];
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0 && i < 15) { tmp[i++] = '0' + (v % 10); v /= 10; }
    if (i >= max) i = max - 1;
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void uint64_to_str(uint64_t v, char *buf, int max)
{
    int i = 0;
    char tmp[32];
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0 && i < 31) { tmp[i++] = '0' + (v % 10); v /= 10; }
    if (i >= max) i = max - 1;
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void uint_to_hex_str(uint32_t v, char *buf, int digits)
{
    const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[digits] = '\0';
}

static void scat(char *dst, const char *src, int max)
{
    int dl = 0;
    while (dst[dl] && dl < max) dl++;
    int i = 0;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = '\0';
}

static void scpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* =========================================================================
 * Partition Type Names
 * ========================================================================= */

const char *partition_type_name(uint8_t type)
{
    switch (type) {
        case PART_TYPE_EMPTY:     return "Empty";
        case PART_TYPE_FAT12:     return "FAT12";
        case PART_TYPE_FAT16:     return "FAT16";
        case PART_TYPE_NTFS:      return "NTFS";
        case PART_TYPE_FAT32:     return "FAT32";
        case PART_TYPE_FAT32_LBA: return "FAT32 LBA";
        case PART_TYPE_FAT16_LBA: return "FAT16 LBA";
        case PART_TYPE_LINUX:     return "Linux";
        case PART_TYPE_LINUX_SWAP: return "Linux swap";
        case PART_TYPE_LINUX_LVM: return "Linux LVM";
        case PART_TYPE_EFI:       return "EFI System";
        case PART_TYPE_AMIGA:     return "Amiga";
        case PART_TYPE_GPT_PROT:  return "GPT protective";
        default:                  return "Unknown";
    }
}

/* =========================================================================
 * MBR Operations
 * ========================================================================= */

int mbr_read(BlockDev *dev, PartitionTable *pt)
{
    if (!dev || !pt) return -1;

    uint8_t sector[512];
    memset(sector, 0, 512);

    int ret = BlockDev_Read(dev, 0, sector, 1);
    if (ret != 0) {
        return -1;
    }

    /* Check boot signature */
    uint16_t sig = sector[510] | (sector[511] << 8);
    if (sig != MBR_BOOT_SIG) {
        /* No valid MBR - treat as empty */
        memset(&pt->mbr, 0, sizeof(MbrSector));
        pt->mbr.boot_sig = MBR_BOOT_SIG;
        pt->valid = 1;
        pt->scheme = PART_SCHEME_MBR;
        pt->num_partitions = 0;
        pt->disk_sectors = dev->num_sectors;
        pt->disk_id = 0;
        pt->mbr_modified = 1;
        return 0;
    }

    /* Copy MBR data */
    memcpy(&pt->mbr, sector, sizeof(MbrSector));
    pt->valid = 1;
    pt->scheme = PART_SCHEME_MBR;
    pt->disk_sectors = dev->num_sectors;
    pt->mbr_modified = 0;

    /* Count active partitions */
    pt->num_partitions = 0;
    for (int i = 0; i < MBR_PART_COUNT; i++) {
        if (pt->mbr.partitions[i].type_code != PART_TYPE_EMPTY) {
            pt->num_partitions++;
        }
    }

    /* Extract disk ID from boot code area (bytes 440-443) */
    pt->disk_id = *(uint32_t*)&sector[440];

    return 0;
}

int mbr_write(BlockDev *dev, PartitionTable *pt)
{
    if (!dev || !pt || !pt->valid) return -10;

    uint8_t sector[512];
    memset(sector, 0, 512);

    /* Copy boot code if present, otherwise leave zeros */
    memcpy(sector, pt->mbr.boot_code, 446);

    /* Copy partition table */
    memcpy(sector + 446, pt->mbr.partitions, sizeof(MbrPartEntry) * 4);

    /* Boot signature */
    sector[510] = 0x55;
    sector[511] = 0xAA;

    int ret = BlockDev_Write(dev, 0, sector, 1);
    if (ret != 0) {
        return -20;
    }

    pt->mbr_modified = 0;
    return 0;
}

int mbr_create_new(PartitionTable *pt)
{
    if (!pt) return -1;

    memset(&pt->mbr, 0, sizeof(MbrSector));
    pt->mbr.boot_sig = MBR_BOOT_SIG;
    pt->valid = 1;
    pt->scheme = PART_SCHEME_MBR;
    pt->num_partitions = 0;
    pt->mbr_modified = 1;

    /* Generate a simple disk ID */
    pt->disk_id = 0x12345678;
    memcpy(pt->mbr.boot_code + 440, &pt->disk_id, 4);

    return 0;
}

int mbr_add_partition(PartitionTable *pt, uint32_t start, uint32_t count, uint8_t type)
{
    if (!pt || !pt->valid) return -1;

    /* Find free slot */
    int idx = -1;
    for (int i = 0; i < MBR_PART_COUNT; i++) {
        if (pt->mbr.partitions[i].type_code == PART_TYPE_EMPTY) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        return -1;  /* No free slots */
    }

    /* Validate bounds */
    if (start == 0) start = 2048;  /* Leave space for MBR and alignment */
    if (start + count > pt->disk_sectors) {
        count = pt->disk_sectors - start;
    }

    MbrPartEntry *p = &pt->mbr.partitions[idx];
    p->boot_flag = 0x00;
    p->type_code = type;
    p->lba_start = start;
    p->sector_count = count;

    /* Simple CHS calculation (not used but set for compatibility) */
    p->chs_start[0] = 0xFE;
    p->chs_start[1] = 0xFF;
    p->chs_start[2] = 0xFF;
    p->chs_end[0] = 0xFE;
    p->chs_end[1] = 0xFF;
    p->chs_end[2] = 0xFF;

    pt->num_partitions++;
    pt->mbr_modified = 1;

    return idx;
}

int mbr_delete_partition(PartitionTable *pt, int index)
{
    if (!pt || !pt->valid) return -1;
    if (index < 0 || index >= MBR_PART_COUNT) return -1;

    if (pt->mbr.partitions[index].type_code == PART_TYPE_EMPTY) {
        return -1;  /* Already empty */
    }

    memset(&pt->mbr.partitions[index], 0, sizeof(MbrPartEntry));
    pt->num_partitions--;
    pt->mbr_modified = 1;

    return 0;
}

void mbr_print_partitions(PartitionTable *pt, void (*print_fn)(const char *))
{
    if (!pt || !pt->valid || !print_fn) return;

    char msg[256];
    char namebuf[16];

    print_fn("Disk identifier: 0x");
    char hex[16];
    uint_to_hex_str(pt->disk_id, hex, 8);
    print_fn(hex);
    print_fn("");

    print_fn("Device   Name       Boot   Start      End  Sectors   Size Id Type");

    for (int i = 0; i < MBR_PART_COUNT; i++) {
        MbrPartEntry *p = &pt->mbr.partitions[i];

        /* Device name */
        scpy(msg, "virtio0", 256);
        msg[7] = '1' + i;
        msg[8] = '\0';

        /* Pad to 9 chars (device column width) */
        int len = 0;
        while (msg[len]) len++;
        while (len < 9) { msg[len] = ' '; len++; }
        msg[len] = '\0';

        /* UAOS display name */
        const char *pn = uaos_meta_get_name(&pt->uaos_meta, i, namebuf, sizeof(namebuf));
        scat(msg, pn, 256);
        len = 0;
        while (msg[len]) len++;
        while (len < 20) { msg[len] = ' '; len++; }
        msg[len] = '\0';

        /* Boot flag */
        if (p->boot_flag == 0x80) {
            scat(msg, "* ", 256);
        } else if (pt->uaos_meta.parts[i].bootable) {
            scat(msg, "B ", 256);
        } else {
            scat(msg, "  ", 256);
        }

        if (p->type_code == PART_TYPE_EMPTY) {
            scat(msg, "      -        -        -        -     -  - Empty", 256);
        } else {
            /* Start */
            char num[32];
            uint_to_str(p->lba_start, num, 32);
            int nl = 0;
            while (num[nl]) nl++;
            while (nl < 8) { scat(msg, " ", 256); nl++; }
            scat(msg, num, 256);
            scat(msg, " ", 256);

            /* End */
            uint64_t end = (uint64_t)p->lba_start + p->sector_count - 1;
            uint64_to_str(end, num, 32);
            nl = 0;
            while (num[nl]) nl++;
            while (nl < 8) { scat(msg, " ", 256); nl++; }
            scat(msg, num, 256);
            scat(msg, " ", 256);

            /* Sectors */
            uint_to_str(p->sector_count, num, 32);
            nl = 0;
            while (num[nl]) nl++;
            while (nl < 8) { scat(msg, " ", 256); nl++; }
            scat(msg, num, 256);
            scat(msg, " ", 256);

            /* Size in MB */
            uint64_t mb = ((uint64_t)p->sector_count * 512) / (1024 * 1024);
            uint64_to_str(mb, num, 32);
            nl = 0;
            while (num[nl]) nl++;
            while (nl < 6) { scat(msg, " ", 256); nl++; }
            scat(msg, num, 256);
            scat(msg, "M ", 256);

            /* Type code */
            uint_to_hex_str(p->type_code, num, 2);
            scat(msg, num, 256);
            scat(msg, " ", 256);

            /* Type name */
            scat(msg, partition_type_name(p->type_code), 256);

            /* Flags summary */
            if (pt->uaos_meta.parts[i].automount) {
                scat(msg, " [auto]", 256);
            }
            if (pt->uaos_meta.parts[i].boot_pri > 0) {
                scat(msg, " pri=", 256);
                char prbuf[4];
                uint_to_str(pt->uaos_meta.parts[i].boot_pri, prbuf, 4);
                scat(msg, prbuf, 256);
            }
        }

        print_fn(msg);
    }
}

/* =========================================================================
 * GPT Operations (stubs for now)
 * ========================================================================= */

int gpt_read(BlockDev *dev, PartitionTable *pt)
{
    /* TODO: Implement GPT reading */
    (void)dev;
    (void)pt;
    return -1;
}

int gpt_write(BlockDev *dev, PartitionTable *pt)
{
    /* TODO: Implement GPT writing */
    (void)dev;
    (void)pt;
    return -1;
}

void gpt_print_partitions(PartitionTable *pt, void (*print_fn)(const char *))
{
    /* TODO: Implement GPT printing */
    (void)pt;
    (void)print_fn;
}

/* =========================================================================
 * RDB Operations (stubs for now)
 * ========================================================================= */

int rdb_read(BlockDev *dev, PartitionTable *pt)
{
    /* TODO: Implement RDB reading */
    (void)dev;
    (void)pt;
    return -1;
}

int rdb_write(BlockDev *dev, PartitionTable *pt)
{
    /* TODO: Implement RDB writing */
    (void)dev;
    (void)pt;
    return -1;
}

void rdb_print_partitions(PartitionTable *pt, void (*print_fn)(const char *))
{
    /* TODO: Implement RDB printing */
    (void)pt;
    (void)print_fn;
}

/* =========================================================================
 * Generic Operations
 * ========================================================================= */

int partition_read(BlockDev *dev, PartitionTable *pt)
{
    if (!dev || !pt) return -1;

    memset(pt, 0, sizeof(PartitionTable));
    pt->disk_sectors = dev->num_sectors;

    /* Try MBR first */
    if (mbr_read(dev, pt) == 0) {
        /* Read UAOS metadata from sector 1 */
        uaos_meta_read(dev, &pt->uaos_meta);
        return 0;
    }

    /* TODO: Try GPT, then RDB */

    return -1;
}

int partition_write(BlockDev *dev, PartitionTable *pt)
{
    if (!dev) return -1;
    if (!pt) return -2;
    if (!pt->valid) return -3;

    int ret = 0;
    switch (pt->scheme) {
        case PART_SCHEME_MBR:
            ret = mbr_write(dev, pt);
            break;
        case PART_SCHEME_GPT:
            ret = gpt_write(dev, pt);
            break;
        case PART_SCHEME_RDB:
            ret = rdb_write(dev, pt);
            break;
        default:
            return -4;
    }
    if (ret == 0 && pt->meta_modified) {
        uaos_meta_write(dev, &pt->uaos_meta);
        pt->meta_modified = 0;
    }
    return ret;
}

void partition_print(PartitionTable *pt, void (*print_fn)(const char *))
{
    if (!pt || !pt->valid || !print_fn) return;

    switch (pt->scheme) {
        case PART_SCHEME_MBR:
            mbr_print_partitions(pt, print_fn);
            break;
        case PART_SCHEME_GPT:
            gpt_print_partitions(pt, print_fn);
            break;
        case PART_SCHEME_RDB:
            rdb_print_partitions(pt, print_fn);
            break;
        default:
            print_fn("Unknown partition scheme");
            break;
    }
}

/* =========================================================================
 * UAOS Partition Metadata (sector 1)
 * ========================================================================= */

void uaos_meta_init(UaosPartMeta *meta)
{
    if (!meta) return;
    memset(meta, 0, sizeof(UaosPartMeta));
    meta->magic = UAOS_PART_META_MAGIC;
    meta->version = UAOS_PART_META_VER;
}

static uint32_t uaos_meta_checksum(const UaosPartMeta *meta)
{
    uint32_t sum = 0;
    const uint8_t *p = (const uint8_t *)meta->parts;
    for (size_t i = 0; i < sizeof(meta->parts); i++)
        sum += p[i];
    return sum;
}

int uaos_meta_read(BlockDev *dev, UaosPartMeta *meta)
{
    if (!dev || !meta) return -1;

    uint8_t sector[512];
    memset(sector, 0, 512);

    if (BlockDev_Read(dev, 1, sector, 1) != 0) {
        return -1;
    }

    memcpy(meta, sector, sizeof(UaosPartMeta));

    if (meta->magic != UAOS_PART_META_MAGIC) {
        /* No valid metadata — initialise defaults */
        uaos_meta_init(meta);
        return 0;  /* Not an error, just no metadata yet */
    }

    uint32_t computed = uaos_meta_checksum(meta);
    if (computed != meta->checksum) {
        /* Checksum mismatch — reset to defaults */
        uaos_meta_init(meta);
        return 0;
    }

    return 0;
}

int uaos_meta_write(BlockDev *dev, UaosPartMeta *meta)
{
    if (!dev || !meta) return -1;

    meta->magic = UAOS_PART_META_MAGIC;
    meta->version = UAOS_PART_META_VER;
    meta->checksum = uaos_meta_checksum(meta);

    uint8_t sector[512];
    memset(sector, 0, 512);
    memcpy(sector, meta, sizeof(UaosPartMeta));

    return BlockDev_Write(dev, 1, sector, 1);
}

const char *uaos_meta_get_name(UaosPartMeta *meta, int part_index, char *buf, int buf_len)
{
    if (!meta || !buf || buf_len < 1 || part_index < 0 || part_index >= MBR_PART_COUNT)
        return NULL;

    if (meta->parts[part_index].name[0]) {
        int i = 0;
        for (i = 0; i < buf_len - 1 && i < UAOS_PART_MAX_NAME; i++)
            buf[i] = meta->parts[part_index].name[i];
        buf[i] = '\0';
        return buf;
    }
    /* Default name */
    buf[0] = 'D'; buf[1] = 'H'; buf[2] = '0' + part_index;
    buf[3] = ':'; buf[4] = '\0';
    return buf;
}

