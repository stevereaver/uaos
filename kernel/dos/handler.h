/* handler.h — AmigaDOS filesystem handler abstraction
 *
 * A Handler is a packet-processing object attached to a MsgPort.
 * UAOS dispatches DosPackets synchronously because the kernel is
 * single-threaded; this header defines the generic dispatch plumbing.
 */

#ifndef UAOS_HANDLER_H
#define UAOS_HANDLER_H

#include "dos/dospacket.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Handler object
 * ------------------------------------------------------------------------- */
typedef struct Handler {
    MsgPort         port;       /* embedded packet port */
    const char     *name;       /* human-readable name, e.g. "ram-handler" */
    void           *private;    /* filesystem-specific state (RamFsVol*, etc.) */
    void (*ProcessPacket)(struct Handler *h, DosPacket *pkt);
} Handler;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Create a handler.  Returns NULL if handler pool exhausted. */
Handler *Handler_Create(const char *name, void *private,
                        void (*proc)(struct Handler *, DosPacket *));

/* -------------------------------------------------------------------------
 * Packet dispatch (synchronous — UAOS is single-threaded)
 * ------------------------------------------------------------------------- */

/* Build a DosPacket and synchronously dispatch it to the handler.
 * Returns dp_Res1 (the primary result).
 * On failure (port NULL) returns 0 and sets global IoErr. */
int32_t DoPkt(MsgPort *port, int32_t action,
              int32_t arg1, int32_t arg2, int32_t arg3,
              int32_t arg4, int32_t arg5);

/* Get handler pointer from its embedded port pointer.
 * Useful when you only have a MsgPort* (e.g. from DosList). */
Handler *Handler_FromPort(MsgPort *port);

/* -------------------------------------------------------------------------
 * Global IoErr (single-threaded = one global value)
 * ------------------------------------------------------------------------- */
extern int32_t g_dos_last_ioerr;

static inline void SetIoErr(int32_t code) { g_dos_last_ioerr = code; }
static inline int32_t IoErr(void) { return g_dos_last_ioerr; }

#endif
