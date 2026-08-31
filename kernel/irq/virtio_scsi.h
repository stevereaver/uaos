/*
 * virtio_scsi.h — UAOS VirtIO-SCSI Block Device Driver Header
 *
 * Supports both the modern (non-transitional) virtio 1.0+ transport used by
 * VirtualBox (PCI 1af4:1048) and the legacy virtio-scsi transport used by
 * older QEMU (PCI 1af4:1004).  The disk is registered as "virtio0" so the
 * existing partition/mount/assign boot flow works unchanged.
 */

#ifndef UAOS_VIRTIO_SCSI_H
#define UAOS_VIRTIO_SCSI_H

#include <stdint.h>

/* Initialize VirtIO-SCSI driver.  Scans the PCI bus for a virtio-scsi
 * controller (modern or legacy), negotiates features, sets up the command
 * virtqueue, performs READ CAPACITY, and registers a "virtio0" block device.
 * Returns 0 on success, non-zero if no device was found or init failed. */
int virtio_scsi_init(void);

/* Register the VirtIO-SCSI interrupt handler.  Call AFTER IDT_Init and
 * PIC_Init.  No-op if no device was initialised. */
void virtio_scsi_setup_irq(void);

/* Get the IRQ line assigned to the VirtIO-SCSI device, or -1 if none. */
int virtio_scsi_get_irq_line(void);

/* Return 1 if virtio_scsi_init() succeeded and owns the virtio0 device. */
int virtio_scsi_is_active(void);

/* Read sectors from the VirtIO-SCSI disk. */
int virtio_scsi_read(uint64_t sector, void *buffer, uint32_t num_sectors);

/* Write sectors to the VirtIO-SCSI disk. */
int virtio_scsi_write(uint64_t sector, const void *buffer, uint32_t num_sectors);

/* Get device capacity in 512-byte sectors. */
uint64_t virtio_scsi_get_capacity(void);

#endif /* UAOS_VIRTIO_SCSI_H */
