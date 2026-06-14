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
 * ------------------------------------------------------------------------- */
typedef struct FileLock {
    BPTR           fl_Link;      /* next lock in list (or 0) */
    int32_t        fl_Key;       /* disk key / unique ID */
    int32_t        fl_Access;    /* SHARED_LOCK or EXCLUSIVE_LOCK */
    struct MsgPort  *fl_Volume;   /* handler MsgPort for this lock */
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
#define DEFAULT_PROTECTION (0xFFFFFFE0 | FIBF_GRP_READ | FIBF_OTR_READ)

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

#endif
