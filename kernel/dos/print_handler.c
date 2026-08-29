/* print_handler.c — PRT: parallel port print device handler
 *
 * Extends the generic DeviceHandler with LPT1 hardware output.
 * When data is written to PRT:, bytes are strobed out to the host
 * parallel port at 0x378. If no LPT1 is detected, data is buffered
 * in the DeviceHandler's ring buffer (and discarded when full).
 */

#include "print_handler.h"
#include "device_handler.h"
#include "dospacket.h"
#include "amiga_dos_types.h"
#include "boot/kprint.h"
#include <stddef.h>
#include <stdint.h>

/* LPT1 hardware registers */
#define LPT1_DATA 0x378
#define LPT1_STAT 0x379
#define LPT1_CTRL 0x37A

/* Status port bits */
#define LPT_STAT_BUSY  0x80  /* inverted: 1 = not busy */
#define LPT_STAT_ACK   0x40
#define LPT_STAT_PAPER 0x20
#define LPT_STAT_SEL   0x10
#define LPT_STAT_ERR   0x08

/* Control port bits */
#define LPT_CTRL_STROBE 0x01
#define LPT_CTRL_INIT   0x04
#define LPT_CTRL_SLIN   0x08  /* select printer */

static inline void pio_outb(uint8_t val, uint16_t port)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t pio_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static int g_lpt1_present = -1;  /* -1 = not probed, 0 = absent, 1 = present */

static void lpt1_probe(void)
{
    uint8_t s = pio_inb(LPT1_STAT);
    g_lpt1_present = (s != 0xFFu) ? 1 : 0;
    if (g_lpt1_present) {
        /* Initialize printer: select + init line high */
        pio_outb(LPT_CTRL_SLIN | LPT_CTRL_INIT, LPT1_CTRL);
    }
}

/* Custom write action — overrides DeviceHandler's ring-buffer write */
static void print_action_write(Handler *h, DosPacket *pkt)
{
    DeviceHandler *dh = (DeviceHandler *)h;
    const uint8_t *buf = (const uint8_t *)(uintptr_t)(uint32_t)pkt->dp_Arg2;
    uint32_t len = (uint32_t)pkt->dp_Arg3;

    if (!dh->is_open) {
        pkt->dp_Res1 = 0;
        pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return;
    }

    uint32_t n = 0;
    for (n = 0; n < len; n++) {
        if (buf) {
            if (g_lpt1_present > 0) {
                /* Send directly to LPT1 hardware */
                pio_outb(buf[n], LPT1_DATA);
                /* Strobe */
                pio_outb(LPT_CTRL_SLIN | LPT_CTRL_INIT | LPT_CTRL_STROBE, LPT1_CTRL);
                /* Brief delay for strobe */
                for (volatile int i = 0; i < 100; i++);
                pio_outb(LPT_CTRL_SLIN | LPT_CTRL_INIT, LPT1_CTRL);
            } else {
                /* No hardware — buffer in ring */
                int space = DEV_RING_SIZE -
                    ((dh->tx_tail >= dh->tx_head) ?
                     (dh->tx_tail - dh->tx_head) :
                     (DEV_RING_SIZE - dh->tx_head + dh->tx_tail)) - 1;
                if (space <= 0) break;
                dh->tx_buf[dh->tx_tail] = buf[n];
                dh->tx_tail = (dh->tx_tail + 1) % DEV_RING_SIZE;
            }
        }
    }

    pkt->dp_Res1 = (int32_t)n;
    pkt->dp_Res2 = 0;
}

/* Custom packet processor — delegates to DeviceHandler for most actions,
 * but overrides ACTION_WRITE to send to LPT1 */
static void print_process_packet(Handler *h, DosPacket *pkt)
{
    if (!h || !pkt) return;

    if (pkt->dp_Type == ACTION_WRITE) {
        print_action_write(h, pkt);
        return;
    }

    /* Delegate everything else to the generic device handler */
    DeviceHandler_ProcessPacket(h, pkt);
}

Handler *PrintHandler_Create(const char *name)
{
    if (g_lpt1_present < 0) lpt1_probe();

    Handler *h = Handler_Create(name ? name : "print-handler",
                                NULL, print_process_packet);
    if (!h) return NULL;

    DeviceHandler *dh = (DeviceHandler *)h;
    dh->device_type = DEV_TYPE_QUEUE;
    dh->is_open = 0;
    dh->rx_head = dh->rx_tail = 0;
    dh->tx_head = dh->tx_tail = 0;
    return h;
}

int PrintHandler_SendByte(uint8_t c)
{
    if (g_lpt1_present < 0) lpt1_probe();
    if (g_lpt1_present <= 0) return 0;

    pio_outb(c, LPT1_DATA);
    pio_outb(LPT_CTRL_SLIN | LPT_CTRL_INIT | LPT_CTRL_STROBE, LPT1_CTRL);
    for (volatile int i = 0; i < 100; i++);
    pio_outb(LPT_CTRL_SLIN | LPT_CTRL_INIT, LPT1_CTRL);
    return 1;
}

int PrintHandler_IsPresent(void)
{
    if (g_lpt1_present < 0) lpt1_probe();
    return g_lpt1_present > 0 ? 1 : 0;
}

void PrintHandler_Flush(void)
{
    /* Nothing to do — writes are sent immediately when LPT1 present */
}
