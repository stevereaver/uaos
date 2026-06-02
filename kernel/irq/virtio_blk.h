/*
 * virtio_blk.h — UAOS VirtIO Block Device Driver Header
 */

#ifndef UAOS_VIRTIO_BLK_H
#define UAOS_VIRTIO_BLK_H

#include <stdint.h>

/* Initialize VirtIO block device driver */
int virtio_blk_init(void);

/* Register VirtIO interrupt handler (call after IDT_Init and PIC_Init) */
void virtio_blk_setup_irq(void);

/* Get the IRQ line assigned to the VirtIO device */
int virtio_blk_get_irq_line(void);

/* Read sectors from VirtIO block device */
int virtio_blk_read(uint64_t sector, void *buffer, uint32_t num_sectors);

/* Write sectors to VirtIO block device */
int virtio_blk_write(uint64_t sector, const void *buffer, uint32_t num_sectors);

/* Get device capacity in sectors */
uint64_t virtio_blk_get_capacity(void);

#endif /* UAOS_VIRTIO_BLK_H */
