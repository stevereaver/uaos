/* ffs.h — UAOS Amiga OFS/FFS filesystem driver (read-only)
 *
 * Implements the Amiga "Old File System" (DOS\0) and "Fast File System"
 * (DOS\1) on-disk layout as written by AmigaOS / WinUAE, plus the
 * international/dircache variants (DOS\2..\7) where they share the same
 * physical structures.
 *
 * Block layout (512-byte blocks, big-endian):
 *   boot block   block 0:  'DOS' + dostype byte (0..7)
 *   root block   block num_blocks/2: T_HEADER + ST_ROOT, hash table,
 *                bitmap flag/pages/extension, volume name + datestamps
 *   dir blocks   T_HEADER + ST_USERDIR: 72-entry hash table of child
 *                header keys, chains linked via next_same_hash
 *   file headers T_HEADER + ST_FILE: high_seq data-block pointers anchored
 *                at the top of the hash-table region (index ht_size+5-i),
 *                extension link to T.LIST blocks for >ht_size-ish files
 *   ext blocks   T_LIST + ST_FILE: 51 further data pointers at index
 *                (56-i), extension link for the next chunk
 *   data blocks  FFS: raw payload;  OFS: 24-byte header + 488 bytes
 *
 * Write operations are intentionally unsupported: the handler reports
 * ERROR_DISK_WRITE_PROTECTED for any mutating packet.
 */

#ifndef UAOS_FFS_H
#define UAOS_FFS_H

#include <stdint.h>
#include "blockdev.h"

/* ---- on-disk constants ------------------------------------------------ */

#define FFS_BLOCK_BYTES   512
#define FFS_BLOCK_LONGS   128
#define FFS_OFS_DATA_HDR  24          /* OFS data block header bytes */
#define FFS_OFS_PAYLOAD   (FFS_BLOCK_BYTES - FFS_OFS_DATA_HDR)
#define FFS_MAX_DATA_LIST 72          /* data ptrs per header/ext block
                                         (== ht_size for 512B blocks) */
#define FFS_MAX_NAME      31          /* BSTR: len byte + up to 30 chars */

#define FFS_T_HEADER   2
#define FFS_T_DATA     8
#define FFS_T_LIST     16

/* longword indices inside a header block */
#define FFS_LW_TYPE        0
#define FFS_LW_HEADER_KEY  1
#define FFS_LW_HIGH_SEQ    2
#define FFS_LW_HT_SIZE     3
#define FFS_LW_FIRST_DATA  4
#define FFS_LW_CHECKSUM    5
#define FFS_LW_TABLE       6          /* hash table / data list region */
#define FFS_LW_BM_FLAG     78         /* root: -1 = bitmap valid */
#define FFS_LW_BM_PAGES    79         /* root: bitmap page keys [25] */
#define FFS_LW_BM_EXT      104        /* root: bitmap extension block */
#define FFS_LW_DAYS        105        /* datestamp (dir/file: entry date) */
#define FFS_LW_MINS        106
#define FFS_LW_TICKS       107
#define FFS_LW_NAME        108        /* BSTR name (root: disk name) */
#define FFS_LW_NEXT_HASH   124        /* same-hash chain link */
#define FFS_LW_PARENT      125        /* parent dir key (ext: hdr key) */
#define FFS_LW_EXTENSION   126        /* next T.LIST ext / dir cache blk */
#define FFS_LW_SEC_TYPE    127        /* secondary type */

/* secondary types (values in FFS_LW_SEC_TYPE) */
#define FFS_ST_ROOT       1
#define FFS_ST_USERDIR    2
#define FFS_ST_FILE      (-3)
#define FFS_ST_LINKFILE  (-4)
#define FFS_ST_LINKDIR    4

/* dostype low byte (boot block dword 'DOS\x') */
#define FFS_ID_OFS   0
#define FFS_ID_FFS   1
/* bits: 1 = FFS data blocks, 2 = INTL, 4 = DIRCACHE/LNFS */

/* ---- decoded entry ---------------------------------------------------- */

typedef struct {
    uint32_t key;           /* block key of this header */
    int32_t  sec_type;      /* FFS_ST_* */
    uint32_t byte_size;     /* file size in bytes (dirs: 0) */
    uint32_t protect;       /* protection bits (raw longword) */
    uint32_t parent_key;    /* parent directory key */
    uint32_t ext_key;       /* first T.LIST extension block (files) */
    uint32_t next_hash;     /* next entry in same hash slot */
    uint32_t high_seq;      /* data ptrs stored in this header (files) */
    uint32_t days, mins, ticks;
    char     name[FFS_MAX_NAME];
} FfsEntry;

/* ---- volume ------------------------------------------------------------ */

typedef struct FfsVolume {
    BlockDev *dev;
    uint8_t   dostype;       /* 0=OFS 1=FFS 2..7 variants */
    uint8_t   in_use;
    uint8_t   is_ofs;        /* data blocks carry 24-byte headers */
    uint8_t   is_intl;
    uint32_t  block_size;    /* always 512 for now */
    uint32_t  ht_size;       /* hash table entries (block_longs - 56) */
    uint32_t  root_key;
    uint32_t  num_blocks;
    uint32_t  used_blocks;   /* from bitmap; 0 when unavailable */
    int       bitmap_valid;
    char      vol_name[FFS_MAX_NAME];
} FfsVolume;

/* ---- directory iterator ------------------------------------------------ */

typedef struct {
    uint32_t ht_idx;         /* next hash-table slot to visit */
    uint32_t chain_key;      /* next_same_hash link being followed */
} FfsDirIter;

/* ---- API --------------------------------------------------------------- */

/* Probe: is sector 0 an Amiga OFS/FFS boot block? */
int FFS_Probe(BlockDev *dev);

/* Mount an OFS/FFS volume (root block located and validated).
 * Returns NULL when the device is not a valid DOS volume. */
FfsVolume *FFS_Mount(BlockDev *dev);
void       FFS_Unmount(FfsVolume *vol);

const char *FFS_VolumeName(FfsVolume *vol);
uint32_t    FFS_DosType(FfsVolume *vol);   /* 'DOS\x' big-endian code */

/* Decode a header block into FfsEntry. key must be a T_HEADER block
 * (root/dir/file). Returns 0 on success. */
int FFS_ReadEntry(FfsVolume *vol, uint32_t key, FfsEntry *out);

/* Look up `name` inside directory `dir_key`. Returns child header key
 * or 0. Name comparison is case-insensitive ASCII (INTL names that need
 * locale collation are not folded). */
uint32_t FFS_Lookup(FfsVolume *vol, uint32_t dir_key, const char *name);

/* Resolve a volume-relative path ("welldone/youhavemadeit.txt", "/x", "",
 * "." or ".."). Returns header key or 0. */
uint32_t FFS_Resolve(FfsVolume *vol, const char *path);

/* Directory enumeration. `dir_key` must be root or a ST_USERDIR block.
 * Iterator walks every hash slot and each same-hash chain. */
void FFS_DirIterInit(FfsDirIter *it);
int  FFS_DirIterNext(FfsVolume *vol, uint32_t dir_key, FfsDirIter *it,
                     FfsEntry *out);

/* Read `len` bytes of file `entry` at absolute `pos` into `dst`.
 * `dst` must be DMA-accessible (block I/O). Returns bytes read, <0 error. */
int32_t FFS_ReadAt(FfsVolume *vol, const FfsEntry *entry,
                   uint32_t pos, void *dst, uint32_t len);

/* Byte size of the file at `key` without decoding the whole entry.
 * Returns 0 on error / non-file. */
uint32_t FFS_FileSize(FfsVolume *vol, uint32_t key);

/* Bitmap accessors (for fsck; advisory — bitmaps are not authoritative).
 * Returns 1 when the bit for `block` could be read; *is_free receives the
 * bit (1 = free). Chain walked with cycle protection. */
int FFS_BlockIsFree(FfsVolume *vol, uint32_t block, int *is_free);

/* Count blocks marked used in the bitmap. Returns -1 when unavailable. */
int32_t FFS_CountUsedBlocks(FfsVolume *vol);

/* Standard OFS/FFS name hash. Public for fsck validation. */
uint32_t FFS_Hash(const char *name, uint32_t ht_size);

#endif /* UAOS_FFS_H */
