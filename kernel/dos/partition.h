/*
 * partition.h — UAOS Partition Table Editor
 *
 * Supports MBR, GPT, and Amiga RDB partition schemes.
 */

#ifndef UAOS_PARTITION_H
#define UAOS_PARTITION_H

#include <stdint.h>
#include "blockdev.h"

/* =========================================================================
 * Partition Scheme Types
 * ========================================================================= */

#define PART_SCHEME_MBR  0
#define PART_SCHEME_GPT  1
#define PART_SCHEME_RDB  2

/* =========================================================================
 * MBR Partition Table
 * ========================================================================= */

#define MBR_PART_COUNT  4
#define MBR_SECTOR_SIZE 512
#define MBR_BOOT_SIG    0xAA55

/* MBR partition entry (16 bytes) */
typedef struct {
    uint8_t  boot_flag;      /* 0x80 = active, 0x00 = inactive */
    uint8_t  chs_start[3];   /* CHS start address */
    uint8_t  type_code;      /* Partition type */
    uint8_t  chs_end[3];     /* CHS end address */
    uint32_t lba_start;      /* LBA of first sector */
    uint32_t sector_count;   /* Number of sectors */
} __attribute__((packed)) MbrPartEntry;

/* MBR sector (512 bytes) */
typedef struct {
    uint8_t       boot_code[446];   /* Boot loader code */
    MbrPartEntry  partitions[4];    /* 4 partition entries */
    uint16_t      boot_sig;         /* 0xAA55 */
} __attribute__((packed)) MbrSector;

/* Common partition type codes */
#define PART_TYPE_EMPTY     0x00
#define PART_TYPE_FAT12     0x01
#define PART_TYPE_FAT16     0x06
#define PART_TYPE_NTFS      0x07
#define PART_TYPE_FAT32     0x0B
#define PART_TYPE_FAT32_LBA 0x0C
#define PART_TYPE_FAT16_LBA 0x0E
#define PART_TYPE_LINUX     0x83
#define PART_TYPE_LINUX_SWAP 0x82
#define PART_TYPE_LINUX_LVM 0x8E
#define PART_TYPE_EFI       0xEF
#define PART_TYPE_AMIGA     0x76
#define PART_TYPE_GPT_PROT  0xEE

/* =========================================================================
 * GPT Partition Table
 * ========================================================================= */

#define GPT_PART_ENTRY_SIZE 128
#define GPT_PART_NAME_LEN   72
#define GPT_MAX_PARTS       128

/* GPT header */
typedef struct {
    uint64_t signature;      /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) GptHeader;

/* GPT partition entry */
typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];       /* UTF-16LE */
} __attribute__((packed)) GptPartEntry;

/* =========================================================================
 * Amiga RDB Partition Table
 * ========================================================================= */

#define RDB_BLOCK_SIZE      512
#define RDB_IDENTIFIER      0x5244534B  /* 'RDSK' */
#define PART_IDENTIFIER     0x50415254  /* 'PART' */
#define FS_IDENTIFIER       0x46534844  /* 'FSHD' */

/* Rigid Disk Block */
typedef struct {
    uint32_t identifier;        /* 'RDSK' */
    uint32_t size;              /* Size of this block in longwords */
    uint32_t checksum;
    uint32_t host_id;           /* SCSI host ID */
    uint32_t block_size;        /* Block size in bytes */
    uint32_t flags;
    uint32_t bad_block_list;
    uint32_t partition_list;
    uint32_t filesystem_list;
    uint32_t reserved[5];
    /* Geometry */
    uint32_t cylinders;
    uint32_t sectors;
    uint32_t heads;
    uint32_t interleave;
    uint32_t parking_zone;
    uint32_t reserved2[3];
    uint32_t cylinder_blocks;   /* blocks per cylinder */
    uint32_t high_cyl;          /* highest cylinder */
    uint32_t low_cyl;           /* lowest cylinder */
    /* More fields... */
} __attribute__((packed)) RdbBlock;

/* Partition Block */
typedef struct {
    uint32_t identifier;        /* 'PART' */
    uint32_t size;
    uint32_t checksum;
    uint32_t host_id;
    uint32_t next;             /* Next partition block */
    uint32_t flags;
    uint32_t reserved[3];
    uint32_t dev_flags;
    uint8_t  name_len;
    char     name[31];         /* Partition name */
    uint32_t reserved2[15];
    uint32_t size_blocks;      /* Size in blocks */
    uint32_t start_block;      /* Start block */
    /* More fields for filesystem specific data */
} __attribute__((packed)) RdbPartBlock;

/* =========================================================================
 * UAOS Partition Metadata (stored in sector 1 of MBR disks)
 * ========================================================================= */

#define UAOS_PART_META_MAGIC  0x55414F53  /* 'UAOS' */
#define UAOS_PART_META_VER    1
#define UAOS_PART_MAX_NAME    12

typedef struct {
    char     name[UAOS_PART_MAX_NAME];  /* Display name e.g. "DH0:" */
    uint8_t  automount;                 /* Auto-mount at boot */
    uint8_t  bootable;                  /* Bootable flag */
    uint8_t  boot_pri;                  /* Boot priority (higher = earlier) */
    uint8_t  reserved;
} UaosPartMetaEntry;

typedef struct {
    uint32_t magic;                     /* UAOS_PART_META_MAGIC */
    uint32_t version;                   /* UAOS_PART_META_VER */
    uint32_t checksum;                  /* Simple sum of data area */
    uint32_t reserved;
    UaosPartMetaEntry parts[MBR_PART_COUNT];
} UaosPartMeta;

/* =========================================================================
 * Partition Editor State
 * ========================================================================= */

#define MAX_FDISK_PARTS     128

typedef struct {
    int       valid;           /* 1 if partition table was read */
    int       scheme;          /* PART_SCHEME_MBR/GPT/RDB */
    int       num_partitions;  /* Number of active partitions */
    uint64_t  disk_sectors;    /* Total disk size in sectors */
    uint32_t  disk_id;         /* Disk identifier (MBR) */
    /* MBR specific */
    MbrSector mbr;
    int       mbr_modified;
    /* GPT specific */
    GptHeader gpt;
    GptPartEntry gpt_parts[GPT_MAX_PARTS];
    int       gpt_modified;
    /* RDB specific */
    RdbBlock  rdb;
    RdbPartBlock rdb_parts[16];
    int       rdb_modified;
    /* UAOS metadata */
    UaosPartMeta uaos_meta;
    int       meta_modified;
} PartitionTable;

/* =========================================================================
 * Partition Type Names
 * ========================================================================= */

const char *partition_type_name(uint8_t type);

/* =========================================================================
 * MBR Operations
 * ========================================================================= */

int mbr_read(BlockDev *dev, PartitionTable *pt);
int mbr_write(BlockDev *dev, PartitionTable *pt);
int mbr_create_new(PartitionTable *pt);
int mbr_add_partition(PartitionTable *pt, uint32_t start, uint32_t count, uint8_t type);
int mbr_delete_partition(PartitionTable *pt, int index);
void mbr_print_partitions(PartitionTable *pt, void (*print_fn)(const char *));

/* =========================================================================
 * GPT Operations
 * ========================================================================= */

int gpt_read(BlockDev *dev, PartitionTable *pt);
int gpt_write(BlockDev *dev, PartitionTable *pt);
void gpt_print_partitions(PartitionTable *pt, void (*print_fn)(const char *));

/* =========================================================================
 * RDB Operations
 * ========================================================================= */

int rdb_read(BlockDev *dev, PartitionTable *pt);
int rdb_write(BlockDev *dev, PartitionTable *pt);
void rdb_print_partitions(PartitionTable *pt, void (*print_fn)(const char *));

/* =========================================================================
 * Generic Operations
 * ========================================================================= */

int partition_read(BlockDev *dev, PartitionTable *pt);
int partition_write(BlockDev *dev, PartitionTable *pt);
void partition_print(PartitionTable *pt, void (*print_fn)(const char *));

/* UAOS partition metadata (sector 1) */
int uaos_meta_read(BlockDev *dev, UaosPartMeta *meta);
int uaos_meta_write(BlockDev *dev, UaosPartMeta *meta);
void uaos_meta_init(UaosPartMeta *meta);

/* Get partition display name from metadata (falls back to default) */
const char *uaos_meta_get_name(UaosPartMeta *meta, int part_index, char *buf, int buf_len);

#endif /* UAOS_PARTITION_H */
