/*
 * fat32.h — UAOS FAT32 Filesystem Driver
 *
 * Implements FAT32 filesystem support for block devices.
 * FAT32 is the most common filesystem for removable media and
 * is widely supported across platforms.
 */

#ifndef UAOS_FAT32_H
#define UAOS_FAT32_H

#include <stdint.h>
#include "blockdev.h"

/* FAT32 Boot Sector (BPB) */
typedef struct {
    uint8_t  jmp_boot[3];      /* Jump instruction */
    uint8_t  oem_name[8];      /* OEM name */
    uint16_t bytes_per_sec;    /* Bytes per sector */
    uint8_t  sec_per_clus;     /* Sectors per cluster */
    uint16_t rsvd_sec_cnt;     /* Reserved sector count */
    uint8_t  num_fats;         /* Number of FATs */
    uint16_t root_ent_cnt;     /* Root directory entry count (FAT12/16 only) */
    uint16_t tot_sec16;        /* Total sectors (FAT12/16 only) */
    uint8_t  media;            /* Media descriptor */
    uint16_t fat_sz16;         /* FAT size in sectors (FAT12/16 only) */
    uint16_t sec_per_trk;      /* Sectors per track */
    uint16_t num_heads;        /* Number of heads */
    uint32_t hidd_sec;         /* Hidden sectors */
    uint32_t tot_sec32;        /* Total sectors (FAT32 only) */
    uint32_t fat_sz32;         /* FAT size in sectors (FAT32 only) */
    uint16_t ext_flags;        /* Extended flags */
    uint16_t fs_ver;           /* Filesystem version */
    uint32_t root_clus;        /* Root cluster number */
    uint16_t fs_info;          /* FSINFO sector */
    uint16_t bk_boot_sec;      /* Backup boot sector */
    uint8_t  reserved[12];     /* Reserved */
    uint8_t  drv_num;         /* Drive number */
    uint8_t  reserved1;        /* Reserved */
    uint8_t  boot_sig;         /* Extended boot signature */
    uint32_t vol_id;           /* Volume serial number */
    uint8_t  vol_label[11];    /* Volume label */
    uint8_t  fs_type[8];       /* Filesystem type */
    uint8_t  boot_code[420];   /* Boot code */
    uint16_t boot_sig55aa;     /* Boot signature (0x55AA) */
} __attribute__((packed)) Fat32BPB;

/* FAT32 Directory Entry */
typedef struct {
    uint8_t  name[11];         /* Short filename (8.3) */
    uint8_t  attr;             /* Attributes */
    uint8_t  nt_res;           /* Reserved for NT */
    uint8_t  crt_time_tenth;   /* Creation time tenths */
    uint16_t crt_time;         /* Creation time */
    uint16_t crt_date;         /* Creation date */
    uint16_t lst_acc_date;     /* Last access date */
    uint16_t fst_clus_hi;      /* First cluster high */
    uint16_t wrt_time;         /* Write time */
    uint16_t wrt_date;         /* Write date */
    uint16_t fst_clus_lo;      /* First cluster low */
    uint32_t file_size;        /* File size */
} __attribute__((packed)) Fat32DirEntry;

/* Directory entry attributes */
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LONG_NAME  0x0F

/* FAT32 filesystem structure */
typedef struct {
    BlockDev *bdev;           /* Block device */
    Fat32BPB  bpb;            /* Boot sector */
    uint32_t  fat_start;      /* FAT start sector */
    uint32_t  data_start;     /* Data start sector */
    uint32_t  root_cluster;   /* Root directory cluster */
    uint32_t  bytes_per_sec;  /* Bytes per sector */
    uint32_t  sec_per_clus;   /* Sectors per cluster */
    uint32_t  cluster_size;   /* Cluster size in bytes */
    uint32_t  total_clusters; /* Total data clusters */
    int32_t   free_clusters;  /* Cached free-cluster count (-1 = unknown) */
    uint8_t  *fat_cache;      /* FAT cache (simplified) */
    uint32_t  fat_cache_sec;  /* Cached FAT sector */
} Fat32FS;

/* File handle */
typedef struct {
    Fat32FS    *fs;           /* Filesystem */
    uint32_t    start_cluster;/* First cluster of file (0 = empty file) */
    uint32_t    cluster;      /* Current cluster in chain */
    uint32_t    offset;       /* Offset within current cluster */
    uint32_t    pos;          /* Current absolute position */
    uint32_t    size;         /* File size in bytes */
    uint8_t     is_dir;       /* Is directory */
    uint8_t     in_use;       /* Allocation flag for pool management */
    /* Directory entry location (for updating file_size on close/write) */
    uint32_t    dir_sector;   /* Sector containing the dir entry for this file */
    uint32_t    dir_offset;   /* Byte offset within that sector */
    /* For directory iteration (ReadDir / ExamineNext) */
    uint32_t    iter_cluster; /* Current cluster being scanned */
    uint32_t    iter_offset;  /* Byte offset within current cluster */
} Fat32File;

/* Mount a FAT32 filesystem on a block device */
Fat32FS *FAT32_Mount(BlockDev *bdev);

/* Unmount a FAT32 filesystem */
void FAT32_Unmount(Fat32FS *fs);

/* Open a file/directory by path (e.g. "WB:dir/file" or "dir/file").
 * The volume prefix (e.g. "WB:") is stripped automatically.
 * Returns a Fat32File* (from a static pool) or NULL on error. */
Fat32File *FAT32_Open(Fat32FS *fs, const char *path);

/* Create a new file (truncate if exists). Returns Fat32File* or NULL. */
Fat32File *FAT32_CreateFile(Fat32FS *fs, const char *path);

/* Close a file (returns it to the pool; flushes size to dir entry). */
void FAT32_Close(Fat32File *file);

/* Read from a file */
uint32_t FAT32_Read(Fat32File *file, void *buffer, uint32_t len);

/* Write to a file (allocates clusters as needed, updates dir entry size) */
uint32_t FAT32_Write(Fat32File *file, const void *buffer, uint32_t len);

/* Seek to position */
void FAT32_Seek(Fat32File *file, uint32_t pos);

/* Get file size */
uint32_t FAT32_Size(Fat32File *file);

/* Read next directory entry. Call repeatedly until it returns 0.
 * Skips LFN, volume label, deleted, and . / .. entries.
 * Returns 1 on success (entry found), 0 on end-of-directory. */
int FAT32_ReadDir(Fat32File *dir, char *name, uint32_t *size, uint8_t *is_dir);

/* Create a directory at the given path. Returns 0 on success, -1 on error. */
int FAT32_CreateDir(Fat32FS *fs, const char *path);

/* Delete a file or empty directory. Returns 0 on success, -1 on error. */
int FAT32_Delete(Fat32FS *fs, const char *path);

/* Get volume statistics (total/used bytes). */
void FAT32_GetVolumeStats(Fat32FS *fs, uint32_t *total_bytes, uint32_t *used_bytes);

/* Copy the volume label (BPB vol_label, space-padded) into dst[max] with
 * trailing spaces stripped.  Returns label length, 0 if none/invalid. */
int FAT32_VolumeLabel(Fat32FS *fs, char *dst, int max);

/* Format a block device with FAT32.  vol_label: optional 11-char volume name */
int FAT32_Format(BlockDev *bdev, const char *vol_label);

#endif /* UAOS_FAT32_H */
