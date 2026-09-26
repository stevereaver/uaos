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

/* Rigid Disk Block — offsets follow Amiga devices/hardblocks.h */
typedef struct {
    uint32_t identifier;        /* 'RDSK'              lw0   */
    uint32_t size;              /* summed longs        lw1   */
    int32_t  checksum;          /*                     lw2   */
    uint32_t host_id;           /*                     lw3   */
    uint32_t block_size;        /* block bytes         lw4   */
    uint32_t flags;             /*                     lw5   */
    uint32_t bad_block_list;    /*                     lw6   */
    uint32_t partition_list;    /* first PART key      lw7   */
    uint32_t filesystem_list;   /* first FSHD key      lw8   */
    uint32_t drive_init;        /*                     lw9   */
    uint32_t boot_block_list;   /*                     lw10  */
    uint32_t reserved1[5];      /*                     lw11-15 */
    uint32_t cylinders;         /*                     lw16  */
    uint32_t sectors;           /* per track           lw17  */
    uint32_t heads;             /*                     lw18  */
    uint32_t interleave;        /*                     lw19  */
    uint32_t parking_zone;      /*                     lw20  */
    uint32_t reserved2[3];      /*                     lw21-23 */
    uint32_t write_pre_comp;    /*                     lw24  */
    uint32_t reduced_write;     /*                     lw25  */
    uint32_t step_rate;         /*                     lw26  */
    uint32_t reserved3[5];      /*                     lw27-31 */
    uint32_t rdb_blocks_lo;     /*                     lw32  */
    uint32_t rdb_blocks_hi;     /*                     lw33  */
    uint32_t low_cyl;           /*                     lw34  */
    uint32_t high_cyl;          /*                     lw35  */
    uint32_t cyl_blocks;        /* blocks per cylinder lw36  */
    uint32_t auto_park;         /*                     lw37  */
    /* More fields follow (vendor/product/revision + controllers) —
     * only the region above is needed to walk the chains. */
} __attribute__((packed)) RdbBlock;

/* Partition Block — layout follows Amiga devices/hardblocks.h:
 * the checksummed region is 64 longwords (256 bytes); the drive
 * environment vector starts at longword 32 (byte 128). */
typedef struct {
    uint32_t identifier;        /* 'PART'        lw0   */
    uint32_t size;              /* summed longs  lw1   */
    int32_t  checksum;          /*               lw2   */
    uint32_t host_id;           /*               lw3   */
    uint32_t next;              /* next PART key lw4   */
    uint32_t flags;             /*               lw5   */
    uint32_t reserved1[2];      /*               lw6-7 */
    uint32_t dev_flags;         /*               lw8   */
    uint8_t  name_len;          /* BSTR          lw9   byte 36 */
    char     name[31];          /*               bytes 37-67 */
    uint32_t reserved2[15];     /*               lw17-31 */
    uint32_t environment[32];   /* drive env     lw32-63 */
} __attribute__((packed)) RdbPartBlock;

/* Partition block flag bits */
#define PARTF_BOOTABLE  0x01
#define PARTF_NOMOUNT   0x02

/* Drive environment indices into RdbPartBlock.environment */
#define RDB_DE_TABLE_SIZE   0
#define RDB_DE_SIZE_BLOCK   1   /* block size in longwords */
#define RDB_DE_SEC_ORG      2
#define RDB_DE_SURFACES     3
#define RDB_DE_SEC_PER_BLK  4
#define RDB_DE_BLOCKS_TRACK 5
#define RDB_DE_RESERVED     6
#define RDB_DE_PREALLOC     7
#define RDB_DE_INTERLEAVE   8
#define RDB_DE_LOW_CYL      9
#define RDB_DE_HIGH_CYL     10
#define RDB_DE_NUM_BUFFERS  11
#define RDB_DE_BUF_MEM_TYPE 12
#define RDB_DE_MAX_TRANSFER 13
#define RDB_DE_MASK         14
#define RDB_DE_BOOT_PRI     15
#define RDB_DE_DOS_TYPE     16
#define RDB_DE_BAUD         17
#define RDB_DE_CONTROL      18
#define RDB_DE_BOOT_BLOCKS  19

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
    int       rdb_block;       /* sector the RDSK block was found at */
    RdbPartBlock rdb_parts[16];
    int       rdb_fshd_count;  /* FSHD blocks in the filesystem chain */
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

/* Partition extent in 512-byte sectors, derived from the PART block's
 * drive environment vector (low/high cyl * surfaces * blocks/track). */
uint64_t rdb_part_start_sector(const RdbPartBlock *pb);
uint64_t rdb_part_size_sectors(const RdbPartBlock *pb);

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
