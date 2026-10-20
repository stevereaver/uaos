/* amiga_dos_types.h — AmigaOS-compatible DOS structure definitions
 *
 * FileLock, FileInfoBlock, DosList, and protection-bit definitions.
 * Used by both the native packet handler layer and M68k emulation glue.
 */

#ifndef UAOS_AMIGA_DOS_TYPES_H
#define UAOS_AMIGA_DOS_TYPES_H

#include <stdint.h>
#include "dospacket.h"

/* -------------------------------------------------------------------------
 * BPTR / BSTR
 * ------------------------------------------------------------------------- */
typedef uint32_t BPTR;
typedef uint32_t BSTR;

/* -------------------------------------------------------------------------
 * FileLock — pointed to by a lock BPTR
 *
 * Layout matches 32-bit AmigaDOS: 20 bytes, all 4-byte fields.
 * fl_Key stores the host HandleTable slot ID (opaque to guest).
 * fl_Task stores a handler marker; guest never dereferences it directly
 * because all dos.library calls are intercepted by the glue layer.
 * ------------------------------------------------------------------------- */
typedef struct FileLock {
    BPTR    fl_Link;     /* 0:  BCPL pointer to next lock (0 for now) */
    BPTR    fl_Key;      /* 4:  disk key / handle ID */
    int32_t fl_Access;   /* 8:  SHARED_LOCK (-2) or EXCLUSIVE_LOCK (-1) */
    BPTR    fl_Task;     /* 12: handler MsgPort (not a real guest pointer) */
    BPTR    fl_Volume;   /* 16: DosList BPTR (0 for now) */
} FileLock;

/* -------------------------------------------------------------------------
 * FileInfoBlock — filled by Examine / ExamineNext / ExamineFh
 * ------------------------------------------------------------------------- */
typedef struct FileInfoBlock {
    int32_t  fib_DiskKey;
    int32_t  fib_DirEntryType;   /* ST_FILE, ST_USERDIR, ST_ROOTDIR, etc. */
    char     fib_FileName[108];
    int32_t  fib_Protection;
    int32_t  fib_EntryType;
    int32_t  fib_Size;
    int32_t  fib_NumBlocks;
    struct {
        int32_t ds_Days;
        int32_t ds_Minute;
        int32_t ds_Tick;
    } fib_Date;
    char     fib_Comment[80];
    int16_t  fib_OwnerUID;
    int16_t  fib_OwnerGID;
    char     fib_Reserved[32];
} FileInfoBlock;

/* -------------------------------------------------------------------------
 * Entry type constants
 * ------------------------------------------------------------------------- */
#define ST_ROOTDIR    1    /* canonical name in dos.h is ST_ROOT */
#define ST_ROOT       1
#define ST_USERDIR    2
#define ST_SOFTLINK   3    /* looks like dir, may point to a file */
#define ST_LINKDIR    4    /* hard link to dir */
#define ST_FILE      (-3)
#define ST_LINKFILE  (-4)
#define ST_PIPEFILE  (-5)
#define ST_SOCKET    (-6)  /* UAOS extension (not in NDK) */

/* -------------------------------------------------------------------------
 * Protection bits (dos/dos.h, NDK 40.13)
 *
 * Owner RWED bits are INVERTED (bit set = denied).
 * GRP/OTR RWED bits are NOT inverted (bit set = allowed).
 * FIBB_* are bit numbers, FIBF_* are masks.
 * ------------------------------------------------------------------------- */
#define FIBB_OTR_READ    15
#define FIBB_OTR_WRITE   14
#define FIBB_OTR_EXECUTE 13
#define FIBB_OTR_DELETE  12
#define FIBB_GRP_READ    11
#define FIBB_GRP_WRITE   10
#define FIBB_GRP_EXECUTE  9
#define FIBB_GRP_DELETE   8
#define FIBB_SCRIPT       6
#define FIBB_PURE         5
#define FIBB_ARCHIVE      4
#define FIBB_READ         3
#define FIBB_WRITE        2
#define FIBB_EXECUTE      1
#define FIBB_DELETE       0

#define FIBF_OTR_READ    (1 << FIBB_OTR_READ)
#define FIBF_OTR_WRITE   (1 << FIBB_OTR_WRITE)
#define FIBF_OTR_EXECUTE (1 << FIBB_OTR_EXECUTE)
#define FIBF_OTR_DELETE  (1 << FIBB_OTR_DELETE)
#define FIBF_GRP_READ    (1 << FIBB_GRP_READ)
#define FIBF_GRP_WRITE   (1 << FIBB_GRP_WRITE)
#define FIBF_GRP_EXECUTE (1 << FIBB_GRP_EXECUTE)
#define FIBF_GRP_DELETE  (1 << FIBB_GRP_DELETE)
#define FIBF_SCRIPT      (1 << FIBB_SCRIPT)
#define FIBF_PURE        (1 << FIBB_PURE)
#define FIBF_ARCHIVE     (1 << FIBB_ARCHIVE)
#define FIBF_READ        (1 << FIBB_READ)
#define FIBF_WRITE       (1 << FIBB_WRITE)
#define FIBF_EXECUTE     (1 << FIBB_EXECUTE)
#define FIBF_DELETE      (1 << FIBB_DELETE)
#define FIBF_HOLD        (1 << 7)  /* 'h' hold/resident bit; unnamed in NDK */

/* Default protection: owner R/W/E/D allowed (owner bits are inverted so
 * they stay clear), group+other read-only (GRP/OTR bits grant when set). */
#define DEFAULT_PROTECTION (FIBF_GRP_READ | FIBF_OTR_READ)

/* -------------------------------------------------------------------------
 * DosList node types
 * ------------------------------------------------------------------------- */
#define DLT_DEVICE       0
#define DLT_DIRECTORY    1   /* bound assign */
#define DLT_VOLUME       2
#define DLT_LATE         3   /* late-binding assign */
#define DLT_NONBINDING   4   /* non-binding assign */
#define DLT_PRIVATE     (-1) /* internal use only */
#define DLT_ASSIGN       5   /* UAOS-internal marker for host DosList nodes
                              * (mapped to DLT_DIRECTORY/NONBINDING for
                              * guest-visible lists) */

/* -------------------------------------------------------------------------
 * DosList — global volume/device/assign list entry
 * ------------------------------------------------------------------------- */
typedef struct DosList {
    struct DosList *dol_Next;
    uint8_t         dol_Type;      /* DLT_* */
    char            dol_Name[32];  /* volume/device/assign name */
    union {
        struct {
            struct MsgPort *dol_Handler;  /* handler MsgPort */
            BPTR            dol_Lock;     /* current dir lock */
            uint32_t        dol_DiskType;
        } dol_Device;
        struct {
            BPTR dol_Lock;        /* assign lock (0 = deferred) */
        } dol_Assign;
        struct {
            uint32_t dol_VolumeDate;
            BPTR     dol_Lock;
        } dol_Volume;
    } u;
} DosList;

/* -------------------------------------------------------------------------
 * InfoData — filled by ACTION_DISK_INFO / ACTION_INFO (Info())
 * ------------------------------------------------------------------------- */
typedef struct InfoData {
    int32_t id_NumSoftErrors;   /* 0:  number of soft errors */
    int32_t id_UnitNumber;      /* 4:  which unit disk is (was) mounted on */
    int32_t id_DiskState;       /* 8:  ID_* disk state */
    int32_t id_NumBlocks;       /* 12: total number of blocks */
    int32_t id_NumBlocksUsed;   /* 16: blocks in use */
    int32_t id_BytesPerBlock;   /* 20: bytes per block */
    int32_t id_DiskType;        /* 24: disk type code */
    BPTR    id_VolumeNode;      /* 28: BCPL pointer to volume node */
    int32_t id_InUse;           /* 32: zero if not in use */
} InfoData;

/* Disk state values (id_DiskState) — canonical AmigaDOS numbers. */
#define ID_WRITE_PROTECTED   80   /* 'P' — write protected */
#define ID_VALIDATING        81   /* 'Q' — currently being validated */
#define ID_VALIDATED         82   /* 'R' — consistent and writeable */

/* Disk type IDs (id_DiskType) */
#define ID_NO_DISK_PRESENT  (-1)
#define ID_UNREADABLE_DISK  0x42414400  /* 'BAD\0' */
#define ID_DOS_DISK         0x444F5300  /* 'DOS\0' */
#define ID_FFS_DISK         0x444F5301  /* 'DOS\1' FFS */
#define ID_INTER_DOS        0x444F5302  /* international mode */
#define ID_INTER_FFS        0x444F5303
#define ID_FASTDIR_DOS      0x444F5304
#define ID_FASTDIR_FFS      0x444F5305
#define ID_NOT_REALLY_DOS   0x4E444F53  /* 'NDOS' — a disk *type*, not a state */
#define ID_KICKSTART        0x4B49434B  /* 'KICK' */
#define ID_MSDOS_DISK       0x4D534400  /* 'MSD\0' */

#endif
