/* dospacket.h — AmigaDOS DosPacket and Handler Action Codes
 *
 * Minimal definitions compatible with AmigaDOS 3.1 for UAOS
 * single-threaded synchronous packet dispatch.
 */

#ifndef UAOS_DOSPACKET_H
#define UAOS_DOSPACKET_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Minimal MsgPort (synchronous — no real Exec signals needed)
 * ------------------------------------------------------------------------- */
typedef struct MsgPort {
    struct DosPacket *mp_MsgList;   /* pending packet queue head */
    const char       *mp_Name;      /* handler name (debug/diagnostic) */
} MsgPort;

/* -------------------------------------------------------------------------
 * DosPacket — universal I/O message sent to a filesystem handler
 * ------------------------------------------------------------------------- */
typedef struct DosPacket {
    struct DosPacket *dp_Next;      /* queue linkage (was dp_Link Message) */
    int32_t           dp_Type;      /* action code */
    int32_t           dp_Res1;      /* primary result (success/count/handle) */
    int32_t           dp_Res2;      /* secondary result / IoErr code */
    int32_t           dp_Arg1;
    int32_t           dp_Arg2;
    int32_t           dp_Arg3;
    int32_t           dp_Arg4;
    int32_t           dp_Arg5;
    int32_t           dp_Arg6;
    int32_t           dp_Arg7;
} DosPacket;

/* -------------------------------------------------------------------------
 * AmigaDOS Action Codes (KS 3.1 compatible)
 * ------------------------------------------------------------------------- */
#define ACTION_NIL                0
#define ACTION_GET_BLOCK          2
#define ACTION_SET_MAP            4
#define ACTION_DIE                5
#define ACTION_EVENT              6
#define ACTION_CURRENT_VOLUME     7
#define ACTION_LOCATE_OBJECT      8
#define ACTION_RENAME_DISK        9
#define ACTION_WRITE             87   /* 'W' */
#define ACTION_READ              82   /* 'R' */
#define ACTION_FREE_LOCK         15
#define ACTION_DELETE_OBJECT     16
#define ACTION_RENAME_OBJECT     17
#define ACTION_MORE_CACHE        18
#define ACTION_COPY_DIR          19
#define ACTION_WAIT_CHAR         20
#define ACTION_SET_PROTECT       21
#define ACTION_CREATE_DIR        22
#define ACTION_EXAMINE_OBJECT    23
#define ACTION_EXAMINE_NEXT      24
#define ACTION_DISK_INFO         25
#define ACTION_INFO              26
#define ACTION_FLUSH             27
#define ACTION_SET_COMMENT       28
#define ACTION_PARENT            29
#define ACTION_TIMER             30
#define ACTION_INHIBIT           31
#define ACTION_DISK_TYPE         32
#define ACTION_DISK_CHANGE       33
#define ACTION_SET_DATE          34
#define ACTION_SAME_LOCK         40
#define ACTION_FINDUPDATE      1004
#define ACTION_FINDINPUT       1005
#define ACTION_FINDOUTPUT      1006
#define ACTION_END             1007
#define ACTION_SEEK            1008
#define ACTION_WRITE_PROTECT   1023
#define ACTION_IS_FILESYSTEM   1027
#define ACTION_SET_FILE_SIZE   1030
#define ACTION_CHANGE_MODE     1034
#define ACTION_COPY_DIR_FH     1035
#define ACTION_PARENT_FH       1036
#define ACTION_EXAMINE_ALL     1037
#define ACTION_EXAMINE_FH      1038

/* -------------------------------------------------------------------------
 * Common AmigaDOS Error Codes (IoErr values)
 * ------------------------------------------------------------------------- */
#define ERROR_NO_FREE_STORE          103
#define ERROR_TASK_TABLE_FULL        105
#define ERROR_BAD_TEMPLATE           114
#define ERROR_BAD_NUMBER             115
#define ERROR_REQUIRED_ARG_MISSING   116
#define ERROR_KEY_NEEDS_ARG          117
#define ERROR_TOO_MANY_ARGS          118
#define ERROR_UNMATCHED_QUOTES       119
#define ERROR_LINE_TOO_LONG          120
#define ERROR_FILE_NOT_OBJECT        121
#define ERROR_INVALID_RES_LIBRARY    122
#define ERROR_INVALID_DIRECTORY      123
#define ERROR_OBJECT_IN_USE          202
#define ERROR_OBJECT_EXISTS          203
#define ERROR_DIR_NOT_FOUND          204
#define ERROR_OBJECT_NOT_FOUND       205
#define ERROR_BAD_STREAM_NAME        206
#define ERROR_OBJECT_TOO_LARGE       207
#define ERROR_ACTION_NOT_KNOWN       209
#define ERROR_INVALID_COMPONENT_NAME 210
#define ERROR_DEVICE_NOT_MOUNTED     215
#define ERROR_NOT_A_DOS_DISK         216
#define ERROR_DISK_FULL              221
#define ERROR_DISK_WRITE_PROTECTED   222
#define ERROR_DISK_NOT_VALIDATED     223
#define ERROR_DELETE_PROTECTED       225
#define ERROR_WRITE_PROTECTED        226
#define ERROR_READ_PROTECTED         227
#define ERROR_NOT_A_DIRECTORY        232
#define ERROR_NO_DISK_INSERTED       233
#define ERROR_NO_MORE_ENTRIES        232
#define ERROR_TOO_MANY_LEVELS        235
#define ERROR_DEVICE_NOT_FOUND       218
#define ERROR_COMMENT_TOO_BIG        213

/* -------------------------------------------------------------------------
 * Seek / Open / Lock constants
 * ------------------------------------------------------------------------- */
#define OFFSET_BEGINNING  -1
#define OFFSET_CURRENT     0
#define OFFSET_END         1

#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
#define MODE_READWRITE  1004

#define SHARED_LOCK     (-2)
#define ACCESS_READ     (-2)
#define EXCLUSIVE_LOCK  (-1)
#define ACCESS_WRITE    (-1)

/* -------------------------------------------------------------------------
 * Boolean return values in AmigaDOS
 * ------------------------------------------------------------------------- */
#define DOSTRUE   (-1)
#define DOSFALSE   0

#endif
