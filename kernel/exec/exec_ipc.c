/* exec_ipc.c — Minimal AmigaOS-compatible IPC primitives
 *
 * WaitPort, GetMsg, PutMsg, ReplyMsg backed by the signal-based
 * scheduler in task.c.  DOS packet handlers use these for safe
 * multitasking.
 */

#include "task.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Minimal Node / List types (AmigaOS compatible)
 * ------------------------------------------------------------------------- */

typedef struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
} MinNode;

typedef struct MinList {
    MinNode *mlh_Head;
    MinNode *mlh_Tail;
    MinNode *mlh_TailPred;
} MinList;

/* -------------------------------------------------------------------------
 * MsgPort — minimal layout (big-endian guest offsets when needed)
 * ------------------------------------------------------------------------- */

typedef struct MsgPort {
    /* Node header */
    struct MsgPort *mp_Succ;
    struct MsgPort *mp_Pred;
    uint8_t         mp_Type;
    int8_t          mp_Pri;
    const char     *mp_Name;

    /* MsgPort fields */
    uint8_t   mp_Flags;
    uint8_t   mp_SigBit;     /* signal bit for this port */
    UaosTask *mp_SigTask;    /* host task to signal */
    MinList   mp_MsgList;    /* pending messages */
} MsgPort;

/* -------------------------------------------------------------------------
 * Message — minimal layout
 * ------------------------------------------------------------------------- */

typedef struct Message {
    /* Node header */
    struct Message *mn_Succ;
    struct Message *mn_Pred;
    uint8_t         mn_Type;
    int8_t          mn_Pri;
    const char     *mn_Name;

    /* Message fields */
    uint16_t  mn_Length;
    MsgPort  *mn_ReplyPort;  /* host pointer to reply port */
    void     *mn_Data;       /* message data */
} Message;

/* -------------------------------------------------------------------------
 * List helpers (MinList)
 * ------------------------------------------------------------------------- */

static void min_list_init(MinList *list)
{
    list->mlh_Head     = (MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (MinNode *)&list->mlh_Head;
}

static int min_list_empty(MinList *list)
{
    return list->mlh_Head->mln_Succ == NULL;
}

static void min_list_add_tail(MinList *list, MinNode *node)
{
    node->mln_Pred = list->mlh_TailPred;
    node->mln_Succ = (MinNode *)&list->mlh_Tail;
    list->mlh_TailPred->mln_Succ = node;
    list->mlh_TailPred = node;
}

static void min_list_remove(MinNode *node)
{
    node->mln_Pred->mln_Succ = node->mln_Succ;
    node->mln_Succ->mln_Pred = node->mln_Pred;
}

static MinNode *min_list_remove_head(MinList *list)
{
    MinNode *node = list->mlh_Head;
    if (node->mln_Succ == NULL) return NULL;
    min_list_remove(node);
    return node;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void NewPort(MsgPort *port, const char *name, uint8_t sigbit)
{
    if (!port) return;
    port->mp_Succ  = port;
    port->mp_Pred  = port;
    port->mp_Type  = 4;   /* NT_MSGPORT */
    port->mp_Pri   = 0;
    port->mp_Name  = name;
    port->mp_Flags = 0;
    port->mp_SigBit = sigbit;
    port->mp_SigTask = Task_Current();
    min_list_init(&port->mp_MsgList);
}

void WaitPort(MsgPort *port)
{
    if (!port) return;
    while (min_list_empty(&port->mp_MsgList)) {
        Wait(1U << port->mp_SigBit);
    }
}

Message *GetMsg(MsgPort *port)
{
    if (!port) return NULL;
    if (min_list_empty(&port->mp_MsgList)) return NULL;
    return (Message *)min_list_remove_head(&port->mp_MsgList);
}

void PutMsg(MsgPort *port, Message *msg)
{
    if (!port || !msg) return;
    min_list_add_tail(&port->mp_MsgList, (MinNode *)msg);
    if (port->mp_SigTask) {
        Signal(port->mp_SigTask, 1U << port->mp_SigBit);
    }
}

void ReplyMsg(Message *msg)
{
    if (!msg || !msg->mn_ReplyPort) return;
    PutMsg(msg->mn_ReplyPort, msg);
}
