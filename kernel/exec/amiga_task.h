/* amiga_task.h — AmigaOS 3.1 Task/Process struct layouts (guest-RAM offsets)
 *
 * These are the exact big-endian layouts used inside the 2 MB M68k guest
 * address space.  Host code that sets up a Process struct must write
 * each field with the correct offset and byte order.
 *
 * Offsets are verified against the AmigaOS 3.1 NDK include files.
 */

#ifndef AMIGA_TASK_H
#define AMIGA_TASK_H

#include <stdint.h>

/* ========================================================================
 * struct Node (8 bytes minimum, but we only need what Task embeds)
 * ======================================================================== */

/* ln_Type values */
#define NT_TASK     13
#define NT_PROCESS  13  /* Process IS a Task */

/* ========================================================================
 * struct Task — AmigaOS 3.1 layout (big-endian in guest RAM)
 * ======================================================================== */

#define TASK_SIZE           0x5A   /* 90 bytes */

/* Node header (embedded, not a pointer) */
#define TASK_LN_SUCC        0x00   /* BPTR to next Node */
#define TASK_LN_PRED        0x04   /* BPTR to prev Node */
#define TASK_LN_TYPE        0x08   /* uint8 */
#define TASK_LN_PRI         0x09   /* int8  */
#define TASK_LN_NAME        0x0A   /* BPTR to name BSTR */

/* Task fields */
#define TASK_TC_FLAGS       0x0E   /* uint8  */
#define TASK_TC_STATE       0x0F   /* uint8  */
#define TASK_TC_IDNESTCNT   0x10   /* int8   */
#define TASK_TD_TDNESTCNT   0x11   /* int8   */
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
#define TASK_TC_SPUPPER     0x3A   /* APTR   */
#define TASK_TC_SPLOWER     0x3E   /* APTR   */
#define TASK_TC_MEMENTRY    0x42   /* struct List * */
#define TASK_TC_USERDATA    0x46   /* APTR   */

/* tc_Flags bits */
#define TF_PROCTASK  0x01
#define TF_ETASK     0x02
#define TF_STACKCHK  0x04
#define TF_EXCEPT    0x08
#define TF_SWITCH    0x10
#define TF_LAUNCH    0x20

/* tc_State values */
#define TS_INVALID   0
#define TS_ADDED     1  /* Ready to run */
#define TS_RUN       2  /* Running */
#define TS_WAIT      3  /* Waiting for signals */
#define TS_READY     4  /* On ready queue */
#define TS_REMOVED   5

/* ========================================================================
 * struct Process — extends Task (big-endian in guest RAM)
 * ======================================================================== */

#define PROCESS_SIZE        0xC0   /* 192 bytes (varies by Kickstart) */

/* Process fields after the embedded Task */
#define PR_TASK             0x00   /* Process starts with Task */
#define PR_PORT             0x5A   /* struct MsgPort */
#define PR_WAITPORT         0x72   /* struct MsgPort * */
#define PR_WORKDIR          0x76   /* BPTR to Lock */
#define PR_CLI              0x7A   /* BPTR to CLI struct */
#define PR_RETURNADDR       0x7E   /* APTR   */
#define PR_PCSAVEFP         0x82   /* APTR   */
#define PR_ARGS             0x86   /* APTR   */
#define PR_CIS              0x8A   /* BPTR   */
#define PR_COS              0x8E   /* BPTR   */
#define PR_CONSNODE         0x92   /* struct FileLock * */
#define PR_CURRDIR          0x96   /* struct FileLock * */
#define PR_CES              0x9A   /* BPTR   */

/* These offsets are critical for CLI detection */
#define PR_CLI_OFFSET       0x7A   /* pr_CLI  (offset from Process start) */
#define PR_CIS_OFFSET       0x8A   /* pr_CIS  */
#define PR_COS_OFFSET       0x8E   /* pr_COS  */

/* ========================================================================
 * struct CLI — minimal layout (big-endian in guest RAM)
 * ======================================================================== */

#define CLI_SIZE            0x40   /* 64 bytes (simplified) */

#define CLI_COMMANDNAME     0x00   /* BPTR to BSTR */
#define CLI_COMMANDDIR      0x04   /* BPTR */
#define CLI_EXITCODE        0x08   /* int32  */
#define CLI_RETURNCODE      0x0C   /* int32  */
#define CLI_COMMANDNUM      0x10   /* uint32 */
#define CLI_RESULT2         0x14   /* int32  */
#define CLI_SETNAME         0x18   /* BPTR   */
#define CLI_COMMANDFILE     0x1C   /* BPTR   */
#define CLI_PROMPT          0x20   /* BPTR   */
#define CLI_COMMANDLINE     0x2C   /* BPTR to BSTR */

/* ========================================================================
 * ExecBase offsets (within exec.library base)
 * ======================================================================== */

#define EXECBASE_THIS_TASK  0x114  /* APTR — current Task/Process */

#endif /* AMIGA_TASK_H */
