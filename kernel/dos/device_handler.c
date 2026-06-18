/* device_handler.c — Base device handler implementation
 *
 * Provides a generic packet processor for non-filesystem handlers.
 * Uses simple ring buffers for RX/TX.
 */

#include "device_handler.h"
#include "dospacket.h"
#include "amiga_dos_types.h"
#include "boot/kprint.h"
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Ring buffer helpers
 * ------------------------------------------------------------------------- */

static inline int rb_count(int head, int tail, int size)
{
    return (tail >= head) ? (tail - head) : (size - head + tail);
}

static inline int rb_space(int head, int tail, int size)
{
    return size - rb_count(head, tail, size) - 1;
}

static inline void rb_put(uint8_t *buf, int *head, int *tail, int size, uint8_t b)
{
    int next = (*tail + 1) % size;
    if (next != *head) {
        buf[*tail] = b;
        *tail = next;
    }
}

static inline int rb_get(uint8_t *buf, int *head, int *tail, int size)
{
    if (*head == *tail) return -1;
    uint8_t b = buf[*head];
    *head = (*head + 1) % size;
    return (int)b;
}

/* -------------------------------------------------------------------------
 * Packet actions
 * ------------------------------------------------------------------------- */

static void dev_action_findinput(DeviceHandler *dh, DosPacket *pkt)
{
    (void)pkt;
    if (dh->is_open) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_IN_USE;
        return;
    }
    dh->is_open = 1;
    dh->rx_head = dh->rx_tail = 0;
    dh->tx_head = dh->tx_tail = 0;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

static void dev_action_findoutput(DeviceHandler *dh, DosPacket *pkt)
{
    (void)pkt;
    if (dh->is_open) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_IN_USE;
        return;
    }
    dh->is_open = 1;
    dh->rx_head = dh->rx_tail = 0;
    dh->tx_head = dh->tx_tail = 0;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

static void dev_action_end(DeviceHandler *dh, DosPacket *pkt)
{
    (void)pkt;
    dh->is_open = 0;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

static void dev_action_read(DeviceHandler *dh, DosPacket *pkt)
{
    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)pkt->dp_Arg2;
    uint32_t len = (uint32_t)pkt->dp_Arg3;
    if (!dh->is_open) {
        pkt->dp_Res1 = 0;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }
    uint32_t n = 0;
    while (n < len && rb_count(dh->rx_head, dh->rx_tail, DEV_RING_SIZE) > 0) {
        int b = rb_get(dh->rx_buf, &dh->rx_head, &dh->rx_tail, DEV_RING_SIZE);
        if (b < 0) break;
        if (buf) buf[n] = (uint8_t)b;
        n++;
    }
    pkt->dp_Res1 = (int32_t)n;
    pkt->dp_Res2 = 0;
}

static void dev_action_write(DeviceHandler *dh, DosPacket *pkt)
{
    const uint8_t *buf = (const uint8_t *)(uintptr_t)(uint32_t)pkt->dp_Arg2;
    uint32_t len = (uint32_t)pkt->dp_Arg3;
    if (!dh->is_open) {
        pkt->dp_Res1 = 0;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }
    uint32_t n = 0;
    while (n < len && rb_space(dh->tx_head, dh->tx_tail, DEV_RING_SIZE) > 0) {
        rb_put(dh->tx_buf, &dh->tx_head, &dh->tx_tail, DEV_RING_SIZE, buf[n]);
        n++;
    }
    pkt->dp_Res1 = (int32_t)n;
    pkt->dp_Res2 = 0;
}

static void dev_action_wait_char(DeviceHandler *dh, DosPacket *pkt)
{
    if (!dh->is_open) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }
    if (rb_count(dh->rx_head, dh->rx_tail, DEV_RING_SIZE) > 0) {
        pkt->dp_Res1 = DOSTRUE;
    } else {
        /* In a synchronous single-threaded kernel we cannot really block,
         * so report no character available. */
        pkt->dp_Res1 = DOSFALSE;
    }
    pkt->dp_Res2 = 0;
}

static void dev_action_disk_info(DeviceHandler *dh, DosPacket *pkt)
{
    (void)dh;
    InfoData *id = (InfoData *)(uintptr_t)(uint32_t)pkt->dp_Arg1;
    if (id) {
        id->id_NumBlocks = 0;
        id->id_NumBlocksUsed = 0;
        id->id_BytesPerBlock = 512;
        id->id_DiskState = ID_VALIDATED;
        id->id_NumSoftErrors = 0;
        id->id_UnitNumber = 0;
        id->id_DiskType = ID_DOS_DISK;
        id->id_VolumeNode = 0;
        id->id_InUse = 1;
    }
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* -------------------------------------------------------------------------
 * Packet dispatcher
 * ------------------------------------------------------------------------- */

void DeviceHandler_ProcessPacket(Handler *h, DosPacket *pkt)
{
    if (!h || !pkt) return;
    DeviceHandler *dh = (DeviceHandler *)h;

    switch (pkt->dp_Type) {
    case ACTION_FINDINPUT:
    case ACTION_FINDUPDATE:
        dev_action_findinput(dh, pkt);
        break;
    case ACTION_FINDOUTPUT:
        dev_action_findoutput(dh, pkt);
        break;
    case ACTION_END:
        dev_action_end(dh, pkt);
        break;
    case ACTION_READ:
        dev_action_read(dh, pkt);
        break;
    case ACTION_WRITE:
        dev_action_write(dh, pkt);
        break;
    case ACTION_WAIT_CHAR:
        dev_action_wait_char(dh, pkt);
        break;
    case ACTION_DISK_INFO:
        dev_action_disk_info(dh, pkt);
        break;
    case ACTION_DIE:
        dh->is_open = 0;
        pkt->dp_Res1 = DOSTRUE;
        pkt->dp_Res2 = 0;
        break;
    case ACTION_INHIBIT:
        pkt->dp_Res1 = DOSTRUE;
        pkt->dp_Res2 = 0;
        break;
    default:
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}

/* -------------------------------------------------------------------------
 * Constructor
 * ------------------------------------------------------------------------- */

DeviceHandler *DeviceHandler_Create(const char *name, int device_type)
{
    Handler *h = Handler_Create(name, NULL, DeviceHandler_ProcessPacket);
    if (!h) return NULL;

    DeviceHandler *dh = (DeviceHandler *)h;
    dh->device_type = device_type;
    dh->is_open = 0;
    dh->rx_head = dh->rx_tail = 0;
    dh->tx_head = dh->tx_tail = 0;
    return dh;
}
