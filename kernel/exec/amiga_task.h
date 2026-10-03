/* amiga_task.h — AmigaOS 3.1 Task/Process struct layouts (guest-RAM offsets)
 *
 * These are the exact big-endian layouts used inside the M68k guest
 * address space (now 16 MB: 8 MB chip + 8 MB fast).  Host code that sets
 * up a Process struct must write each field with the correct offset and
 * byte order.
 *
 * Offsets verified against the AmigaOS NDK (exec/tasks.i, dos/dosextens.h).
 */

#ifndef AMIGA_TASK_H
#define AMIGA_TASK_H

#include <stdint.h>

/* ========================================================================
 * struct Node / List (big-endian guest layout)
 * ======================================================================== */

#define LN_SUCC     0x00   /* APTR */
#define LN_PRED     0x04   /* APTR */
#define LN_TYPE     0x08   /* uint8 */
#define LN_PRI      0x09   /* int8  */
#define LN_NAME     0x0A   /* APTR to C string */
#define NODE_SIZE   0x0E   /* 14 bytes */

#define LH_HEAD     0x00
#define LH_TAIL     0x04
#define LH_TAILPRED 0x08
#define LH_TYPE     0x0C
#define LH_PAD      0x0D
#define LIST_SIZE   0x0E   /* 14 bytes */
#define MLH_HEAD    0x00   /* MinList: no type/pad */
#define MLH_TAIL    0x04
#define MLH_TAILPRED 0x08
#define MINLIST_SIZE 0x0C  /* 12 bytes */

/* ln_Type values */
#define NT_MSGPORT   4
#define NT_TASK      13
#define NT_PROCESS   13   /* Process IS a Task */
#define NT_LIBRARY   9
#define NT_SIGNALSEM 15

/* MsgPort flags (mp_Flags, PA_*) */
#define PA_SIGNAL    0
#define PA_SOFTINT   1
#define PA_IGNORE    2

/* ========================================================================
 * struct MsgPort — 34 bytes (exec/ports.h)
 * ======================================================================== */
#define MP_NODE      0x00   /* struct Node */
#define MP_FLAGS     0x0E
#define MP_SIGBIT    0x0F
#define MP_SIGTASK   0x10   /* APTR Task */
#define MP_MSGLIST   0x14   /* struct List */
#define MSGPORT_SIZE 0x22   /* 34 bytes */

/* ========================================================================
 * struct Task — AmigaOS layout (exec/tasks.i), 92 bytes
 * ======================================================================== */

#define TASK_SIZE           0x5C   /* 92 bytes */

/* Node header (embedded) */
#define TASK_LN_SUCC        0x00
#define TASK_LN_PRED        0x04
#define TASK_LN_TYPE        0x08
#define TASK_LN_PRI         0x09
#define TASK_LN_NAME        0x0A   /* APTR to C string */

/* Task fields */
#define TASK_TC_FLAGS       0x0E   /* uint8  */
#define TASK_TC_STATE       0x0F   /* uint8  */
#define TASK_TC_IDNESTCNT   0x10   /* int8   */
#define TASK_TC_TDNESTCNT   0x11   /* int8   */
#define TASK_TC_SIGALLOC    0x12   /* uint32 */
#define TASK_TC_SIGWAIT     0x16   /* uint32 */
#define TASK_TC_SIGRECVD    0x1A   /* uint32 */
#define TASK_TC_SIGEXCEPT   0x1E   /* uint32 */
#define TASK_TC_TRAPALLOC   0x22   /* uint16 */
#define TASK_TC_TRAPABLE    0x24   /* uint16 */
#define TASK_TC_EXCEPTDATA  0x26   /* APTR   */
#define TASK_TC_EXCEPTCODE  0x2A   /* APTR   */
#define TASK_TC_TRAPDATA    0x2E   /* APTR   */
#define TASK_TC_TRAPCODE    0x32   /* APTR   */
#define TASK_TC_SPREG       0x36   /* APTR   */
#define TASK_TC_SPLOWER     0x3A   /* APTR   */
#define TASK_TC_SPUPPER     0x3E   /* APTR   */
#define TASK_TC_SWITCH      0x42   /* APTR   */
#define TASK_TC_LAUNCH      0x46   /* APTR   */
#define TASK_TC_MEMENTRY    0x4A   /* embedded struct List (14) */
#define TASK_TC_USERDATA    0x58   /* APTR   */

/* tc_Flags bits */
#define TF_PROCTASK  0x01
#define TF_ETASK     0x02
#define TF_STACKCHK  0x04
#define TF_EXCEPT    0x08
#define TF_SWITCH    0x10
#define TF_LAUNCH    0x20

/* tc_State values */
#define TS_INVALID   0
#define TS_ADDED     1
#define TS_RUN       2
#define TS_WAIT      3
#define TS_READY     4
#define TS_REMOVED   5
#define TS_EXCEPT    6

/* ========================================================================
 * struct Process — extends Task (dos/dosextens.h)
 * ======================================================================== */

#define PROCESS_SIZE        0xE4   /* 228 bytes; we round up when allocating */

/* Process fields after the embedded Task */
#define PR_MSGPORT          0x5C   /* embedded struct MsgPort (34 bytes) */
#define PR_PAD              0x7E   /* WORD */
#define PR_SEGLIST          0x80   /* BPTR */
#define PR_STACKSIZE        0x84   /* LONG */
#define PR_GLOBVEC          0x88   /* APTR — DOS library base */
#define PR_TASKNUM          0x8C   /* LONG */
#define PR_STACKBASE        0x90   /* BPTR */
#define PR_RESULT2          0x94   /* LONG */
#define PR_CURRENTDIR       0x98   /* BPTR to Lock */
#define PR_CIS              0x9C   /* BPTR file handle */
#define PR_COS              0xA0   /* BPTR file handle */
#define PR_CONSOLETASK      0xA4   /* APTR MsgPort* */
#define PR_FILESYSTASK      0xA8   /* APTR MsgPort* */
#define PR_CLI              0xAC   /* BPTR to CommandLineInterface */
#define PR_RETURNADDR       0xB0   /* APTR */
#define PR_PKTWAIT          0xB4   /* APTR */
#define PR_WINDOWPTR        0xB8   /* APTR Window*; -1 suppresses requesters */
#define PR_HOMEDIR          0xBC   /* BPTR (V36) */
#define PR_FLAGS            0xC0   /* LONG (V36) */
#define PR_EXITCODE         0xC4   /* APTR fn */
#define PR_EXITDATA         0xC8   /* LONG */
#define PR_ARGUMENTS        0xCC   /* STRPTR */
#define PR_LOCALVARS        0xD0   /* struct MinList (12) */
#define PR_SHELLPRIVATE     0xDC   /* ULONG */
#define PR_CES              0xE0   /* BPTR */

/* Canonical field offsets (compatibility aliases used by dos_lib.c) */
#define PR_CLI_OFFSET       0xAC   /* pr_CLI  */
#define PR_CIS_OFFSET       0x9C   /* pr_CIS  */
#define PR_COS_OFFSET       0xA0   /* pr_COS  */

/* ========================================================================
 * struct CommandLineInterface (dos/dosextens.h)
 * ======================================================================== */

#define CLI_SIZE            0x80   /* allocate generously; struct is ~0x64 */

#define CLI_RESULT2         0x00   /* LONG */
#define CLI_SETNAME         0x04   /* BPTR */
#define CLI_COMMANDDIR      0x08   /* BPTR */
#define CLI_RETURNCODE      0x0C   /* LONG */
#define CLI_COMMANDNAME     0x10   /* BPTR BSTR */
#define CLI_FAILLEVEL       0x14   /* LONG */
#define CLI_PROMPT          0x18   /* BPTR */
#define CLI_DEFAULTINPUT    0x1C   /* BPTR */
#define CLI_DEFAULTOUTPUT   0x20   /* BPTR */
#define CLI_ERRORLEVEL      0x24   /* LONG */
#define CLI_BACKGROUND      0x28   /* LONG */
#define CLI_CURRENTINPUT    0x2C   /* BPTR */
#define CLI_CURRENTOUTPUT   0x30   /* BPTR */
#define CLI_RUNTIME         0x34   /* LONG */
#define CLI_CURRENTDIRNAME  0x38   /* BPTR */
#define CLI_DIRLEN          0x3C   /* LONG */
#define CLI_PROGDIR         0x40   /* BPTR — set by SetProgramDir */
#define CLI_INTERACTIVE     0x44   /* LONG */
#define CLI_SAMELEVEL       0x48   /* BPTR */
#define CLI_SETPROC         0x4C   /* APTR */
#define CLI_EXNAME          0x50   /* APTR */
#define CLI_EXNAMELEN       0x54   /* LONG */
#define CLI_MODULE          0x58   /* BPTR — seglist of running command
                                      * (some SAS/C stubs use +0x3C) */
#define CLI_MODULE_SC       0x3C   /* SAS/C convention observed in OctaMED */

/* ========================================================================
 * ExecBase (struct ExecBase / SysBase) offsets — exec/execbase.i
 * ======================================================================== */

#define EXECBASE_THIS_TASK  0x114  /* APTR ThisTask */
#define EXECBASE_IDNESTCNT  0x126  /* BYTE  IDNestCnt (-1 = enabled) */
#define EXECBASE_TDNESTCNT  0x127  /* BYTE  TDNestCnt (-1 = permitted) */
#define EXECBASE_ATTNFLAGS  0x128  /* WORD  AFF_* */
#define EXECBASE_ATTNRESCHED 0x12A /* BYTE */
#define EXECBASE_TASKSIGALLOC 0x13C /* ULONG TaskSigAlloc */
#define EXECBASE_TASKTRAPALLOC 0x140 /* UWORD TaskTrapAlloc */
#define EXECBASE_MEMLIST    0x142  /* struct List MemList */
#define EXECBASE_RESLIST    0x150  /* struct List ResourceList */
#define EXECBASE_DEVLIST    0x15E  /* struct List DeviceList */
#define EXECBASE_INTRLIST   0x16C  /* struct List IntrList */
#define EXECBASE_LIBLIST    0x17A  /* struct List LibList */
#define EXECBASE_PORTLIST   0x188  /* struct List PortList */
#define EXECBASE_TASKREADY  0x196  /* struct List TaskReady */
#define EXECBASE_TASKWAIT   0x1A4  /* struct List TaskWait */
#define EXECBASE_VBLANKFREQ 0x212  /* UBYTE (V36) */
#define EXECBASE_POWERSUPPLYFREQ 0x213 /* UBYTE (V36) */
#define EXECBASE_SEMLIST    0x214  /* struct List SemaphoreList (V36) */

/* Library struct offsets (exec/libraries.i) */
#define LIB_VERSION         0x14   /* WORD lib_Version */
#define LIB_REVISION        0x16   /* WORD lib_Revision */
#define LIB_IDSTRING        0x18   /* APTR lib_IdString */
#define LIB_SUM             0x1C   /* ULONG lib_Sum */
#define LIB_OPENCNT         0x20   /* WORD lib_OpenCnt */

#endif /* AMIGA_TASK_H */
