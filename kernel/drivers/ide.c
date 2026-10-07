/* ide.c — UAOS IDE/ATA + ATAPI Block Device Driver */

#include "ide.h"
#include "../dos/blockdev.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* I/O helpers */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* PCI */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_config_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"(PCI_CONFIG_ADDRESS));
    uint32_t r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(PCI_CONFIG_DATA)); return r;
}
static uint16_t pci_config_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t dw = pci_config_read_dword(bus, dev, func, offset & 0xFC);
    return (uint16_t)(dw >> ((offset & 2) * 8));
}
static uint8_t pci_config_read_byte(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t dw = pci_config_read_dword(bus, dev, func, offset & 0xFC);
    return (uint8_t)(dw >> ((offset & 3) * 8));
}
static void pci_config_write_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t addr = (1u << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    __asm__ volatile ("outl %0, %1" :: "a"(addr), "Nd"(PCI_CONFIG_ADDRESS));
    int shift = (offset & 2) * 8;
    uint32_t cur; __asm__ volatile ("inl %1, %0" : "=a"(cur) : "Nd"(PCI_CONFIG_DATA));
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    __asm__ volatile ("outl %0, %1" :: "a"(cur), "Nd"(PCI_CONFIG_DATA));
}

#define PCI_REG_COMMAND        0x04
#define PCI_CMD_IO_SPACE       0x01
#define PCI_CMD_BUS_MASTER     0x04
#define PCI_REG_INTERRUPT_LINE 0x3C
#define PCI_REG_HEADER_TYPE    0x0E
#define PCI_HEADER_MULTIFUNC   0x80

/* Globals */
static IdeChannel g_channels[2];
static IdeDeviceInfo g_devices[2][2];
static int g_num_channels = 0;
static int g_pci_bus = -1, g_pci_dev = -1, g_pci_fn = -1;
static int g_pci_pif = -1, g_pci_intline = -1;

/* ~1 ms busy-spin via port-0x80 reads (~1 us each on LPC). */
static void ide_ms_spin(uint32_t ms) {
    for (uint32_t i = 0; i < ms * 1000; i++)
        __asm__ volatile ("inb $0x80, %%al" ::: "eax");
}

/* Wait helpers — one ide_ms_spin per loop so timeout_ms is real ms on
 * metal (an iteration count of ~100 at PCI I/O speed under-waits ~3x). */
static int wait_bsy_clear(const IdeChannelPorts *p, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        uint8_t s = inb(p->cmd_stat_port);
        if (s == 0xFF) return -1; /* No device present (floating bus) */
        if (!(s & ATA_SR_BSY)) return 0;
        ide_ms_spin(1);
    }
    return -1;
}
static int wait_drq_or_err(const IdeChannelPorts *p, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        uint8_t s = inb(p->cmd_stat_port);
        if (s == 0xFF) return -1;
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DRQ) return 1;
        /* Not-BSY with no DRQ = command done. Tolerate a few ms of idle-
         * looking status first: real drives may not have raised BSY yet. */
        if (!(s & ATA_SR_BSY) && i >= 4) return 0;
        ide_ms_spin(1);
    }
    return -2;
}

/* Software reset */
static void ide_soft_reset(const IdeChannelPorts *p) {
    outb(p->ctl_alt_port, 0x04); io_wait(); io_wait(); io_wait(); io_wait();
    outb(p->ctl_alt_port, 0x00); io_wait(); io_wait(); io_wait(); io_wait();
    wait_bsy_clear(p, 2000);
    /* ATAPI devices may run self-diagnostics without asserting BSY; give
     * the signature registers a moment to settle before probing. */
    ide_ms_spin(200);
}

/* Select device */
static void ide_select_device(const IdeChannelPorts *p, int device) {
    outb(p->devsel_port, device ? 0xB0 : 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait();
}

/* Parse ATA IDENTIFY */
static void ata_parse_identify(uint16_t *buf, IdeDeviceInfo *info) {
    info->removable = (buf[0] & 0x80) ? 1 : 0;
    for (int i = 0; i < 20; i++) {
        uint16_t w = buf[27 + i];
        info->model[i * 2] = (char)(w >> 8);
        info->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    info->model[40] = '\0';
    for (int i = 39; i >= 0; i--) { if (info->model[i] != ' ') break; info->model[i] = '\0'; }
    int lba48 = (buf[83] & 0x0400) ? 1 : 0;
    uint32_t lba28 = ((uint32_t)buf[61] << 16) | buf[60];
    uint64_t lba48c = ((uint64_t)buf[103] << 48) | ((uint64_t)buf[102] << 32) | ((uint64_t)buf[101] << 16) | buf[100];
    info->sector_size = 512;
    info->lba48 = lba48;
    info->num_sectors = (lba48 && lba48c > 0) ? lba48c : lba28;
}

/* Parse ATAPI IDENTIFY */
static void atapi_parse_identify(uint16_t *buf, IdeDeviceInfo *info) {
    info->type = IDE_DEV_ATAPI;
    info->atapi_capable = 1;
    info->sector_size = 2048;
    info->removable = (buf[0] & 0x0080) ? 1 : 0;
    for (int i = 0; i < 20; i++) {
        uint16_t w = buf[27 + i];
        info->model[i * 2] = (char)(w >> 8);
        info->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    info->model[40] = '\0';
    for (int i = 39; i >= 0; i--) { if (info->model[i] != ' ') break; info->model[i] = '\0'; }
}

/* Send IDENTIFY to a device.
 * Presence is decided by the post-reset signature registers, read BEFORE
 * any command-block writes clobber them: ATAPI = sc01/lba01/14/eb,
 * ATA = sc01/lba01/00/00. A floating or undriven bus (0xff, 0x00, stale
 * values) means absent. Status alone is NOT reliable — real drives can
 * idle at stat=0x00 (ICH8M PATA SuperDrive does). */
static int ide_identify_device(const IdeChannelPorts *p, int device, IdeDeviceInfo *info) {
    ide_select_device(p, device);
    wait_bsy_clear(p, 1000);

    uint8_t sc   = inb(p->seccount_port);
    uint8_t lba0 = inb(p->lba0_port);
    uint8_t lba1 = inb(p->lba1_port);
    uint8_t lba2 = inb(p->lba2_port);
    int is_atapi = (sc == 0x01 && lba0 == 0x01 && lba1 == 0x14 && lba2 == 0xEB);
    int is_ata   = (sc == 0x01 && lba0 == 0x01 && lba1 == 0x00 && lba2 == 0x00);
    if (!is_atapi && !is_ata) { info->present = 0; return -1; }
    if (inb(p->cmd_stat_port) == 0xFF) { info->present = 0; return -1; }

    outb(p->seccount_port, 0); outb(p->lba0_port, 0); outb(p->lba1_port, 0); outb(p->lba2_port, 0);
    outb(p->cmd_stat_port, is_atapi ? ATAPI_CMD_IDENTIFY : ATA_CMD_IDENTIFY);
    io_wait(); io_wait();
    if (wait_bsy_clear(p, 5000) != 0) { info->present = 0; return -1; }
    if (inb(p->cmd_stat_port) & ATA_SR_ERR) { info->present = 0; return -1; }
    if (wait_drq_or_err(p, 5000) <= 0) { info->present = 0; return -1; }
    uint16_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = inw(p->data_port);
    (void)inb(p->cmd_stat_port);
    info->present = 1;
    if (is_atapi) atapi_parse_identify(buf, info);
    else { info->type = IDE_DEV_ATA; ata_parse_identify(buf, info); }
    return 0;
}

/* ATA PIO Read LBA28 */
static int ata_read_pio(const IdeChannelPorts *p, int device, uint32_t lba, uint8_t count, void *buffer) {
    uint16_t *buf16 = (uint16_t *)buffer;
    ide_select_device(p, device);
    if (wait_bsy_clear(p, 1000) != 0) return -1;
    outb(p->seccount_port, count);
    outb(p->lba0_port, (uint8_t)(lba & 0xFF));
    outb(p->lba1_port, (uint8_t)((lba >> 8) & 0xFF));
    outb(p->lba2_port, (uint8_t)((lba >> 16) & 0xFF));
    outb(p->devsel_port, 0xE0 | (device ? 0x10 : 0) | ((lba >> 24) & 0x0F));
    outb(p->cmd_stat_port, ATA_CMD_READ_SECTORS);
    io_wait(); io_wait();
    for (int s = 0; s < count; s++) {
        if (wait_drq_or_err(p, 5000) <= 0) return -1;
        for (int i = 0; i < 256; i++) buf16[s * 256 + i] = inw(p->data_port);
        if (s < count - 1) { if (wait_bsy_clear(p, 5000) != 0) return -1; }
    }
    return 0;
}

/* ATA PIO Write LBA28 */
static int ata_write_pio(const IdeChannelPorts *p, int device, uint32_t lba, uint8_t count, const void *buffer) {
    const uint16_t *buf16 = (const uint16_t *)buffer;
    ide_select_device(p, device);
    if (wait_bsy_clear(p, 1000) != 0) return -1;
    outb(p->seccount_port, count);
    outb(p->lba0_port, (uint8_t)(lba & 0xFF));
    outb(p->lba1_port, (uint8_t)((lba >> 8) & 0xFF));
    outb(p->lba2_port, (uint8_t)((lba >> 16) & 0xFF));
    outb(p->devsel_port, 0xE0 | (device ? 0x10 : 0) | ((lba >> 24) & 0x0F));
    outb(p->cmd_stat_port, ATA_CMD_WRITE_SECTORS);
    io_wait(); io_wait();
    for (int s = 0; s < count; s++) {
        if (wait_drq_or_err(p, 5000) <= 0) return -1;
        for (int i = 0; i < 256; i++) outw(p->data_port, buf16[s * 256 + i]);
        if (s < count - 1) { if (wait_bsy_clear(p, 5000) != 0) return -1; }
    }
    outb(p->cmd_stat_port, ATA_CMD_FLUSH_CACHE);
    wait_bsy_clear(p, 5000);
    return 0;
}

/* ATAPI PACKET command */
static int atapi_packet_cmd(const IdeChannelPorts *p, int device, const uint8_t *packet, int packet_len,
                             void *data_buffer, uint32_t data_len, int is_read) {
    ide_select_device(p, device);
    if (wait_bsy_clear(p, 1000) != 0) return -1;
    outb(p->err_feat_port, 0);
    outb(p->lba1_port, (uint8_t)(data_len & 0xFF));
    outb(p->lba2_port, (uint8_t)((data_len >> 8) & 0xFF));
    outb(p->cmd_stat_port, ATAPI_CMD_PACKET);
    io_wait(); io_wait();
    if (wait_drq_or_err(p, 5000) <= 0) return -1;
    const uint16_t *pkt16 = (const uint16_t *)packet;
    for (int i = 0; i < (packet_len / 2); i++) outw(p->data_port, pkt16[i]);
    if (packet_len & 1) outw(p->data_port, (uint16_t)packet[packet_len - 1]);
    if (data_len == 0) { wait_bsy_clear(p, 5000); return 0; }
    uint8_t *data8 = (uint8_t *)data_buffer;
    uint32_t done = 0;
    while (done < data_len) {
        int r = wait_drq_or_err(p, 10000);
        if (r < 0) return -1;
        if (r == 0) break;
        uint16_t xfer = inb(p->lba1_port) | ((uint16_t)inb(p->lba2_port) << 8);
        if (xfer == 0) break;
        if (xfer > data_len - done) xfer = (uint16_t)(data_len - done);
        if (is_read) {
            for (uint16_t i = 0; i < (xfer / 2); i++) {
                uint16_t w = inw(p->data_port);
                if (done + 1 < data_len) { data8[done] = (uint8_t)(w & 0xFF); data8[done + 1] = (uint8_t)(w >> 8); }
                else if (done < data_len) data8[done] = (uint8_t)(w & 0xFF);
                done += 2;
            }
        } else {
            for (uint16_t i = 0; i < (xfer / 2); i++) {
                uint16_t w = data8[done] | ((uint16_t)data8[done + 1] << 8);
                outw(p->data_port, w);
                done += 2;
            }
        }
    }
    wait_bsy_clear(p, 5000);
    return 0;
}

/* ATAPI READ(10) */
static int atapi_read_sectors(const IdeChannelPorts *p, int device, uint32_t lba, uint16_t count, void *buffer) {
    uint8_t packet[12] = {0};
    packet[0] = SCSI_READ_10;
    packet[2] = (uint8_t)((lba >> 24) & 0xFF);
    packet[3] = (uint8_t)((lba >> 16) & 0xFF);
    packet[4] = (uint8_t)((lba >> 8) & 0xFF);
    packet[5] = (uint8_t)(lba & 0xFF);
    packet[7] = (uint8_t)((count >> 8) & 0xFF);
    packet[8] = (uint8_t)(count & 0xFF);
    return atapi_packet_cmd(p, device, packet, 12, buffer, count * 2048, 1);
}

/* ATAPI READ CAPACITY(10) */
static int atapi_read_capacity(const IdeChannelPorts *p, int device, uint32_t *out_lba, uint32_t *out_block) {
    uint8_t packet[12] = {0}; packet[0] = SCSI_READ_CAPACITY_10;
    uint8_t resp[8] = {0};
    int ret = atapi_packet_cmd(p, device, packet, 12, resp, 8, 1);
    if (ret != 0) return ret;
    *out_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) | ((uint32_t)resp[2] << 8) | resp[3];
    *out_block = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) | ((uint32_t)resp[6] << 8) | resp[7];
    return 0;
}

/* ATAPI TEST UNIT READY */
static int atapi_test_unit_ready(const IdeChannelPorts *p, int device) {
    uint8_t packet[12] = {0}; packet[0] = SCSI_TEST_UNIT_READY;
    return atapi_packet_cmd(p, device, packet, 12, NULL, 0, 0);
}

/* Setup compatibility ports */
static void setup_compat_ports(int ch) {
    if (ch == 0) {
        g_channels[0].ports = (IdeChannelPorts){ IDE_PRI_DATA, IDE_PRI_ERR_FEAT, IDE_PRI_SECCOUNT,
            IDE_PRI_LBA0, IDE_PRI_LBA1, IDE_PRI_LBA2, IDE_PRI_DEVSEL, IDE_PRI_CMD_STAT, IDE_PRI_CTL_ALT };
        g_channels[0].irq_line = 14;
    } else {
        g_channels[1].ports = (IdeChannelPorts){ IDE_SEC_DATA, IDE_SEC_ERR_FEAT, IDE_SEC_SECCOUNT,
            IDE_SEC_LBA0, IDE_SEC_LBA1, IDE_SEC_LBA2, IDE_SEC_DEVSEL, IDE_SEC_CMD_STAT, IDE_SEC_CTL_ALT };
        g_channels[1].irq_line = 15;
    }
}

/* Setup PCI-native ports: cmd_base is the channel command block (BAR & ~3),
 * ctl_base the control block; the alt status/device control port is ctl_base+2.
 * Native channels interrupt on the PCI INTx line, not IRQ14/15. */
static void setup_native_ports(int ch, uint16_t cmd_base, uint16_t ctl_base, int irq) {
    IdeChannelPorts *p = &g_channels[ch].ports;
    p->data_port     = cmd_base;
    p->err_feat_port = cmd_base + 1;
    p->seccount_port = cmd_base + 2;
    p->lba0_port     = cmd_base + 3;
    p->lba1_port     = cmd_base + 4;
    p->lba2_port     = cmd_base + 5;
    p->devsel_port   = cmd_base + 6;
    p->cmd_stat_port = cmd_base + 7;
    p->ctl_alt_port  = ctl_base + 2;
    g_channels[ch].irq_line = irq;
}

/* Decode a PCI I/O BAR into a port base; 0 if unusable (memory BAR,
 * unassigned, or above the 16-bit I/O space). */
static uint16_t ide_io_bar(uint32_t bar) {
    if (!(bar & 1)) return 0;
    uint32_t base = bar & ~3u;
    return (base > 0xFFFF) ? 0 : (uint16_t)base;
}

/* Configure both channels of a PCI IDE controller (class 01/01).
 * Prog-IF bit0/bit2 select native vs compatibility mode per channel. */
static void setup_pci_controller(uint8_t bus, uint8_t dev, uint8_t func, uint8_t pif) {
    uint32_t bar0 = pci_config_read_dword(bus, dev, func, 0x10);
    uint32_t bar1 = pci_config_read_dword(bus, dev, func, 0x14);
    uint32_t bar2 = pci_config_read_dword(bus, dev, func, 0x18);
    uint32_t bar3 = pci_config_read_dword(bus, dev, func, 0x1C);

    /* Wake to D0 via the PM capability — firmware may have parked an
     * unused optical controller in D3hot, where the I/O BARs don't
     * decode and every port read floats to 0xFF. */
    if (pci_config_read_word(bus, dev, func, 0x06) & 0x10) {
        uint8_t cap = (uint8_t)(pci_config_read_byte(bus, dev, func, 0x34) & ~3u);
        for (int i = 0; i < 48 && cap; i++) {
            if (pci_config_read_byte(bus, dev, func, cap) == 0x01) {
                uint16_t pmcsr = pci_config_read_word(bus, dev, func, cap + 4);
                if (pmcsr & 3) {
                    pci_config_write_word(bus, dev, func, cap + 4, (uint16_t)(pmcsr & ~3u));
                    (void)pci_config_read_word(bus, dev, func, cap + 4);
                    ide_ms_spin(10);
                }
                break;
            }
            cap = (uint8_t)(pci_config_read_byte(bus, dev, func, cap + 1) & ~3u);
        }
    }

    /* Enable I/O space decoding + bus mastering */
    uint16_t cmd = pci_config_read_word(bus, dev, func, PCI_REG_COMMAND);
    pci_config_write_word(bus, dev, func, PCI_REG_COMMAND,
                          (uint16_t)(cmd | PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER));
    (void)pci_config_read_word(bus, dev, func, PCI_REG_COMMAND);

    int intline = pci_config_read_byte(bus, dev, func, PCI_REG_INTERRUPT_LINE);
    if (intline == 0xFF) intline = -1;

    g_pci_bus = bus; g_pci_dev = dev; g_pci_fn = func;
    g_pci_pif = pif; g_pci_intline = intline;

    kprint("[IDE] PCI IDE controller at ");
    kprinthex((uint64_t)bus); kprint(":");
    kprinthex((uint64_t)dev); kprint(".");
    kprinthex((uint64_t)func);
    kprint(" progif="); kprinthex((uint64_t)pif); kprint("\n");

    uint16_t pcmd = ide_io_bar(bar0), pctl = ide_io_bar(bar1);
    uint16_t scmd = ide_io_bar(bar2), sctl = ide_io_bar(bar3);
    if ((pif & 0x01) && pcmd && pctl) {
        setup_native_ports(0, pcmd, pctl, intline);
        kprint("[IDE]   ch0 native cmd="); kprinthex((uint64_t)pcmd);
        kprint(" ctl="); kprinthex((uint64_t)(pctl + 2)); kprint("\n");
    } else setup_compat_ports(0);
    if ((pif & 0x04) && scmd && sctl) {
        setup_native_ports(1, scmd, sctl, intline);
        kprint("[IDE]   ch1 native cmd="); kprinthex((uint64_t)scmd);
        kprint(" ctl="); kprinthex((uint64_t)(sctl + 2)); kprint("\n");
    } else setup_compat_ports(1);
}

int IDE_Init(void) {
    kprint("[IDE] Initialising IDE controller...\n");
    for (int ch = 0; ch < 2; ch++) {
        g_channels[ch].present = 0; g_channels[ch].irq_line = -1;
        for (int dev = 0; dev < 2; dev++) {
            g_devices[ch][dev].type = IDE_DEV_NONE; g_devices[ch][dev].present = 0;
            g_devices[ch][dev].num_sectors = 0; g_devices[ch][dev].sector_size = 0;
            g_devices[ch][dev].model[0] = '\0';
        }
    }
    g_num_channels = 0;

    /* PCI scan: all buses, all functions (same walker as C:pciscan).
     * Honour the header-type multifunction bit so e.g. ICH8M PATA at
     * 0:31.1 is found even though function 0 is the LPC bridge. */
    int pci_found = 0;
    for (int bus = 0; bus < 256 && !pci_found; bus++) {
        for (int dev = 0; dev < 32 && !pci_found; dev++) {
            for (int func = 0; func < 8 && !pci_found; func++) {
                uint16_t vendor = pci_config_read_word(bus, dev, func, 0);
                if (vendor == 0xFFFF || vendor == 0) { if (!func) break; continue; }
                uint8_t cls = pci_config_read_byte(bus, dev, func, 0x0B);
                uint8_t sub = pci_config_read_byte(bus, dev, func, 0x0A);
                uint8_t pif = pci_config_read_byte(bus, dev, func, 0x09);
                if (cls == 0x01 && sub == 0x01) {
                    setup_pci_controller((uint8_t)bus, (uint8_t)dev, (uint8_t)func, pif);
                    pci_found = 1;
                }
                if (func == 0 &&
                    !(pci_config_read_byte(bus, dev, 0, PCI_REG_HEADER_TYPE) & PCI_HEADER_MULTIFUNC))
                    break;
            }
        }
    }
    if (!pci_found) {
        kprint("[IDE] No PCI IDE controller, using compatibility mode\n");
        setup_compat_ports(0); setup_compat_ports(1);
    }

    for (int ch = 0; ch < 2; ch++) {
        const IdeChannelPorts *p = &g_channels[ch].ports;
        kprint("[IDE] Probing channel "); kprinthex((uint64_t)ch); kprint("...\n");
        int any = 0;
        /* Second pass after a grace delay: real ATAPI drives can still be
         * running post-reset diagnostics when the first IDENTIFY lands. */
        for (int attempt = 0; attempt < 2 && !any; attempt++) {
            ide_soft_reset(p);
            for (int dev = 0; dev < 2; dev++) {
            if (ide_identify_device(p, dev, &g_devices[ch][dev]) == 0 && g_devices[ch][dev].present) {
                any = 1;
                kprint("[IDE]   dev "); kprinthex((uint64_t)dev); kprint(" ");
                kprint(g_devices[ch][dev].type == IDE_DEV_ATAPI ? "ATAPI" : "ATA");
                kprint(": "); kprint(g_devices[ch][dev].model);
                kprint("\n");
                if (g_devices[ch][dev].type == IDE_DEV_ATAPI) {
                    uint32_t cap_lba = 0, cap_blk = 0;
                    atapi_test_unit_ready(p, dev);
                    if (atapi_read_capacity(p, dev, &cap_lba, &cap_blk) == 0) {
                        g_devices[ch][dev].num_sectors = cap_lba + 1;
                        g_devices[ch][dev].sector_size = cap_blk;
                    } else {
                        g_devices[ch][dev].num_sectors = 0;
                        g_devices[ch][dev].sector_size = 2048;
                    }
                }
            }
            }
        }
        g_channels[ch].present = any;
        if (any) g_num_channels++;
    }
    kprint("[IDE] Init complete, "); kprinthex((uint64_t)g_num_channels); kprint(" channel(s)\n");
    return g_num_channels;
}

/* Public wrappers */
int IDE_ATA_ReadSectors(int channel, int device, uint64_t sector, uint32_t count, void *buffer) {
    if (channel < 0 || channel >= 2 || device < 0 || device >= 2) return -1;
    if (!g_devices[channel][device].present || g_devices[channel][device].type != IDE_DEV_ATA) return -1;
    const IdeChannelPorts *p = &g_channels[channel].ports;
    uint8_t *buf8 = (uint8_t *)buffer;
    uint64_t cur = sector;
    uint32_t rem = count;
    while (rem > 0) {
        uint8_t n = (uint8_t)(rem > 255 ? 255 : rem);
        uint32_t lba28 = (uint32_t)(cur & 0x0FFFFFFF);
        if (ata_read_pio(p, device, lba28, n, buf8) != 0) return -1;
        buf8 += n * 512;
        cur += n;
        rem -= n;
    }
    return 0;
}

int IDE_ATA_WriteSectors(int channel, int device, uint64_t sector, uint32_t count, const void *buffer) {
    if (channel < 0 || channel >= 2 || device < 0 || device >= 2) return -1;
    if (!g_devices[channel][device].present || g_devices[channel][device].type != IDE_DEV_ATA) return -1;
    const IdeChannelPorts *p = &g_channels[channel].ports;
    const uint8_t *buf8 = (const uint8_t *)buffer;
    uint64_t cur = sector;
    uint32_t rem = count;
    while (rem > 0) {
        uint8_t n = (uint8_t)(rem > 255 ? 255 : rem);
        uint32_t lba28 = (uint32_t)(cur & 0x0FFFFFFF);
        if (ata_write_pio(p, device, lba28, n, buf8) != 0) return -1;
        buf8 += n * 512;
        cur += n;
        rem -= n;
    }
    return 0;
}

int IDE_ATAPI_ReadSectors(int channel, int device, uint64_t sector, uint32_t count, void *buffer) {
    if (channel < 0 || channel >= 2 || device < 0 || device >= 2) return -1;
    if (!g_devices[channel][device].present || g_devices[channel][device].type != IDE_DEV_ATAPI) return -1;
    const IdeChannelPorts *p = &g_channels[channel].ports;
    uint8_t *buf8 = (uint8_t *)buffer;
    uint64_t cur = sector;
    uint32_t rem = count;
    while (rem > 0) {
        uint16_t n = (uint16_t)(rem > 255 ? 255 : rem);
        if (atapi_read_sectors(p, device, (uint32_t)cur, n, buf8) != 0) return -1;
        buf8 += n * 2048;
        cur += n;
        rem -= n;
    }
    return 0;
}

int IDE_GetCapacity(int channel, int device, uint64_t *out_sectors, uint32_t *out_sector_size) {
    if (channel < 0 || channel >= 2 || device < 0 || device >= 2) return -1;
    if (!g_devices[channel][device].present) return -1;
    *out_sectors = g_devices[channel][device].num_sectors;
    *out_sector_size = g_devices[channel][device].sector_size;
    return 0;
}

const IdeDeviceInfo *IDE_GetDeviceInfo(int channel, int device) {
    if (channel < 0 || channel >= 2 || device < 0 || device >= 2) return NULL;
    return g_devices[channel][device].present ? &g_devices[channel][device] : NULL;
}

int IDE_GetChannelCount(void) { return g_num_channels; }

int IDE_GetDeviceCount(int channel) {
    if (channel < 0 || channel >= 2) return 0;
    int n = 0;
    for (int dev = 0; dev < 2; dev++) if (g_devices[channel][dev].present) n++;
    return n;
}

/* =========================================================================
 * BlockDev integration
 * ========================================================================= */

typedef struct {
    int channel;
    int device;
} IdeBlockDevPrivate;

static IdeBlockDevPrivate g_ide_priv[2][2];
static BlockDevOps g_ide_ops[2][2];
static BlockDev g_ide_bdev[2][2];

static int ide_bdev_read(BlockDev *bdev, uint64_t sector, void *buffer, uint32_t num_sectors) {
    IdeBlockDevPrivate *priv = (IdeBlockDevPrivate *)bdev->private_data;
    int ch = priv->channel;
    int dev = priv->device;
    if (g_devices[ch][dev].type == IDE_DEV_ATAPI)
        return IDE_ATAPI_ReadSectors(ch, dev, sector, num_sectors, buffer);
    else
        return IDE_ATA_ReadSectors(ch, dev, sector, num_sectors, buffer);
}

static int ide_bdev_write(BlockDev *bdev, uint64_t sector, const void *buffer, uint32_t num_sectors) {
    IdeBlockDevPrivate *priv = (IdeBlockDevPrivate *)bdev->private_data;
    int ch = priv->channel;
    int dev = priv->device;
    return IDE_ATA_WriteSectors(ch, dev, sector, num_sectors, buffer);
}

static uint64_t ide_bdev_capacity(BlockDev *bdev) {
    IdeBlockDevPrivate *priv = (IdeBlockDevPrivate *)bdev->private_data;
    int ch = priv->channel;
    int dev = priv->device;
    return g_devices[ch][dev].num_sectors;
}

void IDE_RegisterBlockDevs(void) {
    for (int ch = 0; ch < 2; ch++) {
        for (int dev = 0; dev < 2; dev++) {
            if (!g_devices[ch][dev].present) continue;

            g_ide_priv[ch][dev].channel = ch;
            g_ide_priv[ch][dev].device = dev;

            g_ide_ops[ch][dev].read = ide_bdev_read;
            g_ide_ops[ch][dev].write = ide_bdev_write;
            g_ide_ops[ch][dev].get_capacity = ide_bdev_capacity;

            char name[16];
            char dname[16];
            if (g_devices[ch][dev].type == IDE_DEV_ATAPI) {
                /* ATAPI CD-ROM: atapi0, atapi1, ... */
                int idx = ch * 2 + dev;
                name[0] = 'a'; name[1] = 't'; name[2] = 'a'; name[3] = 'p'; name[4] = 'i';
                name[5] = '0' + idx; name[6] = '\0';
                dname[0] = 'C'; dname[1] = 'D'; dname[2] = '0' + idx; dname[3] = ':'; dname[4] = '\0';
            } else {
                /* ATA hard disk: ata0, ata1, ... */
                int idx = ch * 2 + dev;
                name[0] = 'a'; name[1] = 't'; name[2] = 'a';
                name[3] = '0' + idx; name[4] = '\0';
                dname[0] = 'D'; dname[1] = 'H'; dname[2] = '0' + idx; dname[3] = ':'; dname[4] = '\0';
            }

            /* Store names in static arrays so pointers remain valid */
            static char name_storage[4][16];
            static char dname_storage[4][16];
            int slot = ch * 2 + dev;
            for (int i = 0; i < 16; i++) {
                name_storage[slot][i] = name[i];
                dname_storage[slot][i] = dname[i];
            }

            g_ide_bdev[ch][dev].name = name_storage[slot];
            g_ide_bdev[ch][dev].display_name = dname_storage[slot];
            g_ide_bdev[ch][dev].sector_size = g_devices[ch][dev].sector_size;
            g_ide_bdev[ch][dev].num_sectors = g_devices[ch][dev].num_sectors;
            g_ide_bdev[ch][dev].part_offset = 0;
            g_ide_bdev[ch][dev].formatted = 0;
            g_ide_bdev[ch][dev].private_data = &g_ide_priv[ch][dev];
            g_ide_bdev[ch][dev].ops = &g_ide_ops[ch][dev];
            g_ide_bdev[ch][dev].next = NULL;

            BlockDev_Register(&g_ide_bdev[ch][dev]);
            kprint("[IDE] Registered blockdev "); kprint(name); kprint(" ("); kprint(dname); kprint(")\n");
        }
    }
}

/* -------------------------------------------------------------------------
 * C:diskdiag — IDE/ATAPI stage dump (UAOS-204)
 * ------------------------------------------------------------------------- */
#include "../dbg/diag.h"

static void ide_dump_regs(void *ctx, void (*emit)(void *, const char *),
                          const char *tag, const IdeChannelPorts *p)
{
    DiagLine l;
    dl_reset(&l);
    dl_add(&l, tag);
    dl_add(&l, " stat="); dl_hex(&l, inb(p->cmd_stat_port));
    dl_add(&l, " alt=");  dl_hex(&l, inb(p->ctl_alt_port));
    dl_add(&l, " err=");  dl_hex(&l, inb(p->err_feat_port));
    dl_add(&l, " sc=");   dl_hex(&l, inb(p->seccount_port));
    dl_add(&l, " lba=");  dl_hex(&l, inb(p->lba0_port));
    dl_ch(&l, '/');       dl_hex(&l, inb(p->lba1_port));
    dl_ch(&l, '/');       dl_hex(&l, inb(p->lba2_port));
    dl_add(&l, " dsel="); dl_hex(&l, inb(p->devsel_port));
    dl_emit(&l, ctx, emit);
}

void IDE_DiagDump(void *ctx, void (*emit)(void *, const char *))
{
    DiagLine l;

    dl_reset(&l);
    if (g_pci_bus >= 0) {
        dl_add(&l, " ide pci=");
        dl_dec(&l, (uint64_t)g_pci_bus); dl_ch(&l, ':');
        dl_dec(&l, (uint64_t)g_pci_dev); dl_ch(&l, '.');
        dl_dec(&l, (uint64_t)g_pci_fn);
        dl_add(&l, " progif="); dl_hex(&l, (uint64_t)g_pci_pif);
        dl_add(&l, " cmd="); dl_hex(&l, pci_config_read_word(
            (uint8_t)g_pci_bus, (uint8_t)g_pci_dev, (uint8_t)g_pci_fn, PCI_REG_COMMAND));
        dl_add(&l, " intline="); dl_sdec(&l, g_pci_intline);
    } else {
        dl_add(&l, " ide: no pci controller, compat ports");
    }
    dl_emit(&l, ctx, emit);

    for (int c = 0; c < 2; c++) {
        IdeChannel *ch = &g_channels[c];
        const IdeChannelPorts *p = &ch->ports;

        dl_reset(&l);
        dl_add(&l, " ide ch"); dl_dec(&l, (uint64_t)c);
        dl_add(&l, " io="); dl_hex(&l, p->data_port);
        dl_add(&l, " ctl="); dl_hex(&l, p->ctl_alt_port);
        dl_add(&l, " irq="); dl_sdec(&l, ch->irq_line);
        dl_add(&l, ch->present ? " [present]" : " [empty]");
        dl_emit(&l, ctx, emit);
        ide_dump_regs(ctx, emit, "   ", p);

        /* Empty at boot — live SRST re-probe so diskdiag answers "is the
         * drive there at all?". Signature after SRST should settle to
         * sc=0x01 lba=0x01/0x14/0xeb for ATAPI (0x01/0x00/0x00 for ATA). */
        if (!ch->present) {
            ide_soft_reset(p);
            ide_ms_spin(1000);
            ide_dump_regs(ctx, emit, "   after-srst:", p);
        }

        for (int d = 0; d < 2; d++) {
            IdeDeviceInfo *di = &g_devices[c][d];
            if (!di->present) continue;
            dl_reset(&l);
            dl_add(&l, "   dev"); dl_dec(&l, (uint64_t)d);
            dl_add(&l, di->type == IDE_DEV_ATAPI ? " atapi " : " ata   ");
            dl_add(&l, di->model);
            dl_add(&l, "  sectors="); dl_dec(&l, di->num_sectors);
            dl_add(&l, " sec_sz="); dl_dec(&l, di->sector_size);
            if (di->lba48) dl_add(&l, " lba48");
            dl_emit(&l, ctx, emit);
        }
    }
}
