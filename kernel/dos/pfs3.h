/*
 * pfs3.h — UAOS PFS3 Filesystem Driver
 *
 * Implements PFS3 (Professional File System 3) support for block devices.
 * PFS3 is an Amiga-specific filesystem developed by Henk Kelder.
 */

#ifndef UAOS_PFS3_H
#define UAOS_PFS3_H

#include <stdint.h>
#include "blockdev.h"

/* PFS3 Root Block */
typedef struct {
    uint32_t  id;              /* 'PFS3' signature */
    uint32_t  seq_num;         /* Sequence number */
    uint32_t  date_stamp;      /* Date stamp */
    uint32_t  creation;        /* Creation date */
    uint32_t  max_seq;         /* Maximum sequence number */
    uint32_t  reserved1;       /* Reserved */
    uint32_t  options;         /* Options flags */
    uint32_t  reserved2[2];    /* Reserved */
    uint32_t  root_block;      /* Root directory block */
    uint32_t  bitmap_blocks;   /* Bitmap block count */
    uint32_t  bitmap_start;    /* First bitmap block */
    uint32_t  reserved3[3];    /* Reserved */
    uint32_t  disk_name[32];  /* Disk name (BCPL string) */
    uint32_t  reserved4[16];   /* Reserved */
} __attribute__((packed)) Pfs3RootBlock;

/* PFS3 Directory Entry */
typedef struct {
    uint32_t  next;            /* Next entry */
    uint32_t  parent;          /* Parent entry */
    uint32_t  size;            /* File/directory size */
    uint32_t  date;            /* Date stamp */
    uint32_t  protection;      /* Protection bits */
    uint32_t  type;            /* Entry type (file/dir) */
    uint32_t  first_block;     /* First data block */
    uint32_t  name[32];        /* Name (BCPL string) */
    uint32_t  comment[8];      /* Comment (BCPL string) */
    uint32_t  reserved[4];     /* Reserved */
} __attribute__((packed)) Pfs3DirEntry;

/* PFS3 Data Block */
typedef struct {
    uint32_t  next;            /* Next block in chain */
    uint32_t  seq_num;         /* Sequence number */
    uint32_t  data[508];       /* Data (508 32-bit words) */
} __attribute__((packed)) Pfs3DataBlock;

/* PFS3 filesystem structure */
typedef struct {
    BlockDev *bdev;           /* Block device */
    Pfs3RootBlock root;       /* Root block */
    uint32_t  block_size;     /* Block size in bytes */
    uint32_t  total_blocks;   /* Total blocks */
    uint32_t  root_block;     /* Root block number */
    uint32_t  bitmap_start;   /* Bitmap start block */
    uint32_t  bitmap_blocks;  /* Number of bitmap blocks */
} Pfs3FS;

/* File handle */
typedef struct {
    Pfs3FS     *fs;           /* Filesystem */
    uint32_t    current_block;/* Current data block */
    uint32_t    block_offset;  /* Offset within block */
    uint32_t    pos;          /* Current position */
    uint32_t    size;         /* File size */
    uint8_t     is_dir;       /* Is directory */
} Pfs3File;

/* Mount a PFS3 filesystem on a block device */
Pfs3FS *PFS3_Mount(BlockDev *bdev);

/* Unmount a PFS3 filesystem */
void PFS3_Unmount(Pfs3FS *fs);

/* Open a file/directory */
Pfs3File *PFS3_Open(Pfs3FS *fs, const char *path);

/* Close a file */
void PFS3_Close(Pfs3File *file);

/* Read from a file */
uint32_t PFS3_Read(Pfs3File *file, void *buffer, uint32_t len);

/* Write to a file */
uint32_t PFS3_Write(Pfs3File *file, const void *buffer, uint32_t len);

/* Seek to position */
void PFS3_Seek(Pfs3File *file, uint32_t pos);

/* Get file size */
uint32_t PFS3_Size(Pfs3File *file);

/* Read directory entry */
int PFS3_ReadDir(Pfs3File *dir, char *name, uint32_t *size, uint8_t *is_dir);

#endif /* UAOS_PFS3_H */
