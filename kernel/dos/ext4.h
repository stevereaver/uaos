/*
 * ext4.h — UAOS EXT4 Filesystem Driver
 *
 * Implements EXT4 filesystem support for block devices.
 * EXT4 is a modern journaling filesystem for Linux.
 * This is a basic read-only implementation.
 */

#ifndef UAOS_EXT4_H
#define UAOS_EXT4_H

#include <stdint.h>
#include "blockdev.h"

/* EXT4 Superblock */
typedef struct {
    uint32_t  inodes_count;    /* Total inodes */
    uint32_t  blocks_count;    /* Total blocks */
    uint32_t  r_blocks_count;  /* Reserved blocks */
    uint32_t  free_blocks;     /* Free blocks */
    uint32_t  free_inodes;     /* Free inodes */
    uint32_t  first_data_block;/* First data block */
    uint32_t  log_block_size;  /* Log2 of block size */
    uint32_t  log_frag_size;   /* Log2 of fragment size */
    uint32_t  blocks_per_group;/* Blocks per group */
    uint32_t  frags_per_group; /* Fragments per group */
    uint32_t  inodes_per_group;/* Inodes per group */
    uint32_t  mtime;           /* Mount time */
    uint32_t  wtime;           /* Write time */
    uint16_t  mnt_count;       /* Mount count */
    uint16_t  max_mnt_count;   /* Max mount count */
    uint16_t  magic;           /* Magic signature (0xEF53) */
    uint16_t  state;           /* Filesystem state */
    uint16_t  errors;          /* Error behavior */
    uint16_t  minor_rev_level; /* Minor revision level */
    uint32_t  lastcheck;       /* Last check time */
    uint32_t  checkinterval;   /* Check interval */
    uint32_t  creator_os;      /* Creator OS */
    uint32_t  rev_level;       /* Revision level */
    uint16_t  def_resuid;      /* Default reserved UID */
    uint16_t  def_resgid;      /* Default reserved GID */
    uint32_t  first_ino;       /* First non-reserved inode */
    uint16_t  inode_size;      /* Inode structure size */
    uint16_t  block_group_nr; /* Block group number */
    uint32_t  feature_compat;  /* Compatible features */
    uint32_t  feature_incompat;/* Incompatible features */
    uint32_t  feature_ro_compat;/* Read-only compatible features */
    uint8_t   uuid[16];        /* 128-bit UUID */
    char      volume_name[16]; /* Volume name */
    char      last_mounted[64];/* Last mount point */
    uint32_t  algorithm_usage_bitmap;
    uint8_t   prealloc_blocks;
    uint8_t   prealloc_dir_blocks;
    uint16_t  reserved_gdt_blocks;
    uint8_t   journal_uuid[16];
    uint32_t  journal_inum;
    uint32_t  journal_dev;
    uint32_t  last_orphan;
    uint32_t  hash_seed[4];
    uint8_t   def_hash_version;
    uint8_t   journal_backup_type;
    uint16_t  desc_size;
    uint8_t   default_mount_opts;
    uint8_t   first_meta_bg;
    uint32_t  mkfs_time;
    uint32_t  journal_blocks[17];
    uint32_t  min_extra_isize;
    uint32_t  want_extra_isize;
    uint32_t  flags;
    uint16_t  raid_stride;
    uint16_t  mmp_interval;
    uint64_t  mmp_block;
    uint32_t  raid_stripe_width;
    uint8_t   log_groups_per_flex;
    uint8_t   checksum_type;
    uint16_t  reserved_pad;
    uint64_t  kbytes_written;
    uint32_t  snapshot_inum;
    uint32_t  snapshot_id;
    uint64_t  snapshot_r_blocks_count;
    uint32_t  snapshot_list;
    uint32_t  error_count;
    uint32_t  first_error_time;
    uint32_t  first_error_ino;
    uint64_t  first_error_block;
    uint8_t   first_error_func[32];
    uint32_t  first_error_line;
    uint64_t  last_error_time;
    uint32_t  last_error_ino;
    uint32_t  last_error_line;
    uint64_t  last_error_block;
    uint8_t   last_error_func[32];
    uint8_t   mount_opts[64];
    uint32_t  usr_quota_inum;
    uint32_t  grp_quota_inum;
    uint32_t  overhead_count;
    uint32_t  backup_bgs[2];
    uint8_t   encrypt_algo;
    uint8_t   encrypt_pw_salt[16];
    uint32_t  lustus_lstone;
    uint32_t  checksum_seed;
    uint32_t  wtime_hi;
    uint32_t  mtime_hi;
    uint64_t  mkfs_time_hi;
    uint8_t   reserved[167];
} __attribute__((packed)) Ext4Superblock;

/* EXT4 Inode */
typedef struct {
    uint16_t  mode;            /* File mode */
    uint16_t  uid;             /* User ID */
    uint32_t  size;            /* File size */
    uint32_t  atime;           /* Access time */
    uint32_t  ctime;           /* Creation time */
    uint32_t  mtime;           /* Modification time */
    uint32_t  dtime;           /* Deletion time */
    uint16_t  gid;             /* Group ID */
    uint16_t  links_count;     /* Links count */
    uint32_t  blocks;          /* Blocks count */
    uint32_t  flags;           /* Flags */
    uint32_t  osd1;            /* OS dependent 1 */
    uint32_t  block[15];       /* Block pointers */
    uint32_t  generation;      /* File version */
    uint32_t  file_acl;        /* File ACL */
    uint32_t  dir_acl;         /* Directory ACL */
    uint32_t  faddr;           /* Fragment address */
    uint8_t   osd2[16];        /* OS dependent 2 */
    uint16_t  extra_isize;     /* Extra inode size */
    uint16_t  checksum_hi;     /* High 16 bits of checksum */
    uint32_t  ctime_extra;     /* Extra ctime */
    uint32_t  mtime_extra;     /* Extra mtime */
    uint32_t  atime_extra;     /* Extra atime */
    uint32_t  crtime;          /* Creation time */
    uint32_t  crtime_extra;    /* Extra crtime */
    uint32_t  version_hi;      /* High 32 bits of version */
    uint32_t  projid;          /* Project ID */
} __attribute__((packed)) Ext4Inode;

/* EXT4 Directory Entry */
typedef struct {
    uint32_t  inode;           /* Inode number */
    uint16_t  rec_len;         /* Record length */
    uint8_t   name_len;        /* Name length */
    uint8_t   file_type;       /* File type */
    char      name[255];       /* Name */
} __attribute__((packed)) Ext4DirEntry;

/* EXT4 filesystem structure */
typedef struct {
    BlockDev *bdev;           /* Block device */
    Ext4Superblock sb;         /* Superblock */
    uint32_t  block_size;     /* Block size */
    uint32_t  inode_size;     /* Inode size */
    uint32_t  blocks_per_group;/* Blocks per group */
    uint32_t  inodes_per_group;/* Inodes per group */
    uint32_t  inode_table_start;/* Inode table start */
} Ext4FS;

/* File handle */
typedef struct {
    Ext4FS     *fs;           /* Filesystem */
    uint32_t    inode;        /* Inode number */
    uint32_t    pos;          /* Current position */
    uint32_t    size;         /* File size */
    uint8_t     is_dir;       /* Is directory */
} Ext4File;

/* Mount an EXT4 filesystem on a block device */
Ext4FS *EXT4_Mount(BlockDev *bdev);

/* Unmount an EXT4 filesystem */
void EXT4_Unmount(Ext4FS *fs);

/* Open a file/directory */
Ext4File *EXT4_Open(Ext4FS *fs, const char *path);

/* Close a file */
void EXT4_Close(Ext4File *file);

/* Read from a file */
uint32_t EXT4_Read(Ext4File *file, void *buffer, uint32_t len);

/* Write to a file */
uint32_t EXT4_Write(Ext4File *file, const void *buffer, uint32_t len);

/* Seek to position */
void EXT4_Seek(Ext4File *file, uint32_t pos);

/* Get file size */
uint32_t EXT4_Size(Ext4File *file);

/* Read directory entry */
int EXT4_ReadDir(Ext4File *dir, char *name, uint32_t *size, uint8_t *is_dir);

#endif /* UAOS_EXT4_H */
