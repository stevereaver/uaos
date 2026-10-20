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

/* ln_Type values (exec/nodes.h) */
#define NT_UNKNOWN      0
#define NT_TASK         1   /* Exec task */
#define NT_INTERRUPT    2
#define NT_DEVICE       3
#define NT_MSGPORT      4
#define NT_MESSAGE      5
#define NT_FREEMSG      6
#define NT_REPLYMSG     7
#define NT_RESOURCE     8
#define NT_LIBRARY      9
#define NT_MEMORY      10
#define NT_SOFTINT     11
#define NT_FONT        12
#define NT_PROCESS     13   /* AmigaDOS Process (a Task with DOS fields) */
#define NT_SEMAPHORE   14
#define NT_SIGNALSEM   15
#define NT_BOOTNODE    16
#define NT_KICKMEM     17
#define NT_GRAPHICS    18
#define NT_DEATHMESSAGE 19

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

/* tc_Flags bits (exec/tasks.h) */
#define TB_ETASK     3
#define TB_STACKCHK  4
#define TB_EXCEPT    5
#define TB_SWITCH    6
#define TB_LAUNCH    7

#define TF_PROCTIME  0x01   /* (1L<<0) — task is timing-sensitive */
#define TF_ETASK     0x08   /* (1L<<3) */
#define TF_STACKCHK  0x10   /* (1L<<4) */
#define TF_EXCEPT    0x20   /* (1L<<5) */
#define TF_SWITCH    0x40   /* (1L<<6) */
#define TF_LAUNCH    0x80   /* (1L<<7) */

/* tc_State values (exec/tasks.h) */
#define TS_INVALID   0
#define TS_ADDED     1
#define TS_RUN       2
#define TS_READY     3
#define TS_WAIT      4
#define TS_EXCEPT    5
#define TS_REMOVED   6

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

#define CLI_SIZE            0x80   /* canonical struct is 0x40; UAOS uses
                                    * 0x40-0x7F for private extensions */

/* Canonical V36 CommandLineInterface (dos/dosextens.h) — the only part of
 * the struct guests may rely on. */
#define CLI_RESULT2         0x00   /* LONG — IoErr of last command */
#define CLI_SETNAME         0x04   /* BSTR — directory for 'set' cmd */
#define CLI_COMMANDDIR      0x08   /* BPTR — head of path locklist */
#define CLI_RETURNCODE      0x0C   /* LONG — return code of last cmd */
#define CLI_COMMANDNAME     0x10   /* BSTR — name of current command */
#define CLI_FAILLEVEL       0x14   /* LONG — level for 'fail' */
#define CLI_PROMPT          0x18   /* BSTR — prompt string */
#define CLI_STANDARDINPUT   0x1C   /* BPTR — default (terminal) input */
#define CLI_CURRENTINPUT    0x20   /* BPTR — input actually used */
#define CLI_COMMANDFILE     0x24   /* BSTR — name of EXECUTE cmd file */
#define CLI_INTERACTIVE     0x28   /* LONG — prompts required? */
#define CLI_BACKGROUND      0x2C   /* LONG — created by RUN? */
#define CLI_CURRENTOUTPUT   0x30   /* BPTR — output actually used */
#define CLI_DEFAULTSTACK    0x34   /* LONG — stack size in longwords */
#define CLI_STANDARDOUTPUT  0x38   /* BPTR — default (terminal) output */
#define CLI_MODULE          0x3C   /* BPTR — seglist of running command */

/* UAOS private extension area (0x40+) — mirrors the fields programs used
 * to find in the 1.3-era CLI plus a few UAOS conveniences. */
#define CLI_XPROGDIR        0x40   /* BPTR — SetProgramDir() lock */
#define CLI_XCURDIRNAME     0x44   /* BSTR — current directory name */
#define CLI_XCOMMANDLINE    0x48   /* BSTR — full command line text */
#define CLI_XERRORLEVEL     0x4C   /* LONG — error level (1.3: 0x24) */
#define CLI_XRUNTIME        0x50   /* LONG — runtime used (1.3: 0x34) */

/* ========================================================================
 * ExecBase (struct ExecBase / SysBase) offsets — exec/execbase.i
 * ======================================================================== */

#define EXECBASE_THIS_TASK  0x114  /* APTR ThisTask */
#define EXECBASE_IDNESTCNT  0x126  /* BYTE  IDNestCnt (-1 = enabled) */
#define EXECBASE_TDNESTCNT  0x127  /* BYTE  TDNestCnt (-1 = permitted) */
#define EXECBASE_ATTNFLAGS  0x128  /* WORD  AFF_* */
#define EXECBASE_ATTNRESCHED 0x12A /* BYTE */
#define EXECBASE_SOFTVER    0x22   /* UWORD SoftVer (kickstart release) */
#define EXECBASE_MAXLOCMEM  0x3E   /* ULONG MaxLocMem (top of chip RAM) */
#define EXECBASE_MAXEXTMEM  0x4E   /* APTR MaxExtMem (top of ext/fast)  */
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

/* -------------------------------------------------------------------------
 * struct SemaphoreRequest / SignalSemaphore / SemaphoreMessage
 * (exec/semaphores.h, guest layout — 46/24/12 bytes)
 * ------------------------------------------------------------------------- */
#define SSR_LINK    0x00   /* MinNode (8)   */
#define SSR_WAITER  0x08   /* struct Task * — size 12 */

#define SS_LINK        0x00   /* struct Node (14)          */
#define SS_NESTCOUNT   0x0E   /* WORD                      */
#define SS_WAITQUEUE   0x10   /* struct MinList (12)       */
#define SS_MULTIPLELINK 0x1C  /* SemaphoreRequest (12)     */
#define SS_OWNER       0x28   /* struct Task *             */
#define SS_QUEUECNT    0x2C   /* WORD — size 46            */

#define SSM_MESSAGE    0x00   /* struct Message (20)       */
#define SSM_SEMAPHORE  0x14   /* struct SignalSemaphore * — size 24 */

#endif /* AMIGA_TASK_H */
