/* ide.h — UAOS IDE/ATA + ATAPI Block Device Driver
 *
 * Full IDE/ATA + ATAPI support for primary and secondary channels.
 * Detects ATA hard disks and ATAPI CD-ROMs via PCI or compatibility-mode ports.
 */

#ifndef UAOS_IDE_H
#define UAOS_IDE_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * IDE I/O port definitions
 * ------------------------------------------------------------------------- */
#define IDE_PRI_DATA       0x1F0  /* Primary data register */
#define IDE_PRI_ERR_FEAT   0x1F1  /* Error / Features */
#define IDE_PRI_SECCOUNT   0x1F2  /* Sector count */
#define IDE_PRI_LBA0       0x1F3  /* LBA low */
#define IDE_PRI_LBA1       0x1F4  /* LBA mid */
#define IDE_PRI_LBA2       0x1F5  /* LBA high */
#define IDE_PRI_DEVSEL     0x1F6  /* Device / Head */
#define IDE_PRI_CMD_STAT   0x1F7  /* Command / Status */
#define IDE_PRI_CTL_ALT    0x3F6  /* Control / Alternate status */

#define IDE_SEC_DATA       0x170  /* Secondary data register */
#define IDE_SEC_ERR_FEAT   0x171
#define IDE_SEC_SECCOUNT   0x172
#define IDE_SEC_LBA0       0x173
#define IDE_SEC_LBA1       0x174
#define IDE_SEC_LBA2       0x175
#define IDE_SEC_DEVSEL     0x176
#define IDE_SEC_CMD_STAT   0x177
#define IDE_SEC_CTL_ALT    0x376

/* -------------------------------------------------------------------------
 * ATA commands
 * ------------------------------------------------------------------------- */
#define ATA_CMD_READ_SECTORS     0x20
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS    0x30
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_FLUSH_CACHE      0xE7
#define ATA_CMD_SET_FEATURES     0xEF

/* -------------------------------------------------------------------------
 * ATAPI commands
 * ------------------------------------------------------------------------- */
#define ATAPI_CMD_PACKET         0xA0
#define ATAPI_CMD_IDENTIFY       0xA1

/* SCSI/ATAPI packet commands */
#define SCSI_TEST_UNIT_READY     0x00
#define SCSI_INQUIRY             0x12
#define SCSI_READ_CAPACITY_10  0x25
#define SCSI_READ_10             0x28
#define SCSI_READ_12             0xA8
#define SCSI_START_STOP_UNIT     0x1B

/* -------------------------------------------------------------------------
 * Status register bits
 * ------------------------------------------------------------------------- */
#define ATA_SR_BSY  0x80  /* Busy */
#define ATA_SR_DRDY 0x40  /* Drive ready */
#define ATA_SR_DRQ  0x08  /* Data request ready */
#define ATA_SR_ERR  0x01  /* Error */

/* -------------------------------------------------------------------------
 * Error register bits
 * ------------------------------------------------------------------------- */
#define ATA_ER_BBK  0x80  /* Bad block */
#define ATA_ER_UNC  0x40  /* Uncorrectable */
#define ATA_ER_MC   0x20  /* Media change */
#define ATA_ER_IDNF 0x10  /* ID mark not found */
#define ATA_ER_MCR  0x08  /* Media change request */
#define ATA_ER_ABRT 0x04  /* Command aborted */
#define ATA_ER_TK0NF 0x02 /* Track 0 not found */
#define ATA_ER_AMNF 0x01  /* No address mark */

/* -------------------------------------------------------------------------
 * IDE channel and device types
 * ------------------------------------------------------------------------- */
typedef enum {
    IDE_DEV_NONE = 0,
    IDE_DEV_ATA,
    IDE_DEV_ATAPI
} IdeDevType;

typedef struct {
    uint16_t data_port;
    uint16_t err_feat_port;
    uint16_t seccount_port;
    uint16_t lba0_port;
    uint16_t lba1_port;
    uint16_t lba2_port;
    uint16_t devsel_port;
    uint16_t cmd_stat_port;
    uint16_t ctl_alt_port;
} IdeChannelPorts;

typedef struct {
    IdeChannelPorts ports;
    int present;           /* 1 = channel has at least one device */
    int irq_line;          /* IRQ14 for primary, IRQ15 for secondary (or -1 for polling) */
} IdeChannel;

typedef struct {
    IdeDevType type;
    int present;           /* 1 = device responded to IDENTIFY */
    int lba48;             /* 1 = supports LBA48 addressing */
    uint64_t num_sectors;  /* Total sectors */
    uint32_t sector_size;  /* 512 for ATA, 2048 for ATAPI */
    char model[41];        /* Null-terminated model string */
    int removable;         /* 1 = removable media (CD-ROM etc) */
    int atapi_capable;     /* 1 = ATAPI PACKET capable */
} IdeDeviceInfo;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Initialise IDE controller(s): PCI scan + compatibility fallback.
 * Returns number of channels found (0-2). */
int IDE_Init(void);

/* Read sectors from an ATA device.  sector is LBA, count in sectors.
 * Returns 0 on success, negative on error. */
int IDE_ATA_ReadSectors(int channel, int device, uint64_t sector, uint32_t count, void *buffer);

/* Write sectors to an ATA device. */
int IDE_ATA_WriteSectors(int channel, int device, uint64_t sector, uint32_t count, const void *buffer);

/* Read sectors from an ATAPI device (CD-ROM).
 * sector is 2048-byte logical block, count in blocks.
 * Returns 0 on success, negative on error. */
int IDE_ATAPI_ReadSectors(int channel, int device, uint64_t sector, uint32_t count, void *buffer);

/* Get capacity of a device (sectors, sector_size). */
int IDE_GetCapacity(int channel, int device, uint64_t *out_sectors, uint32_t *out_sector_size);

/* Get device info. */
const IdeDeviceInfo *IDE_GetDeviceInfo(int channel, int device);

/* Number of channels found after init. */
int IDE_GetChannelCount(void);

/* Number of present devices on a channel. */
int IDE_GetDeviceCount(int channel);

/* Register all detected IDE/ATAPI devices with the BlockDev layer */
void IDE_RegisterBlockDevs(void);

#endif /* UAOS_IDE_H */
