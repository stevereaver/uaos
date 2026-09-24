/*
 * virtio_scsi.h — UAOS VirtIO-SCSI Block Device Driver Header
 *
 * Supports both the modern (non-transitional) virtio 1.0+ transport used by
 * VirtualBox (PCI 1af4:1048) and the legacy virtio-scsi transport used by
 * older QEMU (PCI 1af4:1004).
 *
 * Scans SCSI targets 0-1 on the controller.  Hard disks (INQUIRY type 0)
 * are registered as "virtio0" with 512-byte sectors.  CD/DVD-ROMs (INQUIRY
 * type 5) are registered as "vio_cd0" with 2048-byte sectors so the boot
 * code can mount Workbench: from them via ISO 9660.
 */

#ifndef UAOS_VIRTIO_SCSI_H
#define UAOS_VIRTIO_SCSI_H

#include <stdint.h>

/* Initialize VirtIO-SCSI driver.  Scans the PCI bus for a virtio-scsi
 * controller (modern or legacy), negotiates features, sets up the command
 * virtqueue, then probes targets 0-1 with TEST UNIT READY + INQUIRY.
 * Hard disks are registered as "virtio0", CD-ROMs as "vio_cd0".
 * Returns 0 if at least one device was registered, non-zero otherwise. */
int virtio_scsi_init(void);

/* Register the VirtIO-SCSI interrupt handler.  Call AFTER IDT_Init and
 * PIC_Init.  No-op if no device was initialised. */
void virtio_scsi_setup_irq(void);

/* Get the IRQ line assigned to the VirtIO-SCSI device, or -1 if none. */
int virtio_scsi_get_irq_line(void);

/* Return 1 if virtio_scsi_init() succeeded and the controller is active. */
int virtio_scsi_is_active(void);

/* Return 1 if a CD-ROM was registered as "vio_cd0". */
int virtio_scsi_has_cdrom(void);

/* Read sectors from the VirtIO-SCSI hard disk. */
int virtio_scsi_read(uint64_t sector, void *buffer, uint32_t num_sectors);

/* Write sectors to the VirtIO-SCSI hard disk. */
int virtio_scsi_write(uint64_t sector, const void *buffer, uint32_t num_sectors);

/* Get hard disk capacity in 512-byte sectors. */
uint64_t virtio_scsi_get_capacity(void);

#endif /* UAOS_VIRTIO_SCSI_H */
