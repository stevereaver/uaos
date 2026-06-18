/* handler.c — Handler creation and synchronous DoPkt dispatch */

#include "handler.h"
#include <stddef.h>
#include <stdint.h>

/* Strace forward declaration for DOS packet tracing */
extern void Strace_DosPacket(int32_t action, int32_t arg1, int32_t arg2,
                             int32_t result, int32_t ioerr);

/* -------------------------------------------------------------------------
 * Static handler pool
 * ------------------------------------------------------------------------- */
#define MAX_HANDLERS 32

static Handler g_pool[MAX_HANDLERS];
static int     g_pool_used = 0;

int32_t g_dos_last_ioerr = 0;

/* -------------------------------------------------------------------------
 * Async pending packet queue (single-threaded kernel)
 * ------------------------------------------------------------------------- */
#define MAX_PENDING 32

static PendingPacket g_pending_pool[MAX_PENDING];
static PendingPacket *g_pending_head = NULL;
static int           g_pending_used = 0;

/* -------------------------------------------------------------------------
 * Handler_Create
 * ------------------------------------------------------------------------- */
Handler *Handler_Create(const char *name, void *private,
                        void (*proc)(struct Handler *, DosPacket *))
{
    if (g_pool_used >= MAX_HANDLERS) return NULL;
    Handler *h = &g_pool[g_pool_used++];
    h->port.mp_MsgList = NULL;
    h->port.mp_Name    = name;
    h->name            = name;
    h->private         = private;
    h->ProcessPacket   = proc;
    return h;
}

/* -------------------------------------------------------------------------
 * Handler_FromPort — port is embedded inside Handler, so we can recover it
 * ------------------------------------------------------------------------- */
Handler *Handler_FromPort(MsgPort *port)
{
    if (!port) return NULL;
    /* Because Handler starts with the MsgPort, the port pointer == handler */
    return (Handler *)port;
}

/* -------------------------------------------------------------------------
 * DoPkt — synchronous dispatch
 * ------------------------------------------------------------------------- */
int32_t DoPkt(MsgPort *port, int32_t action,
              int32_t arg1, int32_t arg2, int32_t arg3,
              int32_t arg4, int32_t arg5)
{
    if (!port || !port->mp_Name) {
        g_dos_last_ioerr = ERROR_DEVICE_NOT_MOUNTED;
        return 0;
    }

    DosPacket pkt;
    pkt.dp_Next = NULL;
    pkt.dp_Type = action;
    pkt.dp_Res1 = 0;
    pkt.dp_Res2 = 0;
    pkt.dp_Arg1 = arg1;
    pkt.dp_Arg2 = arg2;
    pkt.dp_Arg3 = arg3;
    pkt.dp_Arg4 = arg4;
    pkt.dp_Arg5 = arg5;
    pkt.dp_Arg6 = 0;
    pkt.dp_Arg7 = 0;

    Handler *h = Handler_FromPort(port);
    if (h && h->ProcessPacket) {
        h->ProcessPacket(h, &pkt);
        if (pkt.dp_Res2 != 0)
            g_dos_last_ioerr = pkt.dp_Res2;
        else
            g_dos_last_ioerr = 0;
    } else {
        pkt.dp_Res1 = DOSFALSE;
        pkt.dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        g_dos_last_ioerr = pkt.dp_Res2;
    }

    /* DEBUG: Strace disabled to test for lockups
     * Strace_DosPacket(action, arg1, arg2, pkt.dp_Res1, pkt.dp_Res2);
     */

    return pkt.dp_Res1;
}

/* -------------------------------------------------------------------------
 * SendPktAsync — queue packet for later dispatch
 * ------------------------------------------------------------------------- */
int32_t SendPktAsync(MsgPort *port, DosPacket *pkt, MsgPort *reply_port)
{
    if (!port || !pkt) return 0;

    if (g_pending_used >= MAX_PENDING) return 0;

    PendingPacket *pp = &g_pending_pool[g_pending_used++];
    pp->pkt = pkt;
    pp->port = port;
    pp->reply_port = reply_port;
    pp->timestamp = 0;
    pp->next = g_pending_head;
    g_pending_head = pp;
    return 1;
}

/* -------------------------------------------------------------------------
 * Handler_CheckReplies — dispatch all pending packets synchronously
 * ------------------------------------------------------------------------- */
void Handler_CheckReplies(void)
{
    PendingPacket *pp = g_pending_head;
    g_pending_head = NULL;
    g_pending_used = 0;

    while (pp) {
        PendingPacket *next = pp->next;
        DosPacket *pkt = pp->pkt;

        Handler *h = Handler_FromPort(pp->port);
        if (h && h->ProcessPacket && pkt) {
            h->ProcessPacket(h, pkt);
            if (pkt->dp_Res2 != 0)
                g_dos_last_ioerr = pkt->dp_Res2;
            else
                g_dos_last_ioerr = 0;
        } else if (pkt) {
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
            g_dos_last_ioerr = pkt->dp_Res2;
        }

        pp = next;
    }
}
