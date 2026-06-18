/* device_handler.h — Base class for non-filesystem device handlers
 *
 * Device handlers (AUX:, PORT:, etc.) process file-like I/O packets
 * but do not implement a full filesystem.  They support open, read,
 * write, close, and device-specific control packets.
 */

#ifndef UAOS_DEVICE_HANDLER_H
#define UAOS_DEVICE_HANDLER_H

#include "dos/handler.h"
#include <stdint.h>

/* Device handler types */
#define DEV_TYPE_AUX     1  /* Serial/aux */
#define DEV_TYPE_PAR     2  /* Parallel port */
#define DEV_TYPE_QUEUE   3  /* Print queue */
#define DEV_TYPE_PIPE    4  /* Pipe */

/* Maximum internal ring-buffer size for RX/TX */
#define DEV_RING_SIZE 256

typedef struct DeviceHandler {
    Handler  base;
    int      device_type;
    int      is_open;

    /* Simple byte ring buffers */
    uint8_t  rx_buf[DEV_RING_SIZE];
    int      rx_head;
    int      rx_tail;
    uint8_t  tx_buf[DEV_RING_SIZE];
    int      tx_head;
    int      tx_tail;
} DeviceHandler;

/* Create a device handler of the given type. */
DeviceHandler *DeviceHandler_Create(const char *name, int device_type);

/* Generic packet processor for device handlers. */
void DeviceHandler_ProcessPacket(Handler *h, DosPacket *pkt);

#endif
