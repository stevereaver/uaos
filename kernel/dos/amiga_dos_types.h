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
 * Layout matches 32-bit AmigaDOS: 16 bytes, all 4-byte fields.
 * fl_Key stores the host HandleTable slot ID (opaque to guest).
 * fl_Task stores a handler marker; guest never dereferences it directly
 * because all dos.library calls are intercepted by the glue layer.
 * ------------------------------------------------------------------------- */
typedef struct FileLock {
    BPTR    fl_Key;      /* 0: disk key / handle ID */
    int32_t fl_Access;   /* 4: SHARED_LOCK (-2) or EXCLUSIVE_LOCK (-1) */
    BPTR    fl_Task;     /* 8: handler marker (not a real guest pointer) */
    BPTR    fl_Volume;   /* 12: DosList BPTR (0 for now) */
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
#define ST_ROOTDIR    1
#define ST_USERDIR    2
#define ST_LINKDIR    3
#define ST_FILE      (-3)
#define ST_LINKFILE  (-4)
#define ST_PIPEFILE  (-5)
#define ST_SOCKET    (-6)

/* -------------------------------------------------------------------------
 * Protection bits (AmigaDOS 3.x extended set)
 * ------------------------------------------------------------------------- */
#define FIBF_SCRIPT     (1 << 0)
#define FIBF_PURE       (1 << 1)
#define FIBF_ARCHIVE    (1 << 2)
#define FIBF_READ       (1 << 3)
#define FIBF_WRITE      (1 << 4)
#define FIBF_EXECUTE    (1 << 5)
#define FIBF_DELETE     (1 << 6)
#define FIBF_HOLD       (1 << 7)
#define FIBF_OTR_READ   (1 << 8)
#define FIBF_OTR_WRITE  (1 << 9)
#define FIBF_OTR_EXECUTE (1 << 10)
#define FIBF_OTR_DELETE  (1 << 11)
#define FIBF_GRP_READ    (1 << 12)
#define FIBF_GRP_WRITE   (1 << 13)
#define FIBF_GRP_EXECUTE (1 << 14)
#define FIBF_GRP_DELETE  (1 << 15)

/* Default protection: owner R/W/E/D, group+other R only */
#define DEFAULT_PROTECTION (0xFFE0 | FIBF_GRP_READ | FIBF_OTR_READ)

/* -------------------------------------------------------------------------
 * DosList node types
 * ------------------------------------------------------------------------- */
#define DLT_DEVICE       0
#define DLT_DIRECTORY    1
#define DLT_VOLUME       2
#define DLT_LATE         3
#define DLT_LOCK         4
#define DLT_ASSIGN       5
#define DLT_NONBINDING   6
#define DLT_PRIVATE      7

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
    int32_t id_NumBlocks;       /* total number of blocks */
    int32_t id_NumBlocksUsed;   /* blocks in use */
    int32_t id_BytesPerBlock;   /* bytes per block */
    int32_t id_DiskState;       /* disk state */
    int32_t id_NumSoftErrors;   /* number of soft errors */
    int32_t id_UnitNumber;      /* unit number */
    int32_t id_DiskType;        /* disk type code */
    int32_t id_VolumeNode;      /* volume node */
    int32_t id_InUse;           /* in-use flag */
} InfoData;

/* Disk state values */
#define ID_VALIDATED         0
#define ID_WRITE_PROTECTED   1
#define ID_ERROR             2
#define ID_NO_DISK_PRESENT   3

/* Common disk type IDs */
#define ID_DOS_DISK     0x444F5300  /* 'DOS\0' */
#define ID_FFS_DISK     0x444F5301  /* 'DOS\1' FFS */
#define ID_INTER_DOS    0x444F5302  /* international mode */
#define ID_INTER_FFS    0x444F5303
#define ID_FASTDIR_DOS  0x444F5304
#define ID_FASTDIR_FFS  0x444F5305
#define ID_KICKSTART    0x4B49434B  /* 'KICK' */

#endif
