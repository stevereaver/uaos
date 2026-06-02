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
    return (uint8_t)(dw >> ((offset & 2) * 8));
}

/* Globals */
static IdeChannel g_channels[2];
static IdeDeviceInfo g_devices[2][2];
static int g_num_channels = 0;

/* Wait helpers */
static int wait_bsy_clear(const IdeChannelPorts *p, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 100; i++) {
        uint8_t s = inb(p->cmd_stat_port);
        if (s == 0xFF) return -1; /* No device present (floating bus) */
        if (!(s & ATA_SR_BSY)) return 0;
        io_wait();
    }
    return -1;
}
static int wait_drq_or_err(const IdeChannelPorts *p, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 100; i++) {
        uint8_t s = inb(p->cmd_stat_port);
        if (s == 0xFF) return -1;
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DRQ) return 1;
        if (!(s & ATA_SR_BSY)) return 0;
        io_wait();
    }
    return -2;
}

/* Software reset */
static void ide_soft_reset(const IdeChannelPorts *p) {
    outb(p->ctl_alt_port, 0x04); io_wait(); io_wait(); io_wait(); io_wait();
    outb(p->ctl_alt_port, 0x00); io_wait(); io_wait(); io_wait(); io_wait();
    wait_bsy_clear(p, 2000);
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

/* Send IDENTIFY to a device */
static int ide_identify_device(const IdeChannelPorts *p, int device, IdeDeviceInfo *info) {
    ide_select_device(p, device);
    wait_bsy_clear(p, 1000);
    outb(p->seccount_port, 0); outb(p->lba0_port, 0); outb(p->lba1_port, 0); outb(p->lba2_port, 0);
    outb(p->cmd_stat_port, ATA_CMD_IDENTIFY);
    io_wait(); io_wait();
    uint8_t status = inb(p->cmd_stat_port);
    if (status == 0) { info->present = 0; return -1; }
    uint8_t lba1 = inb(p->lba1_port);
    uint8_t lba2 = inb(p->lba2_port);
    int is_atapi = (lba1 == 0x14 && lba2 == 0xEB) ? 1 : 0;
    if (wait_bsy_clear(p, 5000) != 0) { info->present = 0; return -1; }
    status = inb(p->cmd_stat_port);
    if (status & ATA_SR_ERR) {
        if (is_atapi) {
            outb(p->cmd_stat_port, ATAPI_CMD_IDENTIFY);
            io_wait(); io_wait();
            if (wait_bsy_clear(p, 5000) != 0) { info->present = 0; return -1; }
        } else { info->present = 0; return -1; }
    }
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

    int pci_found = 0;
    for (int bus = 0; bus < 4 && !pci_found; bus++) {
        for (int dev = 0; dev < 32 && !pci_found; dev++) {
            uint16_t vendor = pci_config_read_word(bus, dev, 0, 0);
            if (vendor == 0xFFFF) continue;
            uint8_t cls = pci_config_read_byte(bus, dev, 0, 0x0B);
            uint8_t sub = pci_config_read_byte(bus, dev, 0, 0x0A);
            uint8_t pif = pci_config_read_byte(bus, dev, 0, 0x09);
            if (cls == 0x01 && sub == 0x01) {
                uint32_t bar0 = pci_config_read_dword(bus, dev, 0, 0x10);
                uint32_t bar1 = pci_config_read_dword(bus, dev, 0, 0x14);
                uint32_t bar2 = pci_config_read_dword(bus, dev, 0, 0x18);
                uint32_t bar3 = pci_config_read_dword(bus, dev, 0, 0x1C);
                (void)bar3; (void)bar0; (void)bar1; (void)bar2;
                if (pif & 0x01) {
                    /* native mode primary - simplified: use compatibility */
                    setup_compat_ports(0);
                } else setup_compat_ports(0);
                if (pif & 0x04) {
                    /* native mode secondary - simplified: use compatibility */
                    setup_compat_ports(1);
                } else setup_compat_ports(1);
                pci_found = 1;
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
        ide_soft_reset(p);
        int any = 0;
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
